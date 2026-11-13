#include "platform/tc002/wake/WakeListener.h"

#include <algorithm>
#include <array>
#include <utility>

#include "platform/tc002/contract/SupervisorProtocol.h"
#include "platform/tc002/wake/ModelFiles.h"
#include "platform/tc002/wake/WakeDetector.h"
#include "system/Log.h"

namespace awtrix::tc002::wake {
namespace {
static_assert(kMicrophonePcmRate == WakeFrontend::kSampleRate, "the detector takes the microphone's PCM as it comes");
constexpr uint32_t kCooloffMs = 1000, kWarmupMs = 1000;
constexpr std::array<int64_t, 5> kBackoffMs{1000, 2000, 5000, 10000, 30000};

std::unique_ptr<WakeDetector> readDetector(const std::string& modelPath, std::string& error) {
  ModelFiles files;
  if (!readModelFiles(modelPath, files, error)) return nullptr;
  auto detector = std::make_unique<WakeDetector>();
  if (!detector->load(files.model, error)) return nullptr;
  if (!detector->configure({static_cast<float>(files.cutoff), files.window, kCooloffMs, kWarmupMs}, error)) {
    error = files.settingsPath + ": " + error;
    return nullptr;
  }
  return detector;
}

std::string stem(const std::string& path) {
  const auto slash = path.rfind('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const auto dot = name.rfind('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

}

std::unique_ptr<WakeListener> WakeListener::open(const std::string& modelPath, MicrophonePort microphone,
                                                 std::unique_ptr<posix::Executor> worker, std::string& error) {
  if (!readDetector(modelPath, error)) return nullptr;
  std::unique_ptr<WakeListener> listener(new WakeListener(std::move(microphone), std::move(worker)));
  listener->bundle_ = {stem(modelPath), modelPath, WakeSensitivity::Normal, 0};
  listener->bundleEnabled_ = true;
  return listener;
}

std::unique_ptr<WakeListener> WakeListener::create(MicrophonePort microphone, std::unique_ptr<posix::Executor> worker) {
  return std::unique_ptr<WakeListener>(new WakeListener(std::move(microphone), std::move(worker)));
}

WakeListener::WakeListener(MicrophonePort microphone, std::unique_ptr<posix::Executor> worker)
    : microphone_(std::move(microphone)), worker_(std::move(worker)) {}

WakeListener::~WakeListener() {
  cancel();
  worker_.reset();
}

void WakeListener::cancel() { microphone_.cancel(); }

void WakeListener::setBundle(bool enabled, WakeSensitivity level) {
  if (bundle_.path.empty()) return;
  if (enabled != bundleEnabled_) {
    bundleEnabled_ = enabled;
    changed_ = true;
  }
  if (level == bundle_.sensitivity) return;
  bundle_.sensitivity = level;
  // A level reaches a loaded detector with the next block, without reading the model again.
  for (Loaded& model : loaded_)
    if (model.spec.name == bundle_.name && model.spec.path == bundle_.path) model.spec.sensitivity = level;
  if (!loaded_.empty()) adopt();
}

void WakeListener::setModels(std::vector<WakeModelSpec> models) {
  const auto same = [](const WakeModelSpec& a, const WakeModelSpec& b) {
    return a.name == b.name && a.path == b.path && a.version == b.version;
  };
  bool levelsOnly = models.size() == models_.size();
  for (std::size_t i = 0; levelsOnly && i < models.size(); ++i) levelsOnly = same(models[i], models_[i]);
  models_ = std::move(models);
  if (!levelsOnly) {
    changed_ = true;
    return;
  }
  for (Loaded& loaded : loaded_)
    for (const WakeModelSpec& model : models_)
      if (same(loaded.spec, model)) loaded.spec.sensitivity = model.sensitivity;
  if (!loaded_.empty()) adopt();
}

bool WakeListener::hasModels() const { return (bundleEnabled_ && !bundle_.path.empty()) || !models_.empty(); }

bool WakeListener::loaded(const std::string& name) const {
  return std::any_of(loaded_.begin(), loaded_.end(), [&](const Loaded& model) { return model.spec.name == name; });
}

bool WakeListener::scores(std::vector<WakeScore>& out, uint32_t& position) const {
  std::lock_guard<std::mutex> lock(mutex_);
  out = scores_;
  position = scorePosition_;
  return !out.empty();
}

std::vector<WakeModelSpec> WakeListener::wanted() const {
  std::vector<WakeModelSpec> specs;
  if (bundleEnabled_ && !bundle_.path.empty()) specs.push_back(bundle_);
  for (const WakeModelSpec& model : models_)
    if (model.name != bundle_.name || bundle_.path.empty()) specs.push_back(model);
  return specs;
}

std::vector<WakeHeard> WakeListener::poll(int64_t now, Listen listen) {
  std::vector<WakeHeard> heard;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    heard.swap(heard_);
  }
  if (!hasModels()) listen = Listen::Off;
  muted_ = listen != Listen::On;
  if (listen != Listen::Off) wantedAt_ = now;
  if (microphone_.phase() == MicrophoneClient::Phase::Idle) {
    if (listen == Listen::Off) {
      if (now - wantedAt_ >= kModelOffMs) unloadDetectors();
    } else if (now >= retryAt_ && loadDetectors(now)) {
      microphone_.open(now);
    }
  } else if (listen == Listen::Off) {
    microphone_.stop(now);
  } else if ((changed_ || !failedModels_.empty()) && now >= retryAt_) {
    loadDetectors(now);
  }
  apply(microphone_.tick(now), now);
  if (listen == Listen::Off) return {};
  for (WakeHeard& h : heard) {
    h.muted = h.muted || muted_;
    if (!h.muted) logf("wake word: heard %s (score %.2f)", h.name.c_str(), static_cast<double>(h.score));
  }
  return heard;
}

std::optional<WakeHeard> WakeListener::tick(int64_t now, Listen listen) {
  std::optional<WakeHeard> last;
  for (WakeHeard& h : poll(now, listen))
    if (!h.muted) last = std::move(h);
  return last;
}

// Keeps a loaded detector whose files did not change; reads the others. A model that cannot be
// read is left out and tried again every 30 s; the others listen meanwhile.
bool WakeListener::loadDetectors(int64_t now) {
  if (!changed_ && failedModels_.empty() && !loaded_.empty()) return true;
  std::vector<Loaded> next;
  std::vector<std::string> failed;
  for (const WakeModelSpec& spec : wanted()) {
    const auto kept = std::find_if(loaded_.begin(), loaded_.end(), [&](const Loaded& model) {
      return model.spec.name == spec.name && model.spec.path == spec.path && model.spec.version == spec.version;
    });
    if (kept != loaded_.end()) {
      next.push_back({spec, kept->detector});
      continue;
    }
    std::string error;
    std::unique_ptr<WakeDetector> detector = readDetector(spec.path, error);
    if (!detector) {
      if (std::find(loadErrors_.begin(), loadErrors_.end(), error) == loadErrors_.end()) {
        logf("wake word: %s", error.c_str());
        loadErrors_.push_back(error);
      }
      failed.push_back(spec.name);
      continue;
    }
    next.push_back({spec, std::shared_ptr<WakeDetector>(std::move(detector))});
  }
  changed_ = false;
  failedModels_ = std::move(failed);
  if (failedModels_.empty()) loadErrors_.clear();
  else retryAt_ = now + kBackoffMs.back();
  if (next.empty()) {
    loaded_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!draining_ && blocks_.empty()) running_.clear();
    pending_.emplace();
    return false;
  }
  loaded_ = std::move(next);
  adopt();
  return true;
}

void WakeListener::unloadDetectors() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (draining_ || !blocks_.empty()) return;
  loaded_.clear();
  running_.clear();
  pending_.reset();
  scores_.clear();
  spareSamples_.clear();
  changed_ = true;
}

// Hands the loaded detectors to the worker, which takes them before its next block.
void WakeListener::adopt() {
  std::vector<Running> running;
  running.reserve(loaded_.size());
  for (const Loaded& model : loaded_) running.push_back({model.spec.name, model.detector, model.spec.sensitivity, true});
  std::lock_guard<std::mutex> lock(mutex_);
  pending_ = std::move(running);
}

void WakeListener::receive(const StreamEvent& event, int64_t now) {
  const MicrophoneClient::Report kind = microphone_.receive(event, now);
  switch (kind) {
    case MicrophoneClient::Report::Gap:
      restart(event.firstSample);
      [[fallthrough]];
    case MicrophoneClient::Report::Audio:
      return queueAudio(event);
    case MicrophoneClient::Report::None:
    case MicrophoneClient::Report::Available:
    case MicrophoneClient::Report::Unavailable:
    case MicrophoneClient::Report::Started:
    case MicrophoneClient::Report::Ended:
    case MicrophoneClient::Report::Stopped:
      return apply(kind, now);
  }
}

void WakeListener::apply(MicrophoneClient::Report report, int64_t now) {
  switch (report) {
    case MicrophoneClient::Report::Unavailable:
      return unavailable(now);
    case MicrophoneClient::Report::Started:
      failures_ = 0;
      error_.clear();
      return restart(0);
    case MicrophoneClient::Report::Ended:
      return failed(now);
    case MicrophoneClient::Report::Stopped:
      return restart(0);
    case MicrophoneClient::Report::None:
    case MicrophoneClient::Report::Available:
    case MicrophoneClient::Report::Audio:
    case MicrophoneClient::Report::Gap:
      return;
  }
}

void WakeListener::unavailable(int64_t now) {
  if (error_.empty()) logf("wake word: %s", kStreamUnavailable);
  error_ = "unavailable";
  retryAt_ = now + kBackoffMs.back();
}

void WakeListener::failed(int64_t now) {
  restart(0);
  const std::string& error = microphone_.error();
  if (error != error_) logf("wake word: microphone stream ended%s%s", error.empty() ? "" : ": ", error.c_str());
  error_ = error;
  retryAt_ = now + kBackoffMs[std::min<std::size_t>(failures_++, kBackoffMs.size() - 1)];
}

void WakeListener::restart(uint32_t origin) { queue({{}, true, origin}); }

void WakeListener::queueAudio(const StreamEvent& event) {
  std::vector<int16_t> samples;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!spareSamples_.empty()) {
      samples = std::move(spareSamples_.back());
      spareSamples_.pop_back();
    }
  }
  samples = event.samples;
  queue({std::move(samples), false, event.firstSample, muted_});
}

void WakeListener::queue(Block block) {
  bool idle = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (blocks_.size() >= kQueueBlocks) {
      blocks_.clear();
      blocks_.push_back({{}, true, block.firstSample});
    }
    blocks_.push_back(std::move(block));
    idle = !std::exchange(draining_, true);
  }
  if (idle && !worker_->post([this] { drain(); })) {
    std::lock_guard<std::mutex> lock(mutex_);
    draining_ = false;
  }
}

void WakeListener::drain() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!blocks_.empty()) {
    if (pending_) {
      // Detectors the worker did not run yet start from a clean state; the frontend runs on.
      for (Running& next : *pending_)
        next.fresh = std::none_of(running_.begin(), running_.end(),
                                  [&](const Running& old) { return old.detector == next.detector; });
      running_ = std::move(*pending_);
      pending_.reset();
    }
    Block block = std::move(blocks_.front());
    blocks_.pop_front();
    lock.unlock();
    std::vector<WakeHeard> heard;
    if (block.reset) {
      frontend_.reset();
      position_ = 0;
      origin_ = block.firstSample;
      for (Running& model : running_) {
        model.detector->reset();
        model.fresh = false;
      }
    } else {
      detect(block, heard);
      scratch_.resize(running_.size());
      for (std::size_t i = 0; i < running_.size(); ++i) {
        scratch_[i].name = running_[i].name;
        scratch_[i].score = running_[i].detector->score();
      }
    }
    lock.lock();
    if (!block.reset) {
      heard_.insert(heard_.end(), std::make_move_iterator(heard.begin()), std::make_move_iterator(heard.end()));
      scores_.swap(scratch_);
      scorePosition_ = origin_ + static_cast<uint32_t>(position_);
      if (spareSamples_.size() < kQueueBlocks) spareSamples_.push_back(std::move(block.samples));
    }
  }
  draining_ = false;
}

void WakeListener::detect(const Block& block, std::vector<WakeHeard>& heard) {
  for (Running& model : running_) {
    if (model.fresh) {
      model.detector->reset();
      model.fresh = false;
    }
    model.detector->setSensitivity(model.sensitivity);
  }
  const int16_t* samples = block.samples.data();
  std::size_t count = block.samples.size();
  WakeFrontend::Frame frame;
  for (;;) {
    const std::size_t before = count;
    const bool ready = frontend_.next(samples, count, frame);
    position_ += before - count;
    if (!ready) break;
    for (Running& model : running_)
      if (const std::optional<WakeEvent> event = model.detector->push(frame, position_))
        heard.push_back({model.name, event->score, origin_ + static_cast<uint32_t>(event->sample), block.muted});
  }
}

}
