#include "platform/tc002/voice/MicrophoneStreamApi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "core/api/ApiRouter.h"
#include "core/api/HttpProtocol.h"
#include "platform/linux/host/vendor/httplib.h"
#include "platform/tc002/contract/MicrophoneStream.h"
#include "platform/tc002/contract/SupervisorProtocol.h"
#include "system/Log.h"

namespace awtrix::tc002::voice {
namespace {
constexpr const char* kPath = "/api/v1/mic/stream";
constexpr std::array<int64_t, 5> kBackoffMs{1000, 2000, 5000, 10000, 30000};
constexpr int64_t kUnavailableMs = 30000;
static_assert(kMicrophonePcmRate == 16000, "the stream's headers name 16 kHz");

void sendError(httplib::Response& res, int status, const char* code, const std::string& message) {
  res.status = status;
  res.set_content(api::errorJson(code, message), "application/json");
}
}

MicrophoneStreamApi::MicrophoneStreamApi(MicrophonePort microphone)
    : microphone_(std::move(microphone)), stream_({kMaxClients, kMaxQueuedBytes, 0, {}}) {}

bool MicrophoneStreamApi::handle(const httplib::Request& request, httplib::Response& response) {
  if (request.path != kPath) return false;
  response.set_header("Cache-Control", "no-store");
  if (request.method != "GET") {
    sendError(response, 405, "methodNotAllowed", "allowed: GET");
    return true;
  }
  // A page of another site never hears the room, whatever the CORS headers of the API allow.
  if (api::fromOtherSite({request.get_header_value("Origin"), request.get_header_value_count("Origin"),
                          request.get_header_value("Host"), request.get_header_value_count("Host"),
                          request.get_header_value("Sec-Fetch-Site"),
                          request.get_header_value_count("Sec-Fetch-Site")})) {
    response.headers.erase("Access-Control-Allow-Origin");
    const api::HttpResult refusal = api::forbiddenOrigin();
    response.status = refusal.status;
    response.set_content(refusal.body, "application/json");
    return true;
  }
  if (unavailable_ && now_ < retryAt_) {
    sendError(response, 503, "microphoneUnavailable", kStreamUnavailable);
    return true;
  }
  if (!stream_.serve(response, "application/octet-stream")) {
    response.set_header("Retry-After", "5");
    sendError(response, 503, "tooManyClients", "at most 2 clients stream the microphone");
    return true;
  }
  response.set_header("X-Sample-Rate", "16000");
  response.set_header("X-Channels", "1");
  response.set_header("X-Sample-Format", "s16le");
  logf("microphone: a client streams the microphone (%zu now)", stream_.clients());
  return true;
}

void MicrophoneStreamApi::receive(const StreamEvent& event, int64_t now) {
  const MicrophoneClient::Report report = microphone_.receive(event, now);
  if (report == MicrophoneClient::Report::Audio || report == MicrophoneClient::Report::Gap) {
    // The clock is little-endian, so the samples go out as they are.
    std::string bytes(event.samples.size() * sizeof(int16_t), '\0');
    if (!bytes.empty()) std::memcpy(bytes.data(), event.samples.data(), bytes.size());
    stream_.publish(bytes);
    return;
  }
  apply(report, now);
}

void MicrophoneStreamApi::tick(int64_t now) {
  now_ = now;
  const bool wanted = stream_.clients() > 0;
  if (wanted != wanted_) {
    if (!wanted) logf("microphone: no client streams the microphone");
    wanted_ = wanted;
  }
  if (microphone_.phase() == MicrophoneClient::Phase::Idle) {
    if (wanted && now >= retryAt_) microphone_.open(now);
  } else if (!wanted) {
    microphone_.stop(now);
  }
  apply(microphone_.tick(now), now);
}

void MicrophoneStreamApi::apply(MicrophoneClient::Report report, int64_t now) {
  switch (report) {
    case MicrophoneClient::Report::Unavailable:
      if (!unavailable_) logf("microphone: %s", kStreamUnavailable);
      unavailable_ = true;
      retryAt_ = now + kUnavailableMs;
      stream_.endClients();
      return;
    case MicrophoneClient::Report::Started:
      unavailable_ = false;
      failures_ = 0;
      error_.clear();
      return;
    case MicrophoneClient::Report::Ended: {
      const std::string& error = microphone_.error();
      if (error != error_) logf("microphone: the stream ended%s%s", error.empty() ? "" : ": ", error.c_str());
      error_ = error;
      retryAt_ = now + kBackoffMs[std::min<std::size_t>(failures_++, kBackoffMs.size() - 1)];
      return;
    }
    case MicrophoneClient::Report::None:
    case MicrophoneClient::Report::Available:
    case MicrophoneClient::Report::Audio:
    case MicrophoneClient::Report::Gap:
    case MicrophoneClient::Report::Stopped:
      return;
  }
}

void MicrophoneStreamApi::stop() {
  stream_.close();
  microphone_.cancel();
}

}
