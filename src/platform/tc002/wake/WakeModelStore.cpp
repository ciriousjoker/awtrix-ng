#include "platform/tc002/wake/WakeModelStore.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>

#include "core/api/JsonReader.h"
#include "core/api/JsonWriter.h"
#include "platform/posix/Digest.h"
#include "platform/posix/Files.h"
#include "platform/tc002/wake/ProbabilityWindow.h"
#include "platform/tc002/wake/WakeDetector.h"
#include "platform/tc002/wake/WakeFrontend.h"
#include "system/Log.h"

namespace awtrix::tc002::wake {
namespace {
constexpr std::size_t kSettingsLimit = 4096;
constexpr const char* kModelSuffix = ".tflite";
constexpr const char* kSettingsSuffix = ".json";

WakeStoreError error(int status, const char* code, std::string message, std::string field = {}) {
  return {status, code, std::move(message), std::move(field)};
}

std::string stem(const std::string& path) {
  const auto slash = path.rfind('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const auto dot = name.rfind('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

bool printable(std::string_view text) {
  for (const unsigned char c : text)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

// The settings of a model file: the microWakeWord keys (top level or under "micro"), the phrase
// and this store's switch and level.
struct Settings {
  std::string phrase;
  double cutoff = -1;
  long long window = -1;
  bool enabled = true;
  WakeSensitivity sensitivity = WakeSensitivity::Normal;
};

bool readSettings(const std::string& path, Settings& out) {
  std::string json;
  if (!posix::readText(path, json, kSettingsLimit)) return false;
  const api::JsonReader root(json);
  if (!root.isObject()) return false;
  const api::JsonReader micro = api::memberValue(root, "micro");
  const auto number = [&](const char* key, double& value) {
    return api::memberValue(root, key).asDouble(value) || api::memberValue(micro, key).asDouble(value);
  };
  double window = -1;
  if (!number("probability_cutoff", out.cutoff) || !number("sliding_window_size", window)) return false;
  out.window = static_cast<long long>(window);
  for (const char* key : {"wake_word", "DisplayName", "WakePhrase"}) {
    std::string phrase;
    if (api::memberValue(root, key).appendString(phrase) && !phrase.empty()) {
      out.phrase = phrase;
      break;
    }
  }
  bool enabled = true;
  if (api::memberValue(root, "enabled").asBool(enabled)) out.enabled = enabled;
  std::string level;
  if (api::memberValue(root, "sensitivity").appendString(level)) parseWakeSensitivity(level, out.sensitivity);
  return true;
}

// The engine's verdict on a model: the loader's reason, or the arena it plans.
bool engineTakes(std::string_view bytes, double cutoff, std::size_t window, std::size_t& arena, std::string& reason) {
  WakeDetector detector;
  if (!detector.load(bytes, reason)) return false;
  if (!detector.configure({static_cast<float>(cutoff), window, 1000, 1000}, reason)) return false;
  arena = detector.arenaBytes();
  return true;
}

}

WakeModelStore::WakeModelStore(std::string directory, Bundle bundle, uint64_t reserveBytes)
    : directory_(std::move(directory)), bundle_(std::move(bundle)), reserveBytes_(reserveBytes) {
  if (!bundle_.path.empty()) {
    bundleName_ = stem(bundle_.path);
    Settings settings;
    const auto file = std::make_unique<posix::MappedFile>(bundle_.path, kMaxModelBytes);
    std::string reason;
    std::size_t arena = 0;
    if (file->valid() && readSettings(bundle_.path.substr(0, bundle_.path.rfind('.')) + kSettingsSuffix, settings) &&
        settings.window > 0 &&
        engineTakes(file->bytes(), settings.cutoff, static_cast<std::size_t>(settings.window), arena, reason)) {
      bundleStatic_ = {bundleName_, settings.phrase.empty() ? bundleName_ : settings.phrase, false,
                       WakeSensitivity::Normal, settings.cutoff, static_cast<std::size_t>(settings.window),
                       file->bytes().size(), arena, true, posix::sha256Hex(file->bytes()), bundle_.path, 0};
      bundleValid_ = true;
    }
  }
  scan();
}

bool WakeModelStore::validName(std::string_view name) {
  if (name.empty() || name.size() > 32) return false;
  return std::all_of(name.begin(), name.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::string WakeModelStore::modelPath(const std::string& name) const { return directory_ + "/" + name + kModelSuffix; }

std::string WakeModelStore::settingsPath(const std::string& name) const {
  return directory_ + "/" + name + kSettingsSuffix;
}

// Reads the directory once: a model that the engine refuses, or whose settings are missing, is
// left out and logged.
void WakeModelStore::scan() {
  models_.clear();
  DIR* dir = ::opendir(directory_.c_str());
  if (!dir) return;
  std::vector<std::string> names;
  while (const dirent* entry = ::readdir(dir)) {
    const std::string file = entry->d_name;
    const std::size_t suffix = std::char_traits<char>::length(kModelSuffix);
    if (file.size() <= suffix || file.compare(file.size() - suffix, suffix, kModelSuffix) != 0) continue;
    const std::string name = file.substr(0, file.size() - suffix);
    if (validName(name) && name != bundleName_) names.push_back(name);
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    if (models_.size() >= kMaxModels) break;
    Settings settings;
    const auto file = std::make_unique<posix::MappedFile>(modelPath(name), kMaxModelBytes);
    std::string reason;
    std::size_t arena = 0;
    if (!file->valid() || !readSettings(settingsPath(name), settings) || settings.window <= 0) {
      logf("wake word: %s: the model or its settings cannot be read", name.c_str());
      continue;
    }
    if (!engineTakes(file->bytes(), settings.cutoff, static_cast<std::size_t>(settings.window), arena, reason)) {
      logf("wake word: %s: %s", name.c_str(), reason.c_str());
      continue;
    }
    models_[name] = {name, settings.phrase.empty() ? name : settings.phrase, settings.enabled, settings.sensitivity,
                     settings.cutoff, static_cast<std::size_t>(settings.window), file->bytes().size(), arena, false,
                     posix::sha256Hex(file->bytes()), modelPath(name), nextVersion_++};
  }
}

std::optional<WakeModelEntry> WakeModelStore::bundleEntry() const {
  if (!bundleValid_) return std::nullopt;
  WakeModelEntry entry = bundleStatic_;
  entry.enabled = bundle_.enabled && bundle_.enabled();
  entry.sensitivity = bundle_.sensitivity ? bundle_.sensitivity() : WakeSensitivity::Normal;
  return entry;
}

std::vector<WakeModelEntry> WakeModelStore::list() const {
  std::vector<WakeModelEntry> out;
  if (auto bundle = bundleEntry()) out.push_back(std::move(*bundle));
  for (const auto& [name, entry] : models_) out.push_back(entry);
  return out;
}

std::optional<WakeModelEntry> WakeModelStore::find(const std::string& name) const {
  if (bundleValid_ && name == bundleName_) return bundleEntry();
  const auto it = models_.find(name);
  if (it == models_.end()) return std::nullopt;
  return it->second;
}

std::vector<WakeModelSpec> WakeModelStore::enabledModels() const {
  std::vector<WakeModelSpec> out;
  for (const auto& [name, entry] : models_)
    if (entry.enabled) out.push_back({name, entry.path, entry.sensitivity, entry.version});
  return out;
}

std::size_t WakeModelStore::enabledCount(const std::string& except) const {
  std::size_t count = 0;
  if (bundleValid_ && bundleName_ != except && bundle_.enabled && bundle_.enabled()) ++count;
  for (const auto& [name, entry] : models_) count += name != except && entry.enabled;
  return count;
}

// The fields applied to the current entry (or a new one), checked as the listener will check them.
WakeStoreError WakeModelStore::check(const std::string& name, const WakeModelFields& fields,
                                     const WakeModelEntry* current, WakeModelEntry& next) const {
  next = current ? *current : WakeModelEntry{name, name, true, WakeSensitivity::Normal, -1, 0, 0, 0, false, "",
                                             modelPath(name), 0};
  if (fields.phrase) {
    if (fields.phrase->empty() || fields.phrase->size() > kMaxPhraseBytes || !printable(*fields.phrase))
      return error(400, "validationFailed", "phrase must be 1 to 64 printable bytes", "phrase");
    next.phrase = *fields.phrase;
  }
  if (fields.cutoff) {
    if (!(*fields.cutoff > 0.0 && *fields.cutoff <= 1.0))
      return error(400, "validationFailed", "probabilityCutoff must be above 0 and at most 1", "probabilityCutoff");
    next.cutoff = *fields.cutoff;
  }
  if (fields.window) {
    if (*fields.window < 1 || *fields.window > static_cast<long long>(ProbabilityWindow::kMaxWindow))
      return error(400, "validationFailed", "slidingWindowSize must be within 1..64", "slidingWindowSize");
    next.window = static_cast<std::size_t>(*fields.window);
  }
  if (fields.enabled) next.enabled = *fields.enabled;
  if (fields.sensitivity) next.sensitivity = *fields.sensitivity;
  if (next.cutoff <= 0)
    return error(400, "validationFailed", "probabilityCutoff is required for a new model", "probabilityCutoff");
  if (next.window == 0)
    return error(400, "validationFailed", "slidingWindowSize is required for a new model", "slidingWindowSize");
  if (next.enabled && (!current || !current->enabled) && enabledCount(name) >= kMaxEnabled)
    return error(409, "tooManyModels", "at most 4 wake word models listen at the same time; disable one first",
                 "enabled");
  return {};
}

bool WakeModelStore::writeSettings(const WakeModelEntry& entry) const {
  std::string json;
  api::JsonWriter writer(json);
  writer.beginObject()
      .member("wake_word", entry.phrase)
      .member("probability_cutoff", entry.cutoff)
      .member("sliding_window_size", static_cast<unsigned long long>(entry.window))
      .member("feature_step_size", static_cast<unsigned long long>(WakeFrontend::kStepMs))
      .member("enabled", entry.enabled)
      .member("sensitivity", std::string(wakeSensitivityName(entry.sensitivity)))
      .endObject();
  json += '\n';
  return posix::replaceText(settingsPath(entry.name), json);
}

bool WakeModelStore::room(uint64_t bytes) const {
  struct statvfs stats {};
  if (::statvfs(directory_.c_str(), &stats) != 0) return false;
  const uint64_t free = static_cast<uint64_t>(stats.f_bavail) * stats.f_frsize;
  return free >= bytes + reserveBytes_;
}

WakeStoreError WakeModelStore::put(const std::string& name, std::string_view model, const WakeModelFields& fields,
                                   WakeModelEntry& out) {
  if (!validName(name)) return error(400, "invalidName", "a model name is 1 to 32 of a-z, 0-9 and _");
  if (bundleValid_ && name == bundleName_) return error(403, "readOnlyModel", name + " comes with the firmware");
  const auto current = models_.find(name);
  const WakeModelEntry* existing = current == models_.end() ? nullptr : &current->second;
  if (!existing && models_.size() >= kMaxModels)
    return error(409, "tooManyModels", "the clock keeps at most 8 wake word models; delete one first");
  if (model.empty() || model.size() > kMaxModelBytes)
    return error(413, "payloadTooLarge", "a model takes 1 byte to 512 KiB", "tflite");
  WakeModelEntry next;
  if (WakeStoreError refused = check(name, fields, existing, next)) return refused;
  std::string reason;
  std::size_t arena = 0;
  if (!engineTakes(model, next.cutoff, next.window, arena, reason))
    return error(422, "invalidModel", reason, "tflite");
  const uint64_t replaced = existing ? existing->bytes : 0;
  if (!posix::makeDirectories(directory_, 0700)) {
    logf("wake word: cannot create %s: %s", directory_.c_str(), std::strerror(errno));
    return error(500, "internalError", "the model directory cannot be created");
  }
  if (model.size() > replaced && !room(model.size() - replaced + kSettingsLimit))
    return error(507, "insufficientStorage", "not enough free space for the model");
  if (!posix::replaceText(modelPath(name), model) || !writeSettings(next)) {
    logf("wake word: cannot store %s: %s", name.c_str(), std::strerror(errno));
    return error(507, "insufficientStorage", "the model could not be stored");
  }
  next.bytes = model.size();
  next.arenaBytes = arena;
  next.sha256 = posix::sha256Hex(model);
  next.version = nextVersion_++;
  models_[name] = next;
  ++revision_;
  logf("wake word: stored %s (%zu bytes, arena %zu bytes)", name.c_str(), model.size(), arena);
  out = next;
  return {};
}

WakeStoreError WakeModelStore::patch(const std::string& name, const WakeModelFields& fields, WakeModelEntry& out) {
  if (bundleValid_ && name == bundleName_) {
    if (fields.phrase || fields.cutoff || fields.window)
      return error(403, "readOnlyModel", name + " comes with the firmware: only enabled and sensitivity change");
    if (fields.enabled && *fields.enabled && !(bundle_.enabled && bundle_.enabled()) &&
        enabledCount(name) >= kMaxEnabled)
      return error(409, "tooManyModels", "at most 4 wake word models listen at the same time; disable one first",
                   "enabled");
    if ((fields.enabled || fields.sensitivity) && !(bundle_.set && bundle_.set(fields.enabled, fields.sensitivity)))
      return error(500, "internalError", "the voice settings could not be saved");
    out = *bundleEntry();
    return {};
  }
  const auto current = models_.find(name);
  if (current == models_.end()) return error(404, "notFound", "no wake word model " + name);
  WakeModelEntry next;
  if (WakeStoreError refused = check(name, fields, &current->second, next)) return refused;
  const bool reload = next.cutoff != current->second.cutoff || next.window != current->second.window;
  if (reload) {
    const auto file = std::make_unique<posix::MappedFile>(next.path, kMaxModelBytes);
    std::string reason;
    std::size_t arena = 0;
    if (!file->valid() || !engineTakes(file->bytes(), next.cutoff, next.window, arena, reason))
      return error(422, "invalidModel", file->valid() ? reason : "the model file cannot be read");
  }
  if (!writeSettings(next)) return error(507, "insufficientStorage", "the settings could not be stored");
  if (reload) next.version = nextVersion_++;
  current->second = next;
  ++revision_;
  out = next;
  return {};
}

WakeStoreError WakeModelStore::remove(const std::string& name) {
  if (bundleValid_ && name == bundleName_) return error(403, "readOnlyModel", name + " comes with the firmware");
  const auto current = models_.find(name);
  if (current == models_.end()) return error(404, "notFound", "no wake word model " + name);
  ::unlink(settingsPath(name).c_str());
  if (::unlink(modelPath(name).c_str()) != 0 && errno != ENOENT)
    return error(500, "internalError", "the model could not be removed");
  posix::fsyncDirectory(directory_);
  models_.erase(current);
  ++revision_;
  logf("wake word: removed %s", name.c_str());
  return {};
}

bool WakeModelStore::eraseAll() {
  bool ok = true;
  for (const auto& [name, entry] : models_) {
    ::unlink(settingsPath(name).c_str());
    ok = (::unlink(modelPath(name).c_str()) == 0 || errno == ENOENT) && ok;
  }
  models_.clear();
  ++revision_;
  return ok;
}

}
