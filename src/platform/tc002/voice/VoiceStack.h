#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "platform/linux/net/HttpClient.h"
#include "platform/tc002/contract/SupervisorProtocol.h"
#include "platform/tc002/runtime/MicrophoneHub.h"
#include "platform/tc002/runtime/MicrophoneInput.h"
#include "platform/tc002/voice/AssistantRouter.h"
#include "platform/tc002/voice/KnobGesture.h"
#include "platform/tc002/voice/Tc002VoiceSpeaker.h"
#include "platform/tc002/voice/VoiceApi.h"
#include "platform/tc002/voice/VoiceRuntime.h"
#include "platform/tc002/voice/MicrophoneStreamApi.h"
#include "platform/tc002/voice/VoiceScripting.h"
#include "platform/tc002/wake/WakeApi.h"
#include "platform/tc002/wake/WakeListener.h"
#include "platform/tc002/wake/WakeModelStore.h"
#include "platform/tc002/wake/WakeScripting.h"

namespace awtrix::tc002::voice {

class VoiceStack {
 public:
  struct Setup {
    MicrophoneHub::Send send;
    bool supervised = false;
    Tc002Speaker* speaker = nullptr;
    std::string wakeWordModel;
    std::string configDirectory;
    // The writable wake word models, and the free space a stored one must leave there.
    std::string wakeDirectory;
    uint64_t storageReserveBytes = 0;
    // Milliseconds since the epoch, or -1 while the clock has no time.
    std::function<int64_t()> wallClockMs;
  };
  struct ScriptSetup {
    std::function<bool(const std::string&)> visible;
    std::string voiceActivityModel;
    net::FillRequest fillRequest;
  };

  explicit VoiceStack(const Setup& setup);
  VoiceStack(const VoiceStack&) = delete;
  VoiceStack& operator=(const VoiceStack&) = delete;

  VoiceScripting* composeScripting(ScriptSetup setup);
  // on_wake for every running app that defines it; apps names the installed ones.
  wake::WakeScripting* composeWakeScripting(std::function<std::vector<std::string>()> apps,
                                            std::function<bool(const std::string&, std::string&)> source);
  void composeRouter(AssistantRouter::Ports ports);

  void receive(const SupervisorMessage& message, int64_t now);
  void tick(int64_t now, bool allowed, bool knobAllowed);
  void draw(Canvas& canvas, int64_t now);
  // The privacy dot while a client streams the microphone; over whatever the panel shows.
  void drawPrivacy(Canvas& canvas) const;
  bool handle(const httplib::Request& request, httplib::Response& response);
  bool start(VoiceTrigger trigger);
  void stop();
  void stopTransfers();
  bool eraseConfig();

  bool available() const { return runtime_ != nullptr; }
  MicrophoneInput& microphone() { return input_; }
  KnobGesture& gesture() { return gesture_; }
  VoiceRuntime* runtime() { return runtime_.get(); }

 private:
  MicrophoneInput input_;
  MicrophoneHub hub_;
  MicrophoneHub::Consumer wakeMicrophone_, voiceMicrophone_, scriptMicrophone_;
  KnobGesture gesture_;
  std::optional<Tc002VoiceSpeaker> speaker_;
  std::unique_ptr<VoiceRuntime> runtime_;
  std::optional<VoiceApi> api_;
  void heard(const wake::WakeHeard& heard);
  void tickWake(int64_t now);
  std::vector<std::string> listeningModels() const;
  bool setBundle(std::optional<bool> enabled, std::optional<wake::WakeSensitivity> level);

  std::unique_ptr<wake::WakeListener> wake_;
  bool wakeWordAvailable_ = false;
  std::unique_ptr<wake::WakeModelStore> wakeModels_;
  uint64_t wakeRevision_ = 0;
  std::optional<wake::WakeApi> wakeApi_;
  int64_t scoresAt_ = 0;
  std::function<int64_t()> wallClockMs_;
  MicrophoneHub::Consumer streamMicrophone_;
  std::optional<MicrophoneStreamApi> microphoneStream_;
  std::unique_ptr<wake::WakeScripting> wakeScripting_;
  std::unique_ptr<VoiceScripting> scripting_;
  AssistantRouter::NoModule noModule_;
  std::optional<AssistantRouter> router_;
};

}
