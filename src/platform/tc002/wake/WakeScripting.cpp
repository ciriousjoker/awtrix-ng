#include "platform/tc002/wake/WakeScripting.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "berry.h"
#include "core/script/BerryVM.h"

namespace awtrix::tc002::wake {
namespace {

// Instances have no "has method" test in the sandbox (no introspect), so the dispatcher reads
// the member and treats an attribute_error as "this app has no on_wake". The hook runs outside
// the try, so an error in it is the app's own.
constexpr const char* kSource = R"BERRY(
import global
import string
var m = module('wake')
global._wake_dispatch = def (app, name, score)
  var inst = _app_instance(app)
  if inst == nil return end
  var hook
  try
    hook = inst.on_wake
  except 'attribute_error'
    return
  end
  if type(hook) == 'function' hook(inst, name, real(score)) end
end
m.models = def ()
  var out = []
  for name : string.split(_wake_models(), ' ') if size(name) > 0 out.push(name) end end
  return out
end
return m
)BERRY";

// Detections a busy script tick has not delivered yet; older ones go first when it overflows.
constexpr std::size_t kQueueLimit = 32;

WakeScripting& self(bvm* vm) { return *static_cast<WakeScripting*>(script::BerryVM::nativeSelf(vm)); }

}

void WakeScripting::install(script::ScriptExtensionHost& host) {
  host.defineNative("_wake_models", &WakeScripting::nModels, this);
  installed_ = host.defineModule(kName, kSource);
}

void WakeScripting::heard(const std::string& model, float score) {
  char text[16];
  std::snprintf(text, sizeof text, "%.3f", static_cast<double>(score));
  if (heard_.size() >= kQueueLimit) heard_.pop_front();
  heard_.push_back({model, text});
}

// Only apps whose source names on_wake are called: an app without it costs nothing, and a call
// into an app that fails marks the app broken.
void WakeScripting::tick(script::ScriptExtensionHost& host, const RenderCtx* ctx) {
  if (heard_.empty()) return;
  if (!installed_) {
    heard_.clear();
    return;
  }
  std::vector<std::string> apps;
  if (services_.apps)
    for (std::string& app : services_.apps()) {
      std::string source;
      if (services_.source && services_.source(app, source) && source.find(kHook) != std::string::npos)
        apps.push_back(std::move(app));
    }
  for (std::size_t due = std::min(heard_.size(), kPerTick); due > 0; --due) {
    const Heard heard = std::move(heard_.front());
    heard_.pop_front();
    for (const std::string& app : apps) host.deliver(app, kHook, "_wake_dispatch", app, heard.model, heard.score, ctx);
  }
}

int WakeScripting::nModels(bvm* vm) {
  std::string names;
  const WakeScripting& wake = self(vm);
  if (wake.services_.models)
    for (const std::string& name : wake.services_.models()) names += (names.empty() ? "" : " ") + name;
  be_pushstring(vm, names.c_str());
  be_return(vm);
}

}
