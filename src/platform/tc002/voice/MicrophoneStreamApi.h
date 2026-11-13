#pragma once

#include <cstdint>
#include <string>

#include "platform/tc002/contract/MicrophoneClient.h"
#include "platform/tc002/voice/HttpStream.h"

namespace httplib {
struct Request;
struct Response;
}

namespace awtrix::tc002::voice {

// GET /api/v1/mic/stream: the microphone hub's audio as raw PCM (s16le, mono, 16 kHz) to at most
// two clients. Its own hub consumer opens the stream while a client is connected; Voice, the wake
// word and the voice module keep theirs.
class MicrophoneStreamApi {
 public:
  static constexpr std::size_t kMaxClients = 2;
  // Four seconds of audio: a client that falls further behind is ended.
  static constexpr std::size_t kMaxQueuedBytes = 4 * 2 * 16000;

  explicit MicrophoneStreamApi(MicrophonePort microphone);

  bool handle(const httplib::Request& request, httplib::Response& response);
  void receive(const StreamEvent& event, int64_t now);
  void tick(int64_t now);
  // A client is connected: the panel shows the privacy dot.
  bool active() const { return stream_.clients() > 0; }
  void stop();

 private:
  void apply(MicrophoneClient::Report report, int64_t now);

  MicrophoneClient microphone_;
  HttpStream stream_;
  bool unavailable_ = false, wanted_ = false;
  int64_t retryAt_ = 0, now_ = 0;
  unsigned failures_ = 0;
  std::string error_;
};

}
