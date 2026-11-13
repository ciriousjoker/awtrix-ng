#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/tc002/wake/WakeListener.h"
#include "platform/tc002/wake/WakeSensitivity.h"

namespace awtrix::tc002::wake {

struct WakeModelEntry {
  std::string name, phrase;
  bool enabled = false;
  WakeSensitivity sensitivity = WakeSensitivity::Normal;
  double cutoff = 0;
  std::size_t window = 0;
  uint64_t bytes = 0;
  std::size_t arenaBytes = 0;
  bool bundle = false;
  std::string sha256;
  std::string path;
  uint64_t version = 0;
};

// What a PUT or PATCH names; an absent member keeps the current value.
struct WakeModelFields {
  std::optional<std::string> phrase;
  std::optional<double> cutoff;
  std::optional<long long> window;
  std::optional<bool> enabled;
  std::optional<WakeSensitivity> sensitivity;
};

// An HTTP status and the error body's code, message and field; status 0 is success.
struct WakeStoreError {
  int status = 0;
  std::string code, message, field;
  explicit operator bool() const { return status != 0; }
};

// The wake word models: the read-only one of the release, whose switch and level are the Voice
// settings, and those in a writable directory as NAME.tflite with NAME.json, the microWakeWord
// settings plus "enabled" and "sensitivity". A model replaces nothing until the engine took it.
class WakeModelStore {
 public:
  static constexpr std::size_t kMaxModels = 8, kMaxEnabled = 4, kMaxModelBytes = 512 * 1024, kMaxPhraseBytes = 64;

  struct Bundle {
    std::string path;
    std::function<bool()> enabled;
    std::function<WakeSensitivity()> sensitivity;
    std::function<bool(std::optional<bool> enabled, std::optional<WakeSensitivity> level)> set;
  };

  // reserveBytes: the free space a write must leave on the directory's file system.
  WakeModelStore(std::string directory, Bundle bundle, uint64_t reserveBytes);

  static bool validName(std::string_view name);
  std::vector<WakeModelEntry> list() const;
  std::optional<WakeModelEntry> find(const std::string& name) const;
  WakeStoreError put(const std::string& name, std::string_view model, const WakeModelFields& fields,
                     WakeModelEntry& out);
  WakeStoreError patch(const std::string& name, const WakeModelFields& fields, WakeModelEntry& out);
  WakeStoreError remove(const std::string& name);
  bool eraseAll();

  // The enabled models of the directory, for the listener.
  std::vector<WakeModelSpec> enabledModels() const;
  const std::string& bundleName() const { return bundleName_; }
  // Changes with every change of a model of the directory.
  uint64_t revision() const { return revision_; }

 private:
  void scan();
  std::optional<WakeModelEntry> bundleEntry() const;
  std::size_t enabledCount(const std::string& except) const;
  WakeStoreError check(const std::string& name, const WakeModelFields& fields, const WakeModelEntry* current,
                       WakeModelEntry& next) const;
  bool writeSettings(const WakeModelEntry& entry) const;
  std::string modelPath(const std::string& name) const;
  std::string settingsPath(const std::string& name) const;
  bool room(uint64_t bytes) const;

  std::string directory_;
  Bundle bundle_;
  std::string bundleName_;
  WakeModelEntry bundleStatic_;
  bool bundleValid_ = false;
  uint64_t reserveBytes_ = 0;
  std::map<std::string, WakeModelEntry> models_;
  uint64_t revision_ = 1, nextVersion_ = 1;
};

}
