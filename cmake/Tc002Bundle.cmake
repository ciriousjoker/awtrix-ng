include_guard(GLOBAL)

# Installs the AWTRIX programs and assets for the tc002 bundle component.
# The tc002-loader preset adds libawtrix-loader.so; build_bundle.sh adds third-party content.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# The web UI's update check: its release feed and OTA index, upstream's unless set; with
# AWTRIX_WEBUI_UPDATE_CHECK off the web UI offers no check (a product that updates its clocks itself).
option(AWTRIX_WEBUI_UPDATE_CHECK "Let the TC002 web UI check for firmware updates" ON)
set(AWTRIX_WEBUI_UPDATE_API "" CACHE STRING "Release feed of the web UI's update check (empty: upstream's)")
set(AWTRIX_WEBUI_UPDATE_OTA "" CACHE STRING "OTA index of the web UI's update check (empty: upstream's)")
set(TC002_WEBUI_OPTIONS)
if(NOT AWTRIX_WEBUI_UPDATE_CHECK)
  list(APPEND TC002_WEBUI_OPTIONS "--update-api=")
elseif(AWTRIX_WEBUI_UPDATE_API)
  list(APPEND TC002_WEBUI_OPTIONS "--update-api=${AWTRIX_WEBUI_UPDATE_API}")
endif()
if(AWTRIX_WEBUI_UPDATE_OTA)
  list(APPEND TC002_WEBUI_OPTIONS "--update-ota=${AWTRIX_WEBUI_UPDATE_OTA}")
endif()
set(TC002_WEBUI "${PROJECT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT "${TC002_WEBUI}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/tc002/install/bundle.py" webui
          "${PROJECT_SOURCE_DIR}" "${TC002_WEBUI}" ${TC002_WEBUI_OPTIONS}
  DEPENDS "${PROJECT_SOURCE_DIR}/webui/index.html" "${PROJECT_SOURCE_DIR}/src/transport/http/WebUiAsset.h"
          "${PROJECT_SOURCE_DIR}/tools/tc002/install/bundle.py"
  VERBATIM)
add_custom_target(tc002-webui ALL DEPENDS "${TC002_WEBUI}")

install(TARGETS awtrix-linux awtrix-tc002d awtrix-tc002-dhcp-callback awtrix-tc002-audio-pcm awtrix-tc002-flash
  RUNTIME DESTINATION bin COMPONENT tc002 EXCLUDE_FROM_ALL)
install(FILES "${TC002_WEBUI}" "${PROJECT_SOURCE_DIR}/assets/tc002/boot.mp3" DESTINATION share
  COMPONENT tc002 EXCLUDE_FROM_ALL)
install(FILES "${PROJECT_SOURCE_DIR}/assets/tc002/wake/hey_awtrix.tflite"
  "${PROJECT_SOURCE_DIR}/assets/tc002/wake/hey_awtrix.json" DESTINATION share/wake COMPONENT tc002 EXCLUDE_FROM_ALL)
install(FILES "${PROJECT_SOURCE_DIR}/assets/tc002/vad/vad.tflite" "${PROJECT_SOURCE_DIR}/assets/tc002/vad/vad.json"
  "${PROJECT_SOURCE_DIR}/assets/tc002/vad/NOTICE" DESTINATION share/vad COMPONENT tc002 EXCLUDE_FROM_ALL)
set(TC002_SPEECH_VOICE "${PROJECT_SOURCE_DIR}/assets/speech/voice.atts")
if(EXISTS "${TC002_SPEECH_VOICE}")
  install(FILES "${TC002_SPEECH_VOICE}" DESTINATION share/speech COMPONENT tc002 EXCLUDE_FROM_ALL)
endif()
