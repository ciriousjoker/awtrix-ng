#include "../../support.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "core/api/JsonReader.h"
#include "platform/linux/host/vendor/httplib.h"
#include "platform/tc002/wake/WakeApi.h"
#include "platform/tc002/wake/WakeModelStore.h"

using namespace awtrix;
using awtrix::test::check;
using awtrix::test::require;
using tc002::wake::WakeApi;
using tc002::wake::WakeModelEntry;
using tc002::wake::WakeModelFields;
using tc002::wake::WakeModelStore;
using tc002::wake::WakeSensitivity;
using tc002::wake::WakeStoreError;

namespace {
namespace fs = std::filesystem;

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const fs::path& path, const std::string& bytes) { std::ofstream(path, std::ios::binary) << bytes; }

bool near(double a, double b) { return a > b - 1e-9 && a < b + 1e-9; }

std::string base64(const std::string& bytes) {
  static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
    for (int s = 18; s >= 0; s -= 6) out += alphabet[(v >> s) & 63];
  }
  if (i + 1 == bytes.size()) {
    const uint32_t v = uint8_t(bytes[i]) << 16;
    out += alphabet[(v >> 18) & 63];
    out += alphabet[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == bytes.size()) {
    const uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8);
    out += alphabet[(v >> 18) & 63];
    out += alphabet[(v >> 12) & 63];
    out += alphabet[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

// The bundled model in a directory of its own, and the settings the Voice configuration holds.
struct Rig {
  fs::path root, bundleDir, data;
  std::string model;
  bool bundleOn = true;
  WakeSensitivity bundleLevel = WakeSensitivity::Normal;
  std::optional<WakeModelStore> store;

  explicit Rig(const fs::path& shipped, const char* name)
      : root(fs::temp_directory_path() / ("wake-models-" + std::string(name) + "-" + std::to_string(::getpid()))),
        bundleDir(root / "share"),
        data(root / "wake"),
        model(readFile(shipped)) {
    fs::remove_all(root);
    fs::create_directories(bundleDir);
    fs::copy_file(shipped, bundleDir / "hey_awtrix.tflite");
    writeFile(bundleDir / "hey_awtrix.json",
              R"({"ID":"hey_awtrix","DisplayName":"Hey Awtrix","probability_cutoff":0.86,"sliding_window_size":3})");
    open();
  }
  ~Rig() { fs::remove_all(root); }
  void open() {
    store.reset();
    store.emplace(data.string(),
                  WakeModelStore::Bundle{(bundleDir / "hey_awtrix.tflite").string(), [this] { return bundleOn; },
                                         [this] { return bundleLevel; },
                                         [this](std::optional<bool> on, std::optional<WakeSensitivity> level) {
                                           if (on) bundleOn = *on;
                                           if (level) bundleLevel = *level;
                                           return true;
                                         }},
                  0);
  }
  WakeStoreError put(const std::string& name, const std::string& bytes, WakeModelFields fields = {}) {
    if (!fields.cutoff) fields.cutoff = 0.9;
    if (!fields.window) fields.window = 5;
    WakeModelEntry entry;
    return store->put(name, bytes, fields, entry);
  }
};

void namesAreChecked() {
  check(WakeModelStore::validName("gute_nacht") && WakeModelStore::validName("a") &&
            WakeModelStore::validName(std::string(32, 'x')),
        "lowercase, digits and underscores, 1 to 32");
  check(!WakeModelStore::validName("") && !WakeModelStore::validName(std::string(33, 'x')) &&
            !WakeModelStore::validName("Gute") && !WakeModelStore::validName("a-b") &&
            !WakeModelStore::validName("../x") && !WakeModelStore::validName("a.b"),
        "nothing else");
}

void theBundledModelIsListed(const fs::path& shipped) {
  Rig rig(shipped, "bundle");
  const auto list = rig.store->list();
  require(list.size() == 1, "the bundled model alone");
  const WakeModelEntry& entry = list[0];
  check(entry.name == "hey_awtrix" && entry.bundle && entry.enabled && entry.phrase == "Hey Awtrix" &&
            near(entry.cutoff, 0.86) && entry.window == 3 && entry.bytes == rig.model.size() && entry.arenaBytes > 0 &&
            entry.sha256.size() == 64,
        "with its settings, size, arena and digest");
  rig.bundleOn = false;
  check(!rig.store->find("hey_awtrix")->enabled, "its switch is the Voice setting");
  WakeModelEntry out;
  check(rig.store->remove("hey_awtrix").status == 403, "it cannot be deleted");
  check(rig.store->put("hey_awtrix", rig.model, {}, out).status == 403, "nor replaced");
  WakeModelFields phrase;
  phrase.phrase = "Hallo";
  check(rig.store->patch("hey_awtrix", phrase, out).status == 403, "nor renamed");
  WakeModelFields on;
  on.enabled = true;
  on.sensitivity = WakeSensitivity::High;
  check(!rig.store->patch("hey_awtrix", on, out) && rig.bundleOn && rig.bundleLevel == WakeSensitivity::High &&
            out.enabled && out.sensitivity == WakeSensitivity::High,
        "its switch and level change the Voice settings");
}

void aModelIsStoredOnlyWhenTheEngineTakesIt(const fs::path& shipped) {
  Rig rig(shipped, "put");
  const uint64_t revision = rig.store->revision();
  WakeModelFields fields;
  fields.phrase = "Gute Nacht";
  WakeModelEntry entry;
  fields.cutoff = 0.9;
  fields.window = 5;
  check(!rig.store->put("gute_nacht", rig.model, fields, entry), "a model the engine takes is stored");
  check(entry.name == "gute_nacht" && !entry.bundle && entry.enabled && entry.phrase == "Gute Nacht" &&
            entry.cutoff == 0.9 && entry.window == 5 && entry.arenaBytes > 0 && entry.bytes == rig.model.size(),
        "and described");
  check(readFile(rig.data / "gute_nacht.tflite") == rig.model, "the model file holds the bytes");
  const std::string settings = readFile(rig.data / "gute_nacht.json");
  const api::JsonReader json(settings);
  double cutoff = 0;
  long long window = 0;
  check(api::memberValue(json, "probability_cutoff").asDouble(cutoff) && near(cutoff, 0.9) &&
            api::memberValue(json, "sliding_window_size").asLong(window) && window == 5 &&
            api::memberText(json, "wake_word") == "Gute Nacht",
        "the settings are microWakeWord's: " + settings);
  check(rig.store->revision() != revision && rig.store->enabledModels().size() == 1, "the listener learns of it");

  const uint64_t version = rig.store->find("gute_nacht")->version;
  const WakeStoreError refused = rig.put("gute_nacht", "not a model");
  check(refused.status == 422 && refused.code == "invalidModel" && !refused.message.empty(),
        "a file the engine refuses answers 422 with its reason: " + refused.message);
  check(readFile(rig.data / "gute_nacht.tflite") == rig.model && rig.store->find("gute_nacht")->version == version,
        "and the model before it stays");
  std::string truncated = rig.model.substr(0, rig.model.size() / 2);
  check(rig.put("gute_nacht", truncated).status == 422, "a truncated model is refused too");
  check(rig.put("bad name", rig.model).status == 400, "a name outside the rules is refused");
  WakeModelFields high;
  high.cutoff = 1.5;
  check(rig.put("other", rig.model, high).code == "validationFailed", "a cutoff above 1 is refused");
  WakeModelFields wide;
  wide.window = 65;
  check(rig.put("other", rig.model, wide).code == "validationFailed", "a window above 64 is refused");
  WakeModelEntry out;
  check(rig.store->put("other", rig.model, {}, out).code == "validationFailed",
        "a new model needs its cutoff and window");
  check(rig.put("empty", "").status == 413, "an empty model is refused");

  rig.open();
  const auto again = rig.store->find("gute_nacht");
  check(again && again->phrase == "Gute Nacht" && near(again->cutoff, 0.9) && again->sha256 == entry.sha256,
        "a store opened again reads the model back");
}

void settingsChangeWithoutTheModel(const fs::path& shipped) {
  Rig rig(shipped, "patch");
  require(!rig.put("gute_nacht", rig.model), "a model");
  const WakeModelEntry before = *rig.store->find("gute_nacht");
  WakeModelFields level;
  level.sensitivity = WakeSensitivity::Low;
  level.enabled = false;
  WakeModelEntry out;
  check(!rig.store->patch("gute_nacht", level, out) && out.sensitivity == WakeSensitivity::Low && !out.enabled &&
            out.version == before.version,
        "switch and level change without reading the model again");
  check(rig.store->enabledModels().empty(), "a switched-off model is not listened for");
  WakeModelFields window;
  window.window = 7;
  check(!rig.store->patch("gute_nacht", window, out) && out.window == 7 && out.version != before.version,
        "a new window reads the model again");
  check(rig.store->patch("missing", level, out).status == 404, "an unknown model is 404");
  check(!rig.store->remove("gute_nacht") && !fs::exists(rig.data / "gute_nacht.tflite") &&
            !fs::exists(rig.data / "gute_nacht.json") && !rig.store->find("gute_nacht"),
        "delete removes both files");
  check(rig.store->remove("gute_nacht").status == 404, "and a second delete is 404");
}

void atMostFourListen(const fs::path& shipped) {
  Rig rig(shipped, "limit");
  for (const char* name : {"one", "two", "three"}) require(!rig.put(name, rig.model), name);
  const WakeStoreError fourth = rig.put("four", rig.model);
  check(fourth.status == 409 && fourth.code == "tooManyModels", "a fifth listening model is refused");
  WakeModelFields off;
  off.enabled = false;
  check(!rig.put("four", rig.model, off), "a switched-off one is stored");
  WakeModelFields on;
  on.enabled = true;
  WakeModelEntry out;
  check(rig.store->patch("four", on, out).status == 409, "and cannot be switched on");
  rig.bundleOn = false;
  check(!rig.store->patch("four", on, out), "until another stops");
  check(rig.store->patch("hey_awtrix", on, out).status == 409, "the bundled model counts too");
  check(rig.store->eraseAll() && rig.store->list().size() == 1 && fs::is_empty(rig.data),
        "a factory reset removes the stored models");
}

void base64IsStrict() {
  std::string out;
  check(WakeApi::decodeBase64("aGVsbG8=", out) && out == "hello", "padded");
  check(WakeApi::decodeBase64("aGVsbG8", out) && out == "hello", "unpadded");
  check(WakeApi::decodeBase64("", out) && out.empty(), "empty");
  check(WakeApi::decodeBase64("_-8=", out) && out == std::string("\xff\xef", 2), "URL alphabet");
  check(!WakeApi::decodeBase64("aGVs bG8=", out), "no spaces");
  check(!WakeApi::decodeBase64("a=GV", out), "no data after padding");
  check(!WakeApi::decodeBase64("a", out), "no single character");
}

httplib::Response call(WakeApi& api, const std::string& method, const std::string& path, const std::string& body = {}) {
  httplib::Request request;
  request.method = method;
  request.path = path;
  request.body = body;
  httplib::Response response;
  require(api.handle(request, response), "the API takes " + path);
  return response;
}

void theRoutes(const fs::path& shipped) {
  Rig rig(shipped, "api");
  WakeApi api(*rig.store, [](const std::string& name) { return name == "hey_awtrix"; });
  httplib::Request other;
  other.method = "GET";
  other.path = "/api/v1/voice";
  httplib::Response unused;
  check(!api.handle(other, unused), "other paths are not its own");

  auto listed = call(api, "GET", "/api/v1/wake/models");
  check(listed.status == -1 || listed.status == 200, "GET lists");
  check(listed.body.find("\"name\":\"hey_awtrix\"") != std::string::npos &&
            listed.body.find("\"source\":\"bundle\"") != std::string::npos &&
            listed.body.find("\"loaded\":true") != std::string::npos,
        "the bundled model, loaded: " + listed.body);

  const std::string body = R"({"phrase":"Gute Nacht","tflite":")" + base64(rig.model) +
                           R"(","probabilityCutoff":0.9,"slidingWindowSize":5,"enabled":true,"sensitivity":"high"})";
  auto put = call(api, "PUT", "/api/v1/wake/models/gute_nacht", body);
  check(put.status != 400 && put.status != 422 && put.body.find("\"sensitivity\":\"high\"") != std::string::npos &&
            put.body.find("\"source\":\"data\"") != std::string::npos &&
            put.body.find("\"probabilityCutoff\":0.9") != std::string::npos,
        "PUT stores and answers the entry: " + put.body);
  auto broken = call(api, "PUT", "/api/v1/wake/models/gute_nacht",
                     R"({"tflite":"bm90IGEgbW9kZWw=","probabilityCutoff":0.9,"slidingWindowSize":5})");
  check(broken.status == 422 && broken.body.find("invalidModel") != std::string::npos,
        "a model the engine refuses is 422: " + broken.body);
  check(call(api, "PUT", "/api/v1/wake/models/x", R"({"probabilityCutoff":0.9})").status == 400,
        "PUT needs the model");
  check(call(api, "PUT", "/api/v1/wake/models/x", R"({"tflite":"!!!!"})").status == 400, "and valid base64");
  check(call(api, "PUT", "/api/v1/wake/models/Bad", "{}").status == 400, "and a valid name");
  auto patched = call(api, "PATCH", "/api/v1/wake/models/gute_nacht", R"({"enabled":false,"phrase":"Nacht"})");
  check(patched.body.find("\"enabled\":false") != std::string::npos &&
            patched.body.find("\"phrase\":\"Nacht\"") != std::string::npos,
        "PATCH changes settings: " + patched.body);
  check(call(api, "PATCH", "/api/v1/wake/models/gute_nacht", R"({"sensitivity":"loud"})").status == 400,
        "an unknown level is refused");
  check(call(api, "PATCH", "/api/v1/wake/models/gute_nacht", R"({"tflite":"AA=="})").status == 400,
        "PATCH does not replace the model");
  check(call(api, "GET", "/api/v1/wake/models/gute_nacht").body.find("\"name\":\"gute_nacht\"") != std::string::npos,
        "GET of one model");
  check(call(api, "DELETE", "/api/v1/wake/models/hey_awtrix").status == 403, "the bundled model stays");
  check(call(api, "DELETE", "/api/v1/wake/models/gute_nacht").body == "{\"ok\":true}", "DELETE removes");
  check(call(api, "GET", "/api/v1/wake/models/gute_nacht").status == 404, "and it is gone");
  check(call(api, "POST", "/api/v1/wake/models/gute_nacht").status == 405, "other methods are 405");
  check(call(api, "POST", "/api/v1/wake/events").status == 405, "the events are GET only");
  api.stop();
}

}

int main(int argc, char** argv) {
  require(argc == 2, "usage: tc002-wake-models-test <shipped wake word model>");
  const fs::path shipped = argv[1];
  namesAreChecked();
  theBundledModelIsListed(shipped);
  aModelIsStoredOnlyWhenTheEngineTakesIt(shipped);
  settingsChangeWithoutTheModel(shipped);
  atMostFourListen(shipped);
  base64IsStrict();
  theRoutes(shipped);
  return awtrix::test::finish("wake models");
}
