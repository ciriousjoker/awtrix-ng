#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace httplib {
struct Response;
}

namespace awtrix::tc002::voice {

// One endless HTTP response for several clients at once (chunked transfer): the loop publishes,
// each client's listener thread writes what queued for it. A client whose queue outgrows its
// bound is ended rather than given a stream with a hole in it.
class HttpStream {
 public:
  struct Options {
    std::size_t maxClients = 2;
    std::size_t maxQueuedBytes = 64 * 1024;
    // A comment line written when nothing else was for this long, so dead clients are found.
    int64_t keepaliveMs = 0;
    std::string keepalive;
  };

  explicit HttpStream(Options options);
  ~HttpStream();
  HttpStream(const HttpStream&) = delete;
  HttpStream& operator=(const HttpStream&) = delete;

  // Takes a client: false when all places are taken. The response gets the content type and
  // the provider; the caller adds its headers.
  bool serve(httplib::Response& response, const char* contentType, std::string greeting = {});
  void publish(std::string_view data);
  std::size_t clients() const;
  // Ends every client; new ones are still taken.
  void endClients();
  // Ends every client; no new one is taken.
  void close();

 private:
  struct Client {
    std::deque<std::string> queue;
    std::size_t bytes = 0;
    bool ended = false;
  };
  struct State {
    Options options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::shared_ptr<Client>> clients;
    bool closed = false;
  };

  std::shared_ptr<State> state_;
};

}
