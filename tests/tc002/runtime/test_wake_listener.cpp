#include "../../support.h"
#include "../../ManualExecutor.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "platform/tc002/wake/WakeDetector.h"
#include "platform/tc002/wake/WakeFrontend.h"
#include "platform/tc002/wake/WakeListener.h"

using namespace awtrix;
using awtrix::test::check;
using awtrix::test::require;
using tc002::StreamControl;
using tc002::StreamEvent;
using tc002::wake::WakeHeard;
using tc002::wake::WakeListener;
using tc002::wake::WakeSensitivity;
using Listen = WakeListener::Listen;

namespace {
namespace fs = std::filesystem;

constexpr uint32_t kSecond = tc002::wake::WakeFrontend::kSampleRate;
constexpr int64_t kFrameMs = 30;

void writeFile(const fs::path& path, const std::string& bytes) { std::ofstream(path, std::ios::binary) << bytes; }

struct Rig {
  std::vector<StreamControl> sent;
  test::ManualExecutor* worker = nullptr;
  std::unique_ptr<WakeListener> listener;
  std::string error;
  int64_t now = 1000;
  uint32_t position = 0;

  explicit Rig(const fs::path& model) {
    auto executor = std::make_unique<test::ManualExecutor>();
    worker = executor.get();
    listener = WakeListener::open(model.string(), [this](StreamControl control) {
      sent.push_back(control);
      return true;
    }, std::move(executor), error);
  }
  const StreamControl& last() const {
    require(!sent.empty(), "the listener has sent a control");
    return sent.back();
  }
  StreamEvent event(StreamEvent::Kind kind, bool available = true) const {
    StreamEvent e;
    e.epoch = last().epoch;
    e.kind = kind;
    e.available = available;
    return e;
  }
  void answer(StreamEvent::Kind kind, bool available = true) { listener->receive(event(kind, available), now); }
  void fail(const char* error) {
    StreamEvent e = event(StreamEvent::Kind::Ended);
    e.error = error;
    listener->receive(e, now);
  }
  int listen() {
    listener->tick(now, Listen::On);
    require(last().operation == StreamControl::Probe, "listening probes the microphone");
    answer(StreamEvent::Kind::Support);
    answer(StreamEvent::Kind::Started);
    position = 0;
    return last().epoch;
  }
  void block() {
    StreamEvent e = event(StreamEvent::Kind::Audio);
    e.firstSample = position;
    e.samples.assign(tc002::kStreamBlockSamples, 0);
    position += tc002::kStreamBlockSamples;
    listener->receive(e, now);
  }
  std::vector<WakeHeard> feed(int blocks, Listen listen = Listen::On) {
    std::vector<WakeHeard> heard;
    for (int i = 0; i < blocks; ++i) {
      now += kFrameMs;
      block();
      worker->run();
      if (const auto h = listener->tick(now, listen)) heard.push_back(*h);
    }
    return heard;
  }
  unsigned count(StreamControl::Operation operation) const {
    unsigned n = 0;
    for (const auto& c : sent) n += c.operation == operation;
    return n;
  }
};

struct Fixture {
  fs::path dir, model, settings;

  explicit Fixture(const fs::path& shipped)
      : dir(fs::temp_directory_path() / ("wake-listener-" + std::to_string(::getpid()))),
        model(dir / "word.tflite"),
        settings(dir / "word.json") {
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy_file(shipped, model);
  }
  ~Fixture() { fs::remove_all(dir); }
  void firesAlways() const { writeFile(settings, R"({"ID":"word","probability_cutoff":0,"sliding_window_size":1})"); }
};

struct Firing : Rig {
  explicit Firing(const Fixture& f) : Rig((f.firesAlways(), f.model)) { require(listener != nullptr, error); }
};

std::string positions(const std::vector<WakeHeard>& heard) {
  std::string out;
  for (const WakeHeard& h : heard) out += std::to_string(h.sample) + " ";
  return out;
}

bool apartByTheCooloff(const std::vector<WakeHeard>& heard) {
  for (std::size_t i = 1; i < heard.size(); ++i)
    if (heard[i].sample - heard[i - 1].sample < kSecond) return false;
  return true;
}

void settingsAndModelsAreChecked(const Fixture& f) {
  Rig missing(f.model);
  check(!missing.listener && missing.error.find("word.json") != std::string::npos, "the settings file is required");
  writeFile(f.settings, R"({"probability_cutoff":0.9})");
  Rig partial(f.model);
  check(!partial.listener && partial.error.find("sliding_window_size") != std::string::npos,
        "the settings need cutoff and window");
  writeFile(f.settings, R"({"probability_cutoff":0.9,"sliding_window_size":0})");
  Rig zero(f.model);
  check(!zero.listener && zero.error == f.settings.string() + ": the window must be within 1..64",
        "a window of 0 is refused: " + zero.error);
  writeFile(f.settings, R"({"probability_cutoff":0.9,"sliding_window_size":-3})");
  Rig negative(f.model);
  check(!negative.listener && negative.error.find("window") != std::string::npos,
        "a negative window is refused: " + negative.error);
  writeFile(f.settings, R"({"probability_cutoff":1.5,"sliding_window_size":3})");
  Rig high(f.model);
  check(!high.listener && high.error.find("cutoff") != std::string::npos, "a cutoff above 1 is refused: " + high.error);
  writeFile(f.dir / "broken.tflite", "not a model");
  writeFile(f.dir / "broken.json", R"({"probability_cutoff":0.9,"sliding_window_size":3})");
  Rig broken(f.dir / "broken.tflite");
  check(!broken.listener && !broken.error.empty(), "a broken model is refused: " + broken.error);
  writeFile(f.settings,
            R"({"type":"micro","micro":{"probability_cutoff":0.5,"sliding_window_size":5,"feature_step_size":10}})");
  check(Rig(f.model).listener != nullptr, "an ESPHome manifest's settings under micro are read");
  writeFile(f.settings, R"({"micro":{"probability_cutoff":0.5,"sliding_window_size":5,"feature_step_size":20}})");
  Rig coarse(f.model);
  check(!coarse.listener && coarse.error.find("feature_step_size") != std::string::npos,
        "a model on another feature step is refused: " + coarse.error);
  writeFile(f.settings, R"({"micro":{"probability_cutoff":0.5}})");
  Rig nested(f.model);
  check(!nested.listener && nested.error.find("needs probability_cutoff and sliding_window_size") != std::string::npos,
        "an incomplete micro block is refused: " + nested.error);
}

void listensWhenAsked(const Fixture& f) {
  Firing rig(f);
  check(!rig.listener->tick(rig.now, Listen::Off) && rig.sent.empty(), "not wanted: no microphone traffic");
  rig.listener->tick(rig.now, Listen::On);
  check(rig.sent.size() == 1 && rig.last().operation == StreamControl::Probe, "wanted: it probes first");
  rig.answer(StreamEvent::Kind::Support);
  check(rig.sent.size() == 2 && rig.last().operation == StreamControl::Start, "an available stream starts");
  rig.answer(StreamEvent::Kind::Started);
  const auto heard = rig.feed(30);
  check(heard.empty(), "nothing fires within the first second: " + positions(heard));
  const unsigned keepalives = rig.count(StreamControl::Keepalive);
  check(keepalives == 30 * kFrameMs / tc002::kStreamKeepaliveMs,
        "a keepalive every 150 ms: " + std::to_string(keepalives));
}

void detectionsKeepTheirCooloff(const Fixture& f) {
  Firing rig(f);
  rig.listen();
  const auto heard = rig.feed(110);
  check(heard.size() >= 2 && heard[0].sample >= kSecond && heard.back().sample <= rig.position &&
            apartByTheCooloff(heard),
        "detections come after the warm-up, a cool-off apart, at their place in the stream: " + positions(heard));
}

void aJumpStartsTheWarmUpOver(const Fixture& f) {
  Firing rig(f);
  rig.listen();
  rig.feed(40);
  rig.position += 96;
  const uint32_t jumped = rig.position;
  const auto during = rig.feed(30);
  check(during.empty(), "a jump in sample position starts the warm-up over: " + positions(during));
  const auto after = rig.feed(40);
  check(!after.empty() && after[0].sample >= jumped + kSecond && after[0].sample <= rig.position,
        "after a jump, positions continue in the listener's stream: " + positions(after));
}

void aFullQueueStartsTheWarmUpOver(const Fixture& f) {
  Firing rig(f);
  rig.listen();
  rig.worker->run();
  for (std::size_t i = 0; i < WakeListener::kQueueBlocks; ++i) rig.block();
  const uint32_t dropped = rig.position;
  rig.block();
  check(rig.worker->pending() == 1, "the blocks wait for one run of the worker");
  rig.worker->run();
  const auto heard = rig.feed(70);
  check(!heard.empty() && heard[0].sample >= dropped + kSecond,
        "a block that finds the queue full drops the waiting ones and starts the detector over from itself: " +
            positions(heard));
}

void aWorkerThatRefusesIsAskedAgain(const Fixture& f) {
  Firing rig(f);
  rig.worker->refusing = true;
  rig.listen();
  rig.block();
  check(rig.worker->pending() == 0, "a worker that cannot start leaves the blocks waiting");
  rig.worker->refusing = false;
  rig.block();
  check(rig.worker->pending() == 1, "the next block asks it again");
  rig.worker->run();
  check(!rig.feed(70).empty(), "and the listener hears as before");
}

void mutedKeepsTheStream(const Fixture& f) {
  Firing rig(f);
  rig.listen();
  rig.feed(40);
  const std::size_t controls = rig.sent.size();
  const auto muted = rig.feed(60, Listen::Muted);
  check(muted.empty(), "muted: nothing is reported: " + positions(muted));
  unsigned others = 0;
  for (std::size_t i = controls; i < rig.sent.size(); ++i) others += rig.sent[i].operation != StreamControl::Keepalive;
  check(others == 0 && rig.sent.size() > controls, "muted: the stream stays and is kept alive");
  const uint32_t unmuted = rig.position;
  const auto resumed = rig.feed(40);
  check(!resumed.empty() && resumed[0].sample > unmuted && rig.count(StreamControl::Probe) == 1,
        "unmuted: detections resume on the same stream, none from audio heard while muted: " + positions(resumed));
}

void offStopsTheStream(const Fixture& f) {
  Firing rig(f);
  const int epoch = rig.listen();
  for (int i = 0; i < 40; ++i) {
    rig.now += kFrameMs;
    rig.block();
  }
  rig.worker->run();
  check(!rig.listener->tick(rig.now, Listen::Off), "Voice busy: a pending detection is dropped");
  check(rig.last().operation == StreamControl::Stop && rig.last().epoch == epoch, "Voice busy: the stream is stopped");
  rig.answer(StreamEvent::Kind::Ended);
  const std::size_t before = rig.sent.size();
  rig.listener->tick(rig.now, Listen::Off);
  check(rig.sent.size() == before, "nothing starts while Voice is busy");
  rig.listener->tick(rig.now, Listen::On);
  check(rig.last().operation == StreamControl::Probe && rig.last().epoch != epoch,
        "listening resumes with a new epoch after Voice");
}

void theModelLeavesMemoryWhileOff(const Fixture& f) {
  const fs::path moved = f.dir / "moved.tflite";
  const auto withoutTheFile = [&](auto act) {
    fs::rename(f.model, moved);
    act();
    fs::rename(moved, f.model);
  };
  Firing rig(f);
  rig.listener->tick(rig.now, Listen::On);
  rig.listener->cancel();
  rig.sent.clear();
  rig.now += WakeListener::kModelOffMs - 1;
  rig.listener->tick(rig.now, Listen::Off);
  withoutTheFile([&] {
    rig.listener->tick(rig.now, Listen::On);
    check(!rig.sent.empty() && rig.last().operation == StreamControl::Probe,
          "off for a shorter time it keeps the model, the file is not read");
  });
  rig.listener->cancel();
  rig.sent.clear();

  rig.now += WakeListener::kModelOffMs;
  rig.listener->tick(rig.now, Listen::Off);
  withoutTheFile([&] {
    rig.listener->tick(rig.now, Listen::On);
    check(rig.sent.empty(), "after a minute off the model is gone, and without its file nothing starts");
    rig.now += 30000;
    rig.listener->tick(rig.now, Listen::On);
    check(rig.sent.empty(), "it stays that way");
  });
  rig.now += 30000;
  rig.listener->tick(rig.now, Listen::On);
  check(!rig.sent.empty() && rig.last().operation == StreamControl::Probe, "with the file back it loads and listens");
  rig.answer(StreamEvent::Kind::Support);
  rig.answer(StreamEvent::Kind::Started);
  rig.position = 0;
  rig.feed(40);
  check(!rig.feed(40).empty(), "and hears with the new model");

  Firing pending(f);
  pending.listen();
  pending.block();
  pending.listener->cancel();
  pending.now += 2 * WakeListener::kModelOffMs;
  pending.listener->tick(pending.now, Listen::Off);
  pending.worker->run();
  check(pending.worker->pending() == 0, "a block still queued finds the model");
}

void openingKeepsNoModel(const Fixture& f) {
  f.firesAlways();
  const fs::path moved = f.dir / "moved.tflite";
  Rig rig(f.model);
  require(rig.listener != nullptr, rig.error);
  fs::rename(f.model, moved);
  rig.listener->tick(rig.now, Listen::On);
  check(rig.sent.empty(), "the model checked at the start is gone: without its file nothing starts");
  rig.listener->tick(rig.now, Listen::Off);
  fs::rename(moved, f.model);
  rig.now += 30000;
  rig.listen();
  check(rig.feed(40).size() + rig.feed(40).size() > 0, "with the file back it loads and hears");

  Rig off(f.model);
  require(off.listener != nullptr, off.error);
  off.listener->tick(off.now, Listen::Off);
  off.now += 2 * WakeListener::kModelOffMs;
  off.listener->tick(off.now, Listen::Off);
  check(off.sent.empty(), "with the wake word off the microphone stays off");
  off.listener->cancel();
}

void aModelItCannotListenWithIsRefused(const Fixture& f) {
  f.firesAlways();
  const std::string good = [&] {
    std::ifstream in(f.model, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }();
  writeFile(f.model, "not a model");
  Rig damaged(f.model);
  check(damaged.listener == nullptr && !damaged.error.empty(), "a damaged model opens nothing: " + damaged.error);
  writeFile(f.model, "");
  Rig empty(f.model);
  check(empty.listener == nullptr, "nor does an empty file");
  writeFile(f.model, good);
}

void theLevelApplies(const Fixture& f) {
  writeFile(f.settings, R"({"probability_cutoff":0.44,"sliding_window_size":1})");
  Rig rig(f.model);
  require(rig.listener != nullptr, rig.error);
  rig.listen();
  check(rig.feed(100).empty(), "silence stays under the model's cutoff");
  rig.listener->setSensitivity(WakeSensitivity::High);
  check(!rig.feed(60).empty(), "on high it fires, the cutoff of this model drops to 0");
  check(rig.count(StreamControl::Probe) == 1 && rig.count(StreamControl::Stop) == 0, "on the same stream");
  rig.listener->setSensitivity(WakeSensitivity::Normal);
  check(rig.feed(60).empty(), "back on normal it stays silent");
  rig.listener->setSensitivity(WakeSensitivity::High);
  rig.listener->cancel();
  rig.now += WakeListener::kModelOffMs;
  rig.listener->tick(rig.now, Listen::Off);
  rig.listen();
  check(!rig.feed(60).empty(), "a model loaded again takes the level");
}

void aStopWithoutAnswerEnds(const Fixture& f) {
  Firing rig(f);
  const int epoch = rig.listen();
  rig.listener->tick(rig.now, Listen::Off);
  require(rig.last().operation == StreamControl::Stop, "the stream is asked to stop");
  rig.listener->tick(rig.now + tc002::kStreamAnswerMs - 1, Listen::On);
  check(rig.last().operation == StreamControl::Stop, "a stop waits for its answer");
  rig.now += tc002::kStreamAnswerMs;
  rig.listener->tick(rig.now, Listen::On);
  check(rig.last().operation == StreamControl::Stop, "after 2 s the stop counts as done");
  rig.listener->tick(rig.now, Listen::On);
  check(rig.last().operation == StreamControl::Probe && rig.last().epoch != epoch,
        "and the next frame probes again");
}

void failuresBackOff(const Fixture& f) {
  Firing rig(f);
  const auto retriesAfter = [&rig](int64_t ms) {
    const std::size_t before = rig.sent.size();
    rig.listener->tick(rig.now + ms - 1, Listen::On);
    const bool waited = rig.sent.size() == before;
    rig.now += ms;
    rig.listener->tick(rig.now, Listen::On);
    return waited && rig.sent.size() == before + 1 && rig.last().operation == StreamControl::Probe;
  };
  rig.listener->tick(rig.now, Listen::On);
  rig.fail("microphone UART frame lost or corrupt");
  bool steps = true;
  for (const int64_t ms : {1000, 2000, 5000, 10000, 30000}) {
    steps = steps && retriesAfter(ms);
    rig.fail("microphone UART frame lost or corrupt");
  }
  check(steps && retriesAfter(30000), "failed streams retry after 1, 2, 5, 10 and then every 30 s");
  rig.answer(StreamEvent::Kind::Support);
  rig.answer(StreamEvent::Kind::Started);
  rig.fail("microphone UART frame lost or corrupt");
  check(retriesAfter(1000), "a stream that started resets the back-off to 1 s");
  rig.now += tc002::kStreamAnswerMs;
  rig.listener->tick(rig.now, Listen::On);
  check(retriesAfter(2000), "a probe without an answer counts as a failure");
}

void anUnavailableStreamWaits(const Fixture& f) {
  Firing rig(f);
  rig.listener->tick(rig.now, Listen::On);
  const int epoch = rig.last().epoch;
  rig.answer(StreamEvent::Kind::Support, false);
  StreamEvent late = rig.event(StreamEvent::Kind::Started);
  late.epoch = epoch;
  rig.listener->receive(late, rig.now);
  const std::size_t before = rig.sent.size();
  rig.listener->tick(rig.now + 29999, Listen::On);
  check(rig.sent.size() == before, "late events of an old epoch are ignored; it asks again later");
  rig.listener->tick(rig.now + 30000, Listen::On);
  check(rig.sent.size() == before + 1 && rig.last().operation == StreamControl::Probe,
        "an MCU without the stream is asked again after 30 s");
}

void theShippedWakeWordOpens(const fs::path& model) {
  Rig rig(model);
  check(rig.listener != nullptr, "the wake word the release ships opens with its settings: " + rig.error);
}

void cancelAndWithdraw(const Fixture& f) {
  Firing rig(f);
  rig.listen();
  rig.listener->cancel();
  check(rig.last().operation == StreamControl::Stop, "cancel stops the stream");
  rig.listener->tick(rig.now, Listen::On);
  const int probing = rig.last().epoch;
  check(rig.last().operation == StreamControl::Probe, "listening again probes");
  rig.listener->tick(rig.now, Listen::Off);
  check(rig.last().operation == StreamControl::Stop && rig.last().epoch == probing,
        "a probe no longer wanted is withdrawn");
  rig.answer(StreamEvent::Kind::Support);
  check(rig.last().operation == StreamControl::Stop, "and its late answer starts nothing");
}

std::vector<WakeHeard> pollBlocks(Rig& rig, int blocks, Listen listen = Listen::On) {
  std::vector<WakeHeard> heard;
  for (int i = 0; i < blocks; ++i) {
    rig.now += kFrameMs;
    rig.block();
    rig.worker->run();
    for (WakeHeard& h : rig.listener->poll(rig.now, listen)) heard.push_back(std::move(h));
  }
  return heard;
}

unsigned heardFrom(const std::vector<WakeHeard>& heard, const std::string& name) {
  unsigned n = 0;
  for (const WakeHeard& h : heard) n += h.name == name;
  return n;
}

// A second model beside the bundled one: the copy of the shipped model that always fires.
fs::path secondModel(const Fixture& f, const std::string& name) {
  const fs::path model = f.dir / (name + ".tflite");
  fs::copy_file(f.model, model, fs::copy_options::overwrite_existing);
  writeFile(f.dir / (name + ".json"), R"({"probability_cutoff":0,"sliding_window_size":1})");
  return model;
}

void severalModelsShareOneStream(const Fixture& f) {
  Firing rig(f);
  const fs::path second = secondModel(f, "second");
  rig.listener->setModels({{"second", second.string(), WakeSensitivity::Normal, 1}});
  rig.listen();
  const auto heard = pollBlocks(rig, 110);
  check(heardFrom(heard, "word") >= 2 && heardFrom(heard, "second") >= 2,
        "both models are heard on the one stream: " + positions(heard));
  check(rig.count(StreamControl::Probe) == 1, "one stream for both");
  std::vector<tc002::wake::WakeScore> scores;
  uint32_t at = 0;
  check(rig.listener->scores(scores, at) && scores.size() == 2 && scores[0].name == "word" &&
            scores[1].name == "second" && at == rig.position,
        "the scores name each model at the latest position");
  check(rig.listener->loaded("word") && rig.listener->loaded("second") && !rig.listener->loaded("third"),
        "both detectors are in memory");
}

void aModelJoinsAndLeavesWithoutANewStream(const Fixture& f) {
  Firing rig(f);
  rig.listen();
  pollBlocks(rig, 40);
  const fs::path joined = secondModel(f, "joined");
  rig.listener->setModels({{"joined", joined.string(), WakeSensitivity::Normal, 1}});
  const auto with = pollBlocks(rig, 80);
  check(heardFrom(with, "joined") >= 1 && rig.count(StreamControl::Probe) == 1,
        "a model added while listening is heard on the same stream: " + positions(with));
  rig.listener->setModels({});
  pollBlocks(rig, 2);
  const auto without = pollBlocks(rig, 60);
  check(heardFrom(without, "joined") == 0 && heardFrom(without, "word") >= 1,
        "a removed model is no longer heard, the others are");
  rig.listener->setBundle(false, WakeSensitivity::Normal);
  check(!rig.listener->hasModels(), "without the bundled model and others there is nothing to listen for");
  rig.listener->poll(rig.now, Listen::On);
  check(rig.last().operation == StreamControl::Stop, "and the stream stops");
}

void mutedDetectionsAreReported(const Fixture& f) {
  Firing rig(f);
  rig.listen();
  pollBlocks(rig, 40);
  const auto muted = pollBlocks(rig, 70, Listen::Muted);
  bool allMuted = !muted.empty();
  for (const WakeHeard& h : muted) allMuted = allMuted && h.muted;
  check(allMuted, "poll reports detections heard while muted, flagged");
  const auto resumed = pollBlocks(rig, 70);
  bool someUnmuted = false;
  for (const WakeHeard& h : resumed) someUnmuted = someUnmuted || !h.muted;
  check(someUnmuted, "and unmuted ones after it");
}

void aBrokenModelLeavesTheOthers(const Fixture& f) {
  Firing rig(f);
  writeFile(f.dir / "broken.tflite", "not a model");
  writeFile(f.dir / "broken.json", R"({"probability_cutoff":0.9,"sliding_window_size":3})");
  rig.listener->setModels({{"broken", (f.dir / "broken.tflite").string(), WakeSensitivity::Normal, 1}});
  rig.listen();
  const auto heard = pollBlocks(rig, 70);
  check(heardFrom(heard, "word") >= 1 && heardFrom(heard, "broken") == 0 && !rig.listener->loaded("broken"),
        "a model the engine refuses is left out, the others listen");
}

// A detector fed frames of a frontend it shares computes what it computes on its own frontend.
void aSharedFrontendChangesNothing(const Fixture& f) {
  const std::string bytes = [&] {
    std::ifstream in(f.model, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }();
  tc002::wake::WakeDetector own, shared;
  std::string error;
  require(own.load(bytes, error) && shared.load(bytes, error), "the model loads: " + error);
  require(own.configure({0.5f, 3, 1000, 1000}, error) && shared.configure({0.5f, 3, 1000, 1000}, error), error);
  tc002::wake::WakeFrontend frontend;
  uint64_t position = 0;
  uint32_t seed = 12345;
  bool same = true;
  std::vector<int16_t> block(tc002::kStreamBlockSamples);
  for (int b = 0; b < 200 && same; ++b) {
    for (int16_t& sample : block) {
      seed = seed * 1103515245u + 12345u;
      sample = static_cast<int16_t>(static_cast<int32_t>(seed >> 16) % 4000 - 2000);
    }
    const auto a = own.feed(block.data(), block.size());
    std::optional<tc002::wake::WakeEvent> c;
    const int16_t* samples = block.data();
    std::size_t count = block.size();
    tc002::wake::WakeFrontend::Frame frame;
    for (;;) {
      const std::size_t before = count;
      const bool ready = frontend.next(samples, count, frame);
      position += before - count;
      if (!ready) break;
      if (const auto event = shared.push(frame, position)) c = event;
    }
    same = own.score() == shared.score() && a.has_value() == c.has_value() && (!a || a->sample == c->sample);
  }
  check(same, "the shared frontend gives the same scores and detections");
}

void onlyOtherModels(const Fixture& f) {
  const fs::path only = secondModel(f, "only");
  auto executor = std::make_unique<test::ManualExecutor>();
  test::ManualExecutor* worker = executor.get();
  std::vector<StreamControl> sent;
  auto listener = WakeListener::create([&sent](StreamControl control) {
    sent.push_back(control);
    return true;
  }, std::move(executor));
  check(!listener->hasModels() && listener->bundleName().empty(), "without a bundled model nothing is wanted");
  listener->poll(1000, Listen::On);
  check(sent.empty(), "and nothing listens");
  listener->setModels({{"only", only.string(), WakeSensitivity::Normal, 1}});
  listener->poll(1000, Listen::On);
  check(sent.size() == 1 && sent.back().operation == StreamControl::Probe, "a stored model is enough to listen");
  listener->cancel();
  worker->run();
}

}

int main(int argc, char** argv) {
  require(argc == 2, "usage: tc002-wake-listener-test <shipped wake word model>");
  const Fixture fixture(argv[1]);
  settingsAndModelsAreChecked(fixture);
  listensWhenAsked(fixture);
  detectionsKeepTheirCooloff(fixture);
  aJumpStartsTheWarmUpOver(fixture);
  aFullQueueStartsTheWarmUpOver(fixture);
  aWorkerThatRefusesIsAskedAgain(fixture);
  mutedKeepsTheStream(fixture);
  offStopsTheStream(fixture);
  theModelLeavesMemoryWhileOff(fixture);
  openingKeepsNoModel(fixture);
  aModelItCannotListenWithIsRefused(fixture);
  theLevelApplies(fixture);
  aStopWithoutAnswerEnds(fixture);
  failuresBackOff(fixture);
  anUnavailableStreamWaits(fixture);
  cancelAndWithdraw(fixture);
  severalModelsShareOneStream(fixture);
  aModelJoinsAndLeavesWithoutANewStream(fixture);
  mutedDetectionsAreReported(fixture);
  aBrokenModelLeavesTheOthers(fixture);
  onlyOtherModels(fixture);
  aSharedFrontendChangesNothing(fixture);
  theShippedWakeWordOpens(argv[1]);
  return awtrix::test::finish("wake listener");
}
