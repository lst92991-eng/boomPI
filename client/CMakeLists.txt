include(GNUInstallDirs)

set(_boompi_ui_simulator_default ON)
if(BOOMPI_TARGET_RV1106)
  set(_boompi_ui_simulator_default OFF)
endif()
option(BOOMPI_BUILD_UI_SIMULATOR "Build the 320x240 LVGL desktop preview"
  ${_boompi_ui_simulator_default})
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
  if(UNIX AND NOT BOOMPI_TARGET_RV1106)
    # Host 只编译真实 SPI/I2C UI 边界，不打开设备；屏幕逻辑由 SDL 验证。
    add_library(boompi_device_ui_compile OBJECT
      src/ui/device_ui.cpp src/ui/camera_capture.cpp)
    target_compile_features(boompi_device_ui_compile PRIVATE cxx_std_17)
    target_compile_options(boompi_device_ui_compile PRIVATE -Wall -Wextra -Wpedantic
      $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
    target_link_libraries(boompi_device_ui_compile PRIVATE boompi_lvgl_ui Threads::Threads)
  endif()
endif()

if(NOT BOOMPI_TARGET_RV1106)
  include(tests/CMakeLists.txt)
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
  src/audio/voice_audio.cpp
  src/audio/audio_engine.cpp
  src/config/voice_client_config.cpp
  src/network/network_setup.cpp
  src/network/voice_link.cpp
  src/network/voice_codec.cpp
  src/platform/rv1106/audio_backend.cpp
  src/platform/rv1106/audio_thread.cpp
  src/platform/rv1106/alsa_audio.cpp
  src/platform/rv1106/rockchip_voice_dsp.cpp
  src/ui/device_ui.cpp
  src/ui/camera_capture.cpp)
target_compile_features(boompi_client PRIVATE cxx_std_17)
target_compile_definitions(boompi_client PRIVATE
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

install(TARGETS boompi_client RUNTIME DESTINATION /userdata/boompi/bin)
install(PROGRAMS scripts/boompi-clientctl scripts/boompi-provision
  DESTINATION /usr/sbin)
install(PROGRAMS scripts/boompi-provision.py DESTINATION /usr/lib/boompi)
install(PROGRAMS scripts/S99boompi-client DESTINATION /etc/init.d)

# 测试登记单独阅读；include 保留原有 target 输出路径和 CTest 工作目录。
include(tests/CMakeLists.txt)
