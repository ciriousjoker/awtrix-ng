#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "platform/tc002/voice/HttpStream.h"
#include "platform/tc002/wake/WakeListener.h"
#include "platform/tc002/wake/WakeModelStore.h"

namespace httplib {
struct Request;
struct Response;
}

namespace awtrix::tc002::wake {

// /api/v1/wake/models[/NAME] over the model store, and /api/v1/wake/events, the detections and
// window scores as Server-Sent Events.
class WakeApi {
 public:
  static constexpr std::size_t kMaxEventClients = 3;
  static constexpr int64_t kKeepaliveMs = 10000, kScoresMs = 100;

  WakeApi(WakeModelStore& store, std::function<bool(const std::string&)> loaded);
  bool handle(const httplib::Request& request, httplib::Response& response);

  // On the loop: a detection, with the wall clock in ms since the epoch or -1 without one.
  void detection(const WakeHeard& heard, int64_t atMs);
  bool wantsScores() const { return scored_.clients() > 0; }
  void scores(uint32_t position, const std::vector<WakeScore>& scores);
  void stop();

  static bool decodeBase64(std::string_view text, std::string& out);
  static std::string entryJson(const WakeModelEntry& entry, bool loaded);

 private:
  bool models(const httplib::Request& request, httplib::Response& response, const std::string& name);
  bool events(const httplib::Request& request, httplib::Response& response);

  WakeModelStore& store_;
  std::function<bool(const std::string&)> loaded_;
  voice::HttpStream plain_, scored_;
};

}
