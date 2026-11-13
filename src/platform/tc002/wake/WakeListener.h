#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/posix/Executor.h"
#include "platform/tc002/contract/MicrophoneClient.h"
#include "platform/tc002/wake/WakeFrontend.h"
#include "platform/tc002/wake/WakeSensitivity.h"

namespace awtrix::tc002::wake {

class WakeDetector;

// One model the listener runs: its .tflite (with the .json of the same name beside it), the
// level it listens at, and a version that changes whenever its files do, so a replaced model is
// read again.
struct WakeModelSpec {
  std::string name;
  std::string path;
  WakeSensitivity sensitivity = WakeSensitivity::Normal;
  uint64_t version = 0;
};

struct WakeHeard {
  std::string name;
  float score = 0.0f;
  uint32_t sample = 0;
  // Heard in audio that arrived while the clock itself made sound: reported, but not acted on.
  bool muted = false;
};

struct WakeScore {
  std::string name;
  float score = 0.0f;
};

// Runs every wanted model on the one microphone stream: one frontend for all of them, a
// detector per model, on a worker thread. The models in use are the bundled one (open()) while
// it is switched on, and those of setModels().
class WakeListener {
 public:
  enum class Listen { Off, Muted, On };
  static constexpr std::size_t kQueueBlocks = 16;
  static constexpr int64_t kModelOffMs = 60000;

  // Refuses a bundled model it could not listen with, and keeps none in memory until it is asked
  // to listen. Its name is the file's stem.
  static std::unique_ptr<WakeListener> open(const std::string& modelPath, MicrophonePort microphone,
                                            std::unique_ptr<posix::Executor> worker, std::string& error);
  // Without a bundled model: only setModels() gives it something to listen for.
  static std::unique_ptr<WakeListener> create(MicrophonePort microphone, std::unique_ptr<posix::Executor> worker);
  ~WakeListener();
  WakeListener(const WakeListener&) = delete;
  WakeListener& operator=(const WakeListener&) = delete;

  // Every detection since the last call, muted ones flagged; none while it is Off.
  std::vector<WakeHeard> poll(int64_t now, Listen listen);
  // The last unmuted detection, as a single-model caller wants it.
  std::optional<WakeHeard> tick(int64_t now, Listen listen);

  void setBundle(bool enabled, WakeSensitivity level);
  void setSensitivity(WakeSensitivity level) { setBundle(bundleEnabled_, level); }
  void setModels(std::vector<WakeModelSpec> models);
  bool hasModels() const;
  const std::string& bundleName() const { return bundle_.name; }
  // Whether the model's detector is in memory now.
  bool loaded(const std::string& name) const;
  // The window means of the running models after the latest block, and the stream position of it.
  bool scores(std::vector<WakeScore>& out, uint32_t& position) const;

  void receive(const StreamEvent& event, int64_t now);
  void cancel();

 private:
  struct Block {
    std::vector<int16_t> samples;
    bool reset = false;
    uint32_t firstSample = 0;
    bool muted = false;
  };
  struct Loaded {
    WakeModelSpec spec;
    std::shared_ptr<WakeDetector> detector;
  };
  struct Running {
    std::string name;
    std::shared_ptr<WakeDetector> detector;
    WakeSensitivity sensitivity = WakeSensitivity::Normal;
    bool fresh = true;
  };

  WakeListener(MicrophonePort microphone, std::unique_ptr<posix::Executor> worker);
  std::vector<WakeModelSpec> wanted() const;
  bool loadDetectors(int64_t now);
  void unloadDetectors();
  void apply(MicrophoneClient::Report report, int64_t now);
  void unavailable(int64_t now);
  void failed(int64_t now);
  void restart(uint32_t origin);
  void queueAudio(const StreamEvent& event);
  void queue(Block block);
  void drain();
  void adopt();
  void detect(const Block& block, std::vector<WakeHeard>& heard);

  // The loop's side.
  WakeModelSpec bundle_;
  bool bundleEnabled_ = false;
  std::vector<WakeModelSpec> models_;
  bool changed_ = true;
  std::vector<Loaded> loaded_;
  std::vector<std::string> failedModels_, loadErrors_;
  MicrophoneClient microphone_;
  std::string error_;
  int64_t retryAt_ = 0, wantedAt_ = 0;
  unsigned failures_ = 0;
  bool muted_ = false;

  // Shared with the worker under mutex_.
  mutable std::mutex mutex_;
  std::deque<Block> blocks_;
  std::vector<std::vector<int16_t>> spareSamples_;
  std::vector<WakeHeard> heard_;
  std::optional<std::vector<Running>> pending_;
  std::vector<WakeScore> scores_;
  uint32_t scorePosition_ = 0;
  bool draining_ = false;

  // The worker's side.
  std::vector<Running> running_;
  std::vector<WakeScore> scratch_;
  WakeFrontend frontend_;
  uint64_t position_ = 0;
  uint32_t origin_ = 0;
  std::unique_ptr<posix::Executor> worker_;
};

}
