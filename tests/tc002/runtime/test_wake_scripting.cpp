#include "../../support.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/apps/AppRegistry.h"
#include "core/script/ScriptApp.h"
#include "core/script/ScriptHost.h"
#include "platform/linux/script/ExtensionHost.h"
#include "platform/tc002/wake/WakeScripting.h"

using namespace awtrix;
using awtrix::test::check;
using awtrix::test::require;
using tc002::wake::WakeScripting;

namespace {

// Two apps that hear the wake word, one that does not, and the real script host between them.
constexpr const char* kListener = R"BERRY(
class Listener
  var log
  def init() self.log = '' end
  def on_wake(name, score)
    self.log += name + ' ' + str(score) + ' ' + type(score) + ';'
  end
  def check() return self.log end
  def draw() end
end
return Listener()
)BERRY";

constexpr const char* kModels = R"BERRY(
import wake
class Lister
  def check() return wake.models().concat(',') end
  def draw() end
end
return Lister()
)BERRY";

constexpr const char* kDeaf = R"BERRY(
class Deaf
  def draw() end
end
return Deaf()
)BERRY";

struct Rig {
  std::map<std::string, std::string> sources;
  std::vector<std::string> models{"hey_awtrix", "gute_nacht"};
  WakeScripting wake{WakeScripting::Services{
      [this] {
        std::vector<std::string> names;
        for (const auto& [name, source] : sources) names.push_back(name);
        return names;
      },
      [this](const std::string& app, std::string& out) {
        const auto it = sources.find(app);
        if (it == sources.end()) return false;
        out = it->second;
        return true;
      },
      [this] { return models; }}};
  AppRegistry registry;
  script::ScriptServices services;
  script::ScriptHost host{registry, services, nullptr, nullptr};
  std::unique_ptr<script::ExtensionHost> extensions =
      std::make_unique<script::ExtensionHost>(host, std::vector<script::ScriptExtension*>{&wake});

  void install(const std::string& name, const std::string& source) {
    sources[name] = source;
    require(host.set(name, source) && host.errorOf(name).empty(), "the app loads: " + name);
  }
  std::string check(const std::string& name) {
    std::string out;
    if (auto* app = static_cast<script::ScriptApp*>(registry.find(name))) app->callCheckForTest(out);
    return out;
  }
  void frame() {
    RenderCtx ctx;
    host.tick(ctx, "A");
  }
};

void everyAppWithTheHookHearsIt() {
  Rig rig;
  rig.install("A", kListener);
  rig.install("B", kListener);
  rig.install("C", kDeaf);
  rig.wake.heard("gute_nacht", 0.97f);
  rig.frame();
  check(rig.check("A") == "gute_nacht 0.97 real;", "the app on display hears it: " + rig.check("A"));
  check(rig.check("B") == "gute_nacht 0.97 real;", "an app that is not shown hears it too: " + rig.check("B"));
  check(rig.host.errorOf("C").empty(), "an app without on_wake is left alone");
  rig.frame();
  check(rig.check("A") == "gute_nacht 0.97 real;", "each detection arrives once");
  for (int i = 0; i < 12; ++i) rig.wake.heard("hey_awtrix", 0.5f);
  rig.frame();
  rig.frame();
  check(rig.check("B").size() == std::string("gute_nacht 0.97 real;").size() + 12 * std::string("hey_awtrix 0.5 real;").size(),
        "detections of a busy tick wait for the next: " + rig.check("B"));
}

void theModuleListsTheModels() {
  Rig rig;
  rig.install("L", kModels);
  check(rig.check("L") == "hey_awtrix,gute_nacht", "wake.models() names the listening models: " + rig.check("L"));
}

}

int main() {
  everyAppWithTheHookHearsIt();
  theModuleListsTheModels();
  return awtrix::test::finish("wake scripting");
}
