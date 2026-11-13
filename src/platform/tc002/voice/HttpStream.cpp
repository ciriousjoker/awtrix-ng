#include "platform/tc002/voice/HttpStream.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "platform/linux/host/vendor/httplib.h"

namespace awtrix::tc002::voice {
namespace {
// How long a client's thread waits for data before it lets httplib look at the socket again.
constexpr auto kPollInterval = std::chrono::milliseconds(250);
}

HttpStream::HttpStream(Options options) : state_(std::make_shared<State>()) { state_->options = std::move(options); }

HttpStream::~HttpStream() { close(); }

bool HttpStream::serve(httplib::Response& response, const char* contentType, std::string greeting) {
  auto client = std::make_shared<Client>();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closed || state_->clients.size() >= state_->options.maxClients) return false;
    if (!greeting.empty()) {
      client->bytes = greeting.size();
      client->queue.push_back(std::move(greeting));
    }
    state_->clients.push_back(client);
  }
  const std::shared_ptr<State> state = state_;
  using Clock = std::chrono::steady_clock;
  auto lastWrite = std::make_shared<Clock::time_point>(Clock::now());
  response.set_chunked_content_provider(
      contentType,
      [state, client, lastWrite](size_t, httplib::DataSink& sink) {
        std::string data;
        {
          std::unique_lock<std::mutex> lock(state->mutex);
          state->wake.wait_for(lock, kPollInterval,
                               [&] { return !client->queue.empty() || client->ended || state->closed; });
          if (client->ended || state->closed) return false;
          while (!client->queue.empty()) {
            data += client->queue.front();
            client->queue.pop_front();
          }
          client->bytes = 0;
        }
        const Options& options = state->options;
        if (data.empty() && options.keepaliveMs > 0 && !options.keepalive.empty() &&
            Clock::now() - *lastWrite >= std::chrono::milliseconds(options.keepaliveMs))
          data = options.keepalive;
        if (data.empty()) return true;
        *lastWrite = Clock::now();
        return sink.write(data.data(), data.size());
      },
      [state, client](bool) {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto& clients = state->clients;
        clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
      });
  return true;
}

void HttpStream::publish(std::string_view data) {
  if (data.empty()) return;
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->clients.empty()) return;
  for (const std::shared_ptr<Client>& client : state_->clients) {
    if (client->ended) continue;
    if (client->bytes + data.size() > state_->options.maxQueuedBytes) {
      client->ended = true;
      client->queue.clear();
      client->bytes = 0;
      continue;
    }
    client->queue.emplace_back(data);
    client->bytes += data.size();
  }
  state_->wake.notify_all();
}

std::size_t HttpStream::clients() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return static_cast<std::size_t>(std::count_if(state_->clients.begin(), state_->clients.end(),
                                                [](const std::shared_ptr<Client>& c) { return !c->ended; }));
}

void HttpStream::endClients() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (const std::shared_ptr<Client>& client : state_->clients) {
    client->ended = true;
    client->queue.clear();
    client->bytes = 0;
  }
  state_->wake.notify_all();
}

void HttpStream::close() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->closed = true;
  state_->wake.notify_all();
}

}
