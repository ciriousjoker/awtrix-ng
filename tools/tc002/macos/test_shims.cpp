// The one engine function the wake word routes need from src/core/api/ApiRouter.cpp, whose other
// dependencies the macOS test build does not compile. Same output as the engine's.
#include <string>

#include "core/api/JsonWriter.h"

namespace awtrix::api {

std::string errorJson(const char* code, const std::string& message, const std::string& field) {
  std::string out = "{\"error\":{\"code\":\"";
  out += code;
  out += "\",\"message\":";
  appendJsonString(out, message);
  if (!field.empty()) {
    out += ",\"field\":";
    appendJsonString(out, field);
  }
  out += "}}";
  return out;
}

}
