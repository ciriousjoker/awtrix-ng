#include "platform/tc002/wake/WakeApi.h"

#include <utility>

#include "core/api/ApiRouter.h"
#include "core/api/JsonReader.h"
#include "core/api/JsonWriter.h"
#include "platform/linux/host/vendor/httplib.h"

namespace awtrix::tc002::wake {
namespace {
constexpr const char* kModels = "/api/v1/wake/models";
constexpr const char* kEvents = "/api/v1/wake/events";
constexpr const char* kKeepalive = ": keepalive\n\n";

void sendError(httplib::Response& res, int status, const char* code, const std::string& message,
               const std::string& field = {}) {
  res.status = status;
  res.set_content(api::errorJson(code, message, field), "application/json");
}

void sendError(httplib::Response& res, const WakeStoreError& error) {
  res.status = error.status;
  res.set_content(api::errorJson(error.code.c_str(), error.message, error.field), "application/json");
}

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

// The members of a PUT or PATCH body; false with the field and reason of the first bad one.
bool readFields(std::string_view body, WakeModelFields& fields, std::string_view& model, bool& hasModel,
                std::string& field, std::string& reason) {
  api::JsonReader root(body);
  if (!root.isObject()) {
    reason = "the body must be a JSON object";
    return false;
  }
  const auto bad = [&](const char* name, const char* why) {
    field = name;
    reason = why;
    return false;
  };
  const api::JsonReader phrase = api::memberValue(root, "phrase");
  if (api::present(phrase)) {
    std::string text;
    if (!phrase.appendString(text)) return bad("phrase", "phrase must be a string");
    fields.phrase = text;
  }
  const api::JsonReader cutoff = api::memberValue(root, "probabilityCutoff");
  if (api::present(cutoff)) {
    double value = 0;
    if (!cutoff.asDouble(value)) return bad("probabilityCutoff", "probabilityCutoff must be a number");
    fields.cutoff = value;
  }
  const api::JsonReader window = api::memberValue(root, "slidingWindowSize");
  if (api::present(window)) {
    long long value = 0;
    if (!window.isInteger() || !window.asLong(value)) return bad("slidingWindowSize", "slidingWindowSize must be an integer");
    fields.window = value;
  }
  const api::JsonReader enabled = api::memberValue(root, "enabled");
  if (api::present(enabled)) {
    bool value = false;
    if (!enabled.asBool(value)) return bad("enabled", "enabled must be true or false");
    fields.enabled = value;
  }
  const api::JsonReader sensitivity = api::memberValue(root, "sensitivity");
  if (api::present(sensitivity)) {
    std::string text;
    WakeSensitivity level = WakeSensitivity::Normal;
    if (!sensitivity.appendString(text) || !parseWakeSensitivity(text, level))
      return bad("sensitivity", "sensitivity must be low, normal or high");
    fields.sensitivity = level;
  }
  const api::JsonReader tflite = api::memberValue(root, "tflite");
  hasModel = api::present(tflite);
  if (hasModel && !tflite.stringView(model)) return bad("tflite", "tflite must be a base64 string");
  return true;
}

std::string event(const char* name, const std::string& data) {
  return std::string("event: ") + name + "\ndata: " + data + "\n\n";
}

}

WakeApi::WakeApi(WakeModelStore& store, std::function<bool(const std::string&)> loaded)
    : store_(store),
      loaded_(std::move(loaded)),
      plain_({kMaxEventClients, 64 * 1024, kKeepaliveMs, kKeepalive}),
      scored_({kMaxEventClients, 64 * 1024, kKeepaliveMs, kKeepalive}) {}

bool WakeApi::decodeBase64(std::string_view text, std::string& out) {
  out.clear();
  out.reserve(text.size() / 4 * 3);
  uint32_t bits = 0;
  int count = 0;
  std::size_t padding = 0;
  for (const char c : text) {
    if (c == '=') {
      ++padding;
      continue;
    }
    const int value = base64Value(c);
    if (value < 0 || padding) return false;
    bits = (bits << 6) | static_cast<uint32_t>(value);
    if (++count == 4) {
      out.push_back(static_cast<char>(bits >> 16));
      out.push_back(static_cast<char>(bits >> 8));
      out.push_back(static_cast<char>(bits));
      bits = 0;
      count = 0;
    }
  }
  if (count == 1 || padding > 2) return false;
  if (count == 2) out.push_back(static_cast<char>(bits >> 4));
  if (count == 3) {
    out.push_back(static_cast<char>(bits >> 10));
    out.push_back(static_cast<char>(bits >> 2));
  }
  return true;
}

std::string WakeApi::entryJson(const WakeModelEntry& entry, bool loaded) {
  std::string json;
  api::JsonWriter writer(json);
  writer.beginObject()
      .member("name", entry.name)
      .member("phrase", entry.phrase)
      .member("enabled", entry.enabled)
      .member("sensitivity", std::string(wakeSensitivityName(entry.sensitivity)))
      .member("probabilityCutoff", entry.cutoff, 4)
      .member("slidingWindowSize", static_cast<unsigned long long>(entry.window))
      .member("bytes", static_cast<unsigned long long>(entry.bytes))
      .member("arenaBytes", static_cast<unsigned long long>(entry.arenaBytes))
      .member("source", entry.bundle ? "bundle" : "data")
      .member("sha256", entry.sha256)
      .member("loaded", loaded)
      .endObject();
  return json;
}

bool WakeApi::handle(const httplib::Request& request, httplib::Response& response) {
  const std::string& path = request.path;
  if (path == kEvents) return events(request, response);
  const std::string models = kModels;
  if (path == models || path == models + "/") return this->models(request, response, "");
  if (path.compare(0, models.size() + 1, models + "/") != 0) return false;
  return this->models(request, response, path.substr(models.size() + 1));
}

bool WakeApi::models(const httplib::Request& request, httplib::Response& response, const std::string& name) {
  response.set_header("Cache-Control", "no-store");
  const std::string& method = request.method;
  if (name.empty()) {
    if (method != "GET") {
      sendError(response, 405, "methodNotAllowed", "allowed: GET");
      return true;
    }
    std::string json = "[";
    for (const WakeModelEntry& entry : store_.list()) {
      if (json.size() > 1) json += ',';
      json += entryJson(entry, loaded_ && loaded_(entry.name));
    }
    json += ']';
    response.set_content(json, "application/json");
    return true;
  }
  if (!WakeModelStore::validName(name)) {
    sendError(response, 400, "invalidName", "a model name is 1 to 32 of a-z, 0-9 and _");
    return true;
  }
  if (method == "GET") {
    const auto entry = store_.find(name);
    if (!entry) sendError(response, 404, "notFound", "no wake word model " + name);
    else response.set_content(entryJson(*entry, loaded_ && loaded_(name)), "application/json");
    return true;
  }
  if (method == "DELETE") {
    if (const WakeStoreError error = store_.remove(name)) sendError(response, error);
    else response.set_content("{\"ok\":true}", "application/json");
    return true;
  }
  if (method != "PUT" && method != "PATCH") {
    sendError(response, 405, "methodNotAllowed", "allowed: GET, PUT, PATCH, DELETE");
    return true;
  }
  WakeModelFields fields;
  std::string_view encoded;
  bool hasModel = false;
  std::string field, reason;
  if (!readFields(request.body, fields, encoded, hasModel, field, reason)) {
    sendError(response, 400, "validationFailed", reason, field);
    return true;
  }
  WakeModelEntry entry;
  WakeStoreError error;
  if (method == "PATCH") {
    if (hasModel) {
      sendError(response, 400, "validationFailed", "PATCH changes settings; PUT replaces the model", "tflite");
      return true;
    }
    error = store_.patch(name, fields, entry);
  } else {
    std::string model;
    if (!hasModel) {
      sendError(response, 400, "validationFailed", "tflite is required", "tflite");
      return true;
    }
    if (!decodeBase64(encoded, model)) {
      sendError(response, 400, "validationFailed", "tflite is not valid base64", "tflite");
      return true;
    }
    error = store_.put(name, model, fields, entry);
  }
  if (error) sendError(response, error);
  else response.set_content(entryJson(entry, loaded_ && loaded_(name)), "application/json");
  return true;
}

bool WakeApi::events(const httplib::Request& request, httplib::Response& response) {
  if (request.method != "GET") {
    sendError(response, 405, "methodNotAllowed", "allowed: GET");
    return true;
  }
  const bool withScores = request.has_param("scores") && request.get_param_value("scores") != "0";
  if (plain_.clients() + scored_.clients() >= kMaxEventClients ||
      !(withScores ? scored_ : plain_).serve(response, "text/event-stream", ": awtrix wake events\n\n")) {
    response.set_header("Retry-After", "5");
    sendError(response, 503, "tooManyClients", "at most 3 clients follow the wake word events");
    return true;
  }
  response.set_header("Cache-Control", "no-store");
  response.set_header("X-Accel-Buffering", "no");
  return true;
}

void WakeApi::detection(const WakeHeard& heard, int64_t atMs) {
  if (plain_.clients() + scored_.clients() == 0) return;
  std::string json;
  api::JsonWriter writer(json);
  writer.beginObject()
      .member("name", heard.name)
      .member("score", static_cast<double>(heard.score), 3)
      .member("at", static_cast<long long>(atMs))
      .member("muted", heard.muted)
      .endObject();
  const std::string text = event("detection", json);
  plain_.publish(text);
  scored_.publish(text);
}

void WakeApi::scores(uint32_t position, const std::vector<WakeScore>& scores) {
  std::string json;
  api::JsonWriter writer(json);
  writer.beginObject().member("t", static_cast<unsigned long long>(position)).key("scores").beginObject();
  for (const WakeScore& score : scores) writer.member(score.name.c_str(), static_cast<double>(score.score), 3);
  writer.endObject().endObject();
  scored_.publish(event("scores", json));
}

void WakeApi::stop() {
  plain_.close();
  scored_.close();
}

}
