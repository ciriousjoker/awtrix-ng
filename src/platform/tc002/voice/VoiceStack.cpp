#include "platform/tc002/voice/VoiceStack.h"

#include <utility>

#include "core/api/JsonWriter.h"
#include "core/render/Canvas.h"
#include "platform/linux/LinuxMemory.h"
#include "platform/posix/WorkerThread.h"
#include "platform/tc002/contract/tc002_layout.h"
#include "platform/tc002/voice/ClipStore.h"
#include "system/Log.h"
#include "system/MonotonicClock.h"

namespace awtrix::tc002::voice {
namespace {
constexpr uint32_t kPrivacyRed = 0xff0000;
}

VoiceStack::VoiceStack(const Setup& setup)
    : input_([send = setup.send, supervised = setup.supervised](std::string request) {
        return supervised && send(std::move(request));
      }),
      hub_(setup.send, [this](const StreamEvent& block, int64_t now) {
        input_.streamed(block.samples.data(), block.samples.size(), block.firstSample, now);
      }) {
  if (!setup.supervised) return;
  ClipStore::removeLeftovers(TC002_VOLATILE_DIR);
  if (!setup.speaker) return;
  wakeMicrophone_ = hub_.attach([this](const StreamEvent& e, int64_t now) {
    if (wake_) wake_->receive(e, now);
  });
  if (!setup.wakeWordModel.empty()) {
    std::string error;
    wake_ = wake::WakeListener::open(setup.wakeWordModel, wakeMicrophone_.port(),
                                     std::make_unique<posix::WorkerThread>(), error);
    if (!wake_) logf("wake word: %s", error.c_str());
  }
  wakeWordAvailable_ = wake_ != nullptr;
  if (!wake_) wake_ = wake::WakeListener::create(wakeMicrophone_.port(), std::make_unique<posix::WorkerThread>());
  wakeModels_ = std::make_unique<wake::WakeModelStore>(
      setup.wakeDirectory,
      wake::WakeModelStore::Bundle{
          wakeWordAvailable_ ? setup.wakeWordModel : std::string(),
          [this] { return runtime_ && runtime_->wakeWord(); },
          [this] { return runtime_ ? runtime_->wakeSensitivity() : wake::WakeSensitivity::Normal; },
          [this](std::optional<bool> enabled, std::optional<wake::WakeSensitivity> level) {
            return setBundle(enabled, level);
          }},
      setup.storageReserveBytes);
  wakeApi_.emplace(*wakeModels_, [this](const std::string& name) { return wake_ && wake_->loaded(name); });
  wallClockMs_ = setup.wallClockMs;
  streamMicrophone_ = hub_.attach([this](const StreamEvent& e, int64_t now) {
    if (microphoneStream_) microphoneStream_->receive(e, now);
  });
  microphoneStream_.emplace(streamMicrophone_.port());
  speaker_.emplace(*setup.speaker);
  voiceMicrophone_ = hub_.attach([this](const StreamEvent& e, int64_t now) {
    if (runtime_) runtime_->receive(e, now);
  });
  runtime_ = std::make_unique<VoiceRuntime>(setup.configDirectory, *speaker_, voiceMicrophone_.port());
  api_.emplace(*runtime_, wakeWordAvailable_,
               VoiceApi::Hooks{[this] { return router_->apps(); },
                               [this] { return start(VoiceTrigger::Api); },
                               [this] { return router_->listening(); },
                               [this] { return router_->appProblem(); }});
}

VoiceScripting* VoiceStack::composeScripting(ScriptSetup setup) {
  if (!runtime_) return nullptr;
  scriptMicrophone_ = hub_.attach([this](const StreamEvent& e, int64_t now) {
    if (scripting_) scripting_->receive(e, now);
  });
  scripting_ = std::make_unique<VoiceScripting>(VoiceScripting::Services{
      scriptMicrophone_.port(),
      [this](MicrophoneMark mark) { scriptMicrophone_.startFrom(mark); },
      [this](MicrophoneMark mark) { return hub_.live(mark); },
      &*speaker_, [this] { return runtime_->busy(); }, std::move(setup.visible),
      [] { return monotonicMs(); }, TC002_VOLATILE_DIR, readMemAvailable, std::move(setup.voiceActivityModel),
      std::move(setup.fillRequest)});
  return scripting_.get();
}

wake::WakeScripting* VoiceStack::composeWakeScripting(std::function<std::vector<std::string>()> apps,
                                                     std::function<bool(const std::string&, std::string&)> source) {
  if (!wake_) return nullptr;
  wakeScripting_ = std::make_unique<wake::WakeScripting>(
      wake::WakeScripting::Services{std::move(apps), std::move(source), [this] { return listeningModels(); }});
  return wakeScripting_.get();
}

void VoiceStack::composeRouter(AssistantRouter::Ports ports) {
  if (!runtime_) return;
  ports.heard = [this](const wake::WakeHeard& h) { heard(h); };
  AssistantRouter::Module& module = scripting_ ? static_cast<AssistantRouter::Module&>(*scripting_) : noModule_;
  router_.emplace(std::move(ports),
                  AssistantRouter::Parts{*runtime_, module, gesture_, hub_, wake_.get(), wakeMicrophone_});
}

void VoiceStack::receive(const SupervisorMessage& message, int64_t now) {
  if (message.type == MessageType::MicrophonePcm) input_.receive(message.microphonePcm, now);
  if (message.type == MessageType::MicrophoneStreamEvent) hub_.receive(message.streamEvent, now);
}

void VoiceStack::tick(int64_t now, bool allowed, bool knobAllowed) {
  hub_.tick(now);
  if (!knobAllowed) gesture_.cancel();
  if (runtime_) {
    runtime_->tick(now, allowed);
    if (scripting_) scripting_->update(now);
    tickWake(now);
    router_->tick(now, knobAllowed);
  }
  if (microphoneStream_) microphoneStream_->tick(now);
  input_.setStreaming(hub_.active());
}

// Hands the stored models to the listener when they changed, and the window scores to the
// event stream ten times a second while a client wants them.
void VoiceStack::tickWake(int64_t now) {
  if (!wake_ || !wakeModels_) return;
  if (wakeModels_->revision() != wakeRevision_) {
    wakeRevision_ = wakeModels_->revision();
    wake_->setModels(wakeModels_->enabledModels());
  }
  if (!wakeApi_->wantsScores() || now - scoresAt_ < wake::WakeApi::kScoresMs) return;
  scoresAt_ = now;
  std::vector<wake::WakeScore> scores;
  uint32_t position = 0;
  if (wake_->scores(scores, position)) wakeApi_->scores(position, scores);
}

void VoiceStack::heard(const wake::WakeHeard& h) {
  if (wakeApi_) wakeApi_->detection(h, wallClockMs_ ? wallClockMs_() : -1);
  if (!h.muted && wakeScripting_) wakeScripting_->heard(h.name, h.score);
}

std::vector<std::string> VoiceStack::listeningModels() const {
  std::vector<std::string> names;
  if (!wakeModels_) return names;
  for (const wake::WakeModelEntry& entry : wakeModels_->list())
    if (entry.enabled) names.push_back(entry.name);
  return names;
}

bool VoiceStack::setBundle(std::optional<bool> enabled, std::optional<wake::WakeSensitivity> level) {
  if (!runtime_) return false;
  std::string json;
  api::JsonWriter writer(json);
  writer.beginObject();
  if (enabled) writer.member("wakeWord", *enabled);
  if (level) writer.member("wakeSensitivity", std::string(wake::wakeSensitivityName(*level)));
  writer.endObject();
  ConfigStore::Rejection rejection;
  return runtime_->updateConfig(json, rejection);
}

void VoiceStack::draw(Canvas& canvas, int64_t now) {
  if (runtime_) runtime_->draw(canvas, now);
}

void VoiceStack::drawPrivacy(Canvas& canvas) const {
  if (microphoneStream_ && microphoneStream_->active()) canvas.setPixel(canvas.width() - 1, 0, kPrivacyRed);
}

bool VoiceStack::handle(const httplib::Request& request, httplib::Response& response) {
  if (wakeApi_ && wakeApi_->handle(request, response)) return true;
  if (microphoneStream_ && microphoneStream_->handle(request, response)) return true;
  return api_ && api_->handle(request, response);
}

bool VoiceStack::start(VoiceTrigger trigger) { return router_->start(trigger, monotonicMs()); }

void VoiceStack::stop() {
  if (wakeApi_) wakeApi_->stop();
  if (microphoneStream_) microphoneStream_->stop();
  if (runtime_) runtime_->cancel();
  if (wake_) wake_->cancel();
  hub_.close();
}

void VoiceStack::stopTransfers() {
  if (scripting_) scripting_->shutdown();
}

bool VoiceStack::eraseConfig() {
  const bool models = !wakeModels_ || wakeModels_->eraseAll();
  return (!runtime_ || runtime_->eraseConfig()) && models;
}

}
