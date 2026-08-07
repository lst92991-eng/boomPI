include(GNUInstallDirs)

set(_boompi_ui_simulator_default ON)
if(BOOMPI_TARGET_RV1106)
  set(_boompi_ui_simulator_default OFF)
endif()
option(BOOMPI_BUILD_UI_SIMULATOR "Build the 320x240 LVGL desktop preview"
  ${_boompi_ui_simulator_default})
option(BOOMPI_BUILD_AEC_LOOP_HIL
  "Build the opt-in RV1106 local-playback AEC regression probe" OFF)
option(BOOMPI_REQUIRE_HOST_TRANSPORT_TEST
  "Fail host configuration unless the real VoiceTransport tests can run" OFF)
option(BOOMPI_AEC_LOOP_ENABLE_STDT
  "Enable vendor STDT in the opt-in AEC regression probe" ON)
option(BOOMPI_AEC_LOOP_ACTIVE_PROBE
  "Exercise the application mute-and-recheck barge-in guard in the AEC probe" OFF)
set(BOOMPI_AEC_LOOP_DELAY_SAMPLES "0" CACHE STRING
  "Fixed Rockchip AEC reference delay used by the opt-in probe (0 or 256)")
set(BOOMPI_OPENSSL_ROOT "" CACHE PATH "Pinned OpenSSL 3.5.7 package root")
set(BOOMPI_BOOST_INCLUDE_DIR "" CACHE PATH "External Boost header root")
set(BOOMPI_WEBRTC_VAD_INCLUDE_DIR "" CACHE PATH "Directory containing webrtc_vad.h")
set(BOOMPI_WEBRTC_VAD_LIBRARY "" CACHE FILEPATH "RV1106 WebRTC VAD archive")
set(BOOMPI_LVGL_ROOT "" CACHE PATH "Validated LVGL 8.2 source root")
if(BOOMPI_LVGL_ROOT STREQUAL "" AND DEFINED ENV{BOOMPI_LVGL_ROOT})
  file(TO_CMAKE_PATH "$ENV{BOOMPI_LVGL_ROOT}" BOOMPI_LVGL_ROOT)
endif()

if(BOOMPI_TARGET_RV1106 OR BOOMPI_BUILD_UI_SIMULATOR)
  find_package(Threads REQUIRED)
  find_package(Freetype REQUIRED)
  include(cmake/BoompiLvgl.cmake)
  boompi_configure_lvgl()

  add_library(boompi_lvgl_ui STATIC
    src/ui/lvgl_screen.cpp
    assets/xiaozhi/emoji_1f606_64.c
    assets/xiaozhi/emoji_1f614_64.c
    assets/xiaozhi/emoji_1f62f_64.c
    assets/xiaozhi/emoji_1f642_64.c
    assets/xiaozhi/emoji_1f914_64.c)
  target_compile_features(boompi_lvgl_ui PRIVATE cxx_std_17)
  target_compile_options(boompi_lvgl_ui PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${BOOMPI_STRICT_WARNINGS}>>:-Werror>)
  target_include_directories(boompi_lvgl_ui PUBLIC include PRIVATE
    assets/xiaozhi)
  target_link_libraries(boompi_lvgl_ui PUBLIC lvgl::lvgl Freetype::Freetype)
endif()

if(BOOMPI_BUILD_UI_SIMULATOR)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  add_executable(boompi_ui_simulator apps/boompi_ui_simulator/main.cpp)
  target_compile_features(boompi_ui_simulator PRIVATE cxx_std_17)
  target_compile_options(boompi_ui_simulator PRIVATE -Wall -Wextra -Wpedantic
    $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
  target_link_libraries(boompi_ui_simulator PRIVATE boompi_lvgl_ui
    PkgConfig::SDL2 Threads::Threads)
  set_target_properties(boompi_ui_simulator PROPERTIES OUTPUT_NAME boompi-ui-simulator)
endif()

if(BOOMPI_BUILD_TESTS AND NOT BOOMPI_TARGET_RV1106)
  if(NOT TARGET Threads::Threads)
    find_package(Threads REQUIRED)
  endif()
  add_executable(boompi_voice_client_config_test
    tests/voice_client_config_test.cpp
    src/config/voice_client_config.cpp)
  target_compile_features(boompi_voice_client_config_test PRIVATE cxx_std_17)
  target_include_directories(boompi_voice_client_config_test PRIVATE include)
  if(MSVC)
    target_compile_definitions(boompi_voice_client_config_test PRIVATE
      _CRT_SECURE_NO_WARNINGS)
    target_compile_options(boompi_voice_client_config_test PRIVATE /W4 /utf-8
      $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:/WX>)
  else()
    target_compile_options(boompi_voice_client_config_test PRIVATE
      -Wall -Wextra -Wpedantic
      $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
  endif()
  add_test(NAME voice-client-config-contract
    COMMAND boompi_voice_client_config_test)

  find_package(OpenSSL QUIET)
  find_path(BOOMPI_HOST_BOOST_INCLUDE_DIR NAMES boost/asio.hpp)
  find_path(BOOMPI_HOST_CJSON_INCLUDE_DIR NAMES cjson/cJSON.h)
  find_library(BOOMPI_HOST_CJSON_LIBRARY NAMES cjson libcjson)
  if(OpenSSL_FOUND AND BOOMPI_HOST_BOOST_INCLUDE_DIR AND
      BOOMPI_HOST_CJSON_INCLUDE_DIR AND BOOMPI_HOST_CJSON_LIBRARY)
    add_executable(boompi_voice_transport_loopback_test
      tests/voice_transport_loopback_test.cpp
      src/network/voice_transport.cpp
      src/config/voice_client_config.cpp)
    add_executable(boompi_protocol_json_test
      tests/protocol_json_contract.cpp
      src/config/voice_client_config.cpp)
    foreach(_target IN ITEMS boompi_voice_transport_loopback_test
        boompi_protocol_json_test)
      target_compile_features(${_target} PRIVATE cxx_std_17)
      target_compile_definitions(${_target} PRIVATE
        BOOST_ERROR_CODE_HEADER_ONLY=1 BOOST_SYSTEM_NO_DEPRECATED=1)
      target_include_directories(${_target} PRIVATE include)
      target_include_directories(${_target} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party"
        "${BOOMPI_HOST_BOOST_INCLUDE_DIR}"
        "${BOOMPI_HOST_CJSON_INCLUDE_DIR}")
      if(MSVC)
        target_compile_definitions(${_target} PRIVATE
          _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${_target} PRIVATE
          /W4 /utf-8 $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:/WX>)
      else()
        target_compile_options(${_target} PRIVATE
          -Wall -Wextra -Wpedantic
          $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
      endif()
      target_link_libraries(${_target} PRIVATE
        "${BOOMPI_HOST_CJSON_LIBRARY}" OpenSSL::SSL OpenSSL::Crypto
        Threads::Threads ${CMAKE_DL_LIBS})
    endforeach()
    add_test(NAME voice-transport-loopback
      COMMAND boompi_voice_transport_loopback_test)
    add_test(NAME protocol-json-contract
      COMMAND boompi_protocol_json_test)
  else()
    if(BOOMPI_REQUIRE_HOST_TRANSPORT_TEST)
      message(FATAL_ERROR
        "Real VoiceTransport tests require host OpenSSL, Boost, and cJSON development files")
    endif()
    message(STATUS
      "Real VoiceTransport tests skipped: host OpenSSL, Boost, and cJSON development files are required")
  endif()

  add_executable(boompi_voice_client_harness
    tests/voice_client_harness_test.cpp
    tests/support/voice_client_harness.cpp
    src/application/voice_client.cpp)
  target_compile_features(boompi_voice_client_harness PRIVATE cxx_std_17)
  target_compile_definitions(boompi_voice_client_harness PRIVATE
    BOOMPI_VOICE_CLIENT_HARNESS=1)
  if(MSVC)
    target_compile_options(boompi_voice_client_harness PRIVATE /W4 /utf-8
      $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:/WX>)
  else()
    target_compile_options(boompi_voice_client_harness PRIVATE
      -Wall -Wextra -Wpedantic
      $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
  endif()
  target_include_directories(boompi_voice_client_harness PRIVATE
    include tests)
  target_link_libraries(boompi_voice_client_harness PRIVATE Threads::Threads)
  foreach(_scenario IN ITEMS explicit-endpoint capture-hole touch-wake-stale-vad
      hello-timeout hello-frame-timeout
      heartbeat-timeout heartbeat-frame-timeout
      capture-wall-timeout cancel-timeout cancel-frame-timeout cancel-late-ack
      follow-up-timeout follow-up-frame-timeout
      drain-touch drain-barge drain-timeout drain-complete-timeout
      drain-complete-frame
      drain-discontinuity cancel-discontinuity
      barge-turn-discontinuity first-response-hint first-response-content
      stop-stalled-capture
      backpressure reconnect connection-subtitle abnormal-subtitle barge
      barge-short-before-ack barge-long-across-ack barge-delayed-ack
      cancel-done stale-epoch session-error-after-turn
      downlink-sequence-gap max-input)
    add_test(NAME voice-client-${_scenario}
      COMMAND boompi_voice_client_harness ${_scenario})
  endforeach()

  if(UNIX)
    add_executable(boompi_audio_engine_harness
      tests/audio_engine_harness_test.cpp
      tests/support/audio_backend.cpp
      src/audio/audio_engine.cpp)
    target_compile_features(boompi_audio_engine_harness PRIVATE cxx_std_17)
    target_compile_options(boompi_audio_engine_harness PRIVATE
      -Wall -Wextra -Wpedantic
      $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
    # support must precede the board backend directory so audio_engine.cpp
    # binds to the deterministic fake while the production target stays intact.
    target_include_directories(boompi_audio_engine_harness PRIVATE
      tests/support include)
    target_link_libraries(boompi_audio_engine_harness PRIVATE Threads::Threads)
    foreach(_scenario IN ITEMS sub-grace-jitter confirmed-gap short-tail
        bounded-close bounded-capture bounded-command reset-preserves-pcm
        queue-results open-clears-error playback-clears-error)
      add_test(NAME audio-engine-${_scenario}
        COMMAND boompi_audio_engine_harness ${_scenario})
    endforeach()
  endif()
endif()

if(NOT BOOMPI_TARGET_RV1106)
  message(STATUS "boompi-client is a board-only target; configure with BOOMPI_TARGET_RV1106=ON")
  return()
endif()

foreach(_target IN ITEMS boompi_vendor::rockchip_3a_aec boompi_vendor::snowboy)
  if(NOT TARGET ${_target})
    message(FATAL_ERROR "Missing validated vendor target ${_target}")
  endif()
endforeach()

find_package(ALSA REQUIRED)
find_library(BOOMPI_SWRESAMPLE_LIBRARY NAMES swresample REQUIRED)
find_library(BOOMPI_AVUTIL_LIBRARY NAMES avutil REQUIRED)
find_path(BOOMPI_CJSON_INCLUDE_DIR NAMES cjson/cJSON.h REQUIRED)
find_library(BOOMPI_CJSON_LIBRARY NAMES cjson REQUIRED)
if(NOT IS_ABSOLUTE "${BOOMPI_CJSON_INCLUDE_DIR}" OR
   NOT EXISTS "${BOOMPI_CJSON_INCLUDE_DIR}/cjson/cJSON.h" OR
   NOT IS_ABSOLUTE "${BOOMPI_CJSON_LIBRARY}" OR
   NOT EXISTS "${BOOMPI_CJSON_LIBRARY}")
  message(FATAL_ERROR "RV1106 BSP cJSON headers/library are missing")
endif()

if(NOT IS_ABSOLUTE "${BOOMPI_BOOST_INCLUDE_DIR}" OR
   NOT EXISTS "${BOOMPI_BOOST_INCLUDE_DIR}/boost/asio.hpp")
  message(FATAL_ERROR "BOOMPI_BOOST_INCLUDE_DIR must be an explicit Boost header root")
endif()
if(NOT IS_ABSOLUTE "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}" OR
   NOT EXISTS "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}/webrtc_vad.h" OR
   NOT IS_ABSOLUTE "${BOOMPI_WEBRTC_VAD_LIBRARY}" OR
   NOT EXISTS "${BOOMPI_WEBRTC_VAD_LIBRARY}")
  message(FATAL_ERROR "WebRTC VAD headers/archive are missing")
endif()

if(NOT IS_ABSOLUTE "${BOOMPI_OPENSSL_ROOT}" OR NOT IS_DIRECTORY "${BOOMPI_OPENSSL_ROOT}")
  message(FATAL_ERROR "BOOMPI_OPENSSL_ROOT must be an explicit absolute package root")
endif()
file(REAL_PATH "${BOOMPI_OPENSSL_ROOT}" BOOMPI_OPENSSL_ROOT)
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(OpenSSL_DIR "${BOOMPI_OPENSSL_ROOT}" CACHE PATH "Pinned OpenSSL package" FORCE)
find_package(OpenSSL 3.5.7 EXACT CONFIG REQUIRED PATHS "${BOOMPI_OPENSSL_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

add_library(boompi_webrtc_vad STATIC IMPORTED)
set_target_properties(boompi_webrtc_vad PROPERTIES IMPORTED_LOCATION "${BOOMPI_WEBRTC_VAD_LIBRARY}")

add_library(boompi_snowboy_bridge STATIC src/platform/rv1106/snowboy_legacy_bridge.cpp)
target_compile_features(boompi_snowboy_bridge PRIVATE cxx_std_17)
target_compile_definitions(boompi_snowboy_bridge PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(boompi_snowboy_bridge PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
target_include_directories(boompi_snowboy_bridge PRIVATE src/platform/rv1106)
target_include_directories(boompi_snowboy_bridge SYSTEM PRIVATE "${BOOMPI_SNOWBOY_INCLUDE_DIR}")
target_link_libraries(boompi_snowboy_bridge PRIVATE boompi_vendor::snowboy)

add_executable(boompi_client
  apps/boompi_client/main.cpp
  src/application/voice_client.cpp
  src/audio/audio_engine.cpp
  src/config/voice_client_config.cpp
  src/network/network_bootstrap.cpp
  src/network/voice_transport.cpp
  src/platform/rv1106/audio_backend.cpp
  src/platform/rv1106/rockchip_voice_dsp.cpp
  src/ui/device_ui.cpp)
target_compile_features(boompi_client PRIVATE cxx_std_17)
target_compile_definitions(boompi_client PRIVATE BOOMPI_TARGET_RV1106=1
  BOOST_ERROR_CODE_HEADER_ONLY=1 BOOST_SYSTEM_NO_DEPRECATED=1)
target_compile_options(boompi_client PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
target_include_directories(boompi_client PRIVATE include src/platform/rv1106)
target_include_directories(boompi_client SYSTEM PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party"
  "${BOOMPI_BOOST_INCLUDE_DIR}"
  "${BOOMPI_CJSON_INCLUDE_DIR}"
  "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}"
  "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}")
target_link_libraries(boompi_client PRIVATE
  boompi_lvgl_ui
  boompi_snowboy_bridge
  boompi_vendor::rockchip_3a_aec
  boompi_webrtc_vad
  ALSA::ALSA
  "${BOOMPI_SWRESAMPLE_LIBRARY}"
  "${BOOMPI_AVUTIL_LIBRARY}"
  "${BOOMPI_CJSON_LIBRARY}"
  Freetype::Freetype
  OpenSSL::SSL OpenSSL::Crypto
  Threads::Threads ${CMAKE_DL_LIBS} atomic)
target_link_options(boompi_client PRIVATE
  $<$<CONFIG:Release>:-Wl,--strip-all>)
set_target_properties(boompi_client PROPERTIES OUTPUT_NAME boompi-client
  SKIP_BUILD_RPATH TRUE BUILD_RPATH "" INSTALL_RPATH ""
  BUILD_WITH_INSTALL_RPATH FALSE INSTALL_RPATH_USE_LINK_PATH FALSE)

add_executable(boompi_protocol_json_test EXCLUDE_FROM_ALL
  tests/protocol_json_contract.cpp
  src/config/voice_client_config.cpp)
target_compile_features(boompi_protocol_json_test PRIVATE cxx_std_17)
target_compile_definitions(boompi_protocol_json_test PRIVATE
  BOOST_ERROR_CODE_HEADER_ONLY=1 BOOST_SYSTEM_NO_DEPRECATED=1)
target_compile_options(boompi_protocol_json_test PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
target_include_directories(boompi_protocol_json_test PRIVATE include)
target_include_directories(boompi_protocol_json_test SYSTEM PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party" "${BOOMPI_BOOST_INCLUDE_DIR}"
  "${BOOMPI_CJSON_INCLUDE_DIR}")
target_link_libraries(boompi_protocol_json_test PRIVATE
  "${BOOMPI_CJSON_LIBRARY}" OpenSSL::SSL OpenSSL::Crypto
  Threads::Threads ${CMAKE_DL_LIBS} atomic)
install(TARGETS boompi_client RUNTIME DESTINATION /userdata/boompi/bin)
install(PROGRAMS scripts/boompi-clientctl scripts/boompi-provision
  DESTINATION /usr/sbin)
install(PROGRAMS scripts/boompi-provision.py DESTINATION /usr/lib/boompi)
install(PROGRAMS scripts/S99boompi-client DESTINATION /etc/init.d)

if(BOOMPI_BUILD_AEC_LOOP_HIL)
  if(NOT BOOMPI_AEC_LOOP_DELAY_SAMPLES MATCHES "^(0|256)$")
    message(FATAL_ERROR "BOOMPI_AEC_LOOP_DELAY_SAMPLES must be 0 or 256")
  endif()
  add_executable(boompi_aec_loop_hil EXCLUDE_FROM_ALL
    tests/hil/aec_loop_hil.cpp
    src/audio/audio_engine.cpp
    src/config/voice_client_config.cpp
    src/platform/rv1106/audio_backend.cpp
    src/platform/rv1106/rockchip_voice_dsp.cpp)
  target_compile_features(boompi_aec_loop_hil PRIVATE cxx_std_17)
  target_compile_definitions(boompi_aec_loop_hil PRIVATE
    BOOMPI_TARGET_RV1106=1
    BOOMPI_AEC_LOOP_DIAGNOSTICS=1
    BOOMPI_AEC_LOOP_ACTIVE_PROBE=$<BOOL:${BOOMPI_AEC_LOOP_ACTIVE_PROBE}>
    BOOMPI_ROCKCHIP_ENABLE_STDT=$<BOOL:${BOOMPI_AEC_LOOP_ENABLE_STDT}>
    BOOMPI_ROCKCHIP_DELAY_SAMPLES=${BOOMPI_AEC_LOOP_DELAY_SAMPLES})
  target_compile_options(boompi_aec_loop_hil PRIVATE -Wall -Wextra -Wpedantic
    $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
  target_include_directories(boompi_aec_loop_hil PRIVATE
    include src/platform/rv1106)
  target_include_directories(boompi_aec_loop_hil SYSTEM PRIVATE
    "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}"
    "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}")
  target_link_libraries(boompi_aec_loop_hil PRIVATE
    boompi_snowboy_bridge
    boompi_vendor::rockchip_3a_aec
    boompi_webrtc_vad
    ALSA::ALSA
    "${BOOMPI_SWRESAMPLE_LIBRARY}"
    "${BOOMPI_AVUTIL_LIBRARY}"
    Threads::Threads ${CMAKE_DL_LIBS} atomic)
  set_target_properties(boompi_aec_loop_hil PROPERTIES
    OUTPUT_NAME boompi-aec-loop-hil
    SKIP_BUILD_RPATH TRUE BUILD_RPATH "" INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH FALSE INSTALL_RPATH_USE_LINK_PATH FALSE)
endif()
