#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "core/script/ScriptExtension.h"

typedef struct bvm bvm;

namespace awtrix::tc002::wake {

// The scripts' side of the wake word: every unmuted detection reaches on_wake(name, score) of
// every running app that defines it, shown or not; `import wake` lists the listening models.
class WakeScripting : public script::ScriptExtension {
 public:
  static constexpr const char* kName = "wake";
  static constexpr const char* kHook = "on_wake";
  static constexpr std::size_t kPerTick = 8;

  struct Services {
    std::function<std::vector<std::string>()> apps;
    std::function<std::vector<std::string>()> models;
  };

  explicit WakeScripting(Services services) : services_(std::move(services)) {}

  std::vector<std::string> modules() const override { return {kName}; }
  void install(script::ScriptExtensionHost& host) override;
  void tick(script::ScriptExtensionHost& host, const RenderCtx* ctx) override;

  void heard(const std::string& model, float score);

 private:
  struct Heard {
    std::string model, score;
  };
  static int nModels(bvm* vm);

  Services services_;
  std::deque<Heard> heard_;
};

}
