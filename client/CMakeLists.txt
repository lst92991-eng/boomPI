include(GNUInstallDirs)

add_library(boompi_debug STATIC src/debug.cpp)
target_compile_features(boompi_debug PUBLIC cxx_std_17)
target_include_directories(boompi_debug PUBLIC include)
target_compile_options(boompi_debug PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)

set(BOOMPI_AUDIO_SOURCES
  src/audio/voice_input.cpp src/audio/playback.cpp src/audio/speech.cpp
  src/platform/rv1106/audio_convert.cpp src/platform/rv1106/rockchip_3a.cpp
  src/platform/rv1106/wake.cpp src/platform/rv1106/vad.cpp)
# 旧C++ ABI只属于实际调用Snowboy的这一份源文件。
set_source_files_properties(src/platform/rv1106/wake.cpp PROPERTIES
  COMPILE_DEFINITIONS "_GLIBCXX_USE_CXX11_ABI=0")

set(BOOMPI_OPENSSL_ROOT "" CACHE PATH "OpenSSL 3.5.7 package root")
set(BOOMPI_BOOST_INCLUDE_DIR "" CACHE PATH "Boost header root")
set(BOOMPI_WEBRTC_VAD_INCLUDE_DIR "" CACHE PATH "Directory containing webrtc_vad.h")
set(BOOMPI_WEBRTC_VAD_LIBRARY "" CACHE FILEPATH "RV1106 WebRTC VAD archive")
set(BOOMPI_LVGL_ROOT "" CACHE PATH "LVGL 8.2 source root")
if(BOOMPI_LVGL_ROOT STREQUAL "" AND DEFINED ENV{BOOMPI_LVGL_ROOT})
  file(TO_CMAKE_PATH "$ENV{BOOMPI_LVGL_ROOT}" BOOMPI_LVGL_ROOT)
endif()
find_package(Threads REQUIRED)
find_package(Freetype REQUIRED)
find_package(ALSA REQUIRED)
find_library(BOOMPI_SWRESAMPLE_LIBRARY NAMES swresample REQUIRED)
find_library(BOOMPI_AVUTIL_LIBRARY NAMES avutil REQUIRED)
include(cmake/BoompiLvgl.cmake)
boompi_configure_lvgl()

add_library(boompi_lvgl_ui STATIC src/ui/lvgl_screen.cpp
  assets/xiaozhi/emoji_1f606_64.c assets/xiaozhi/emoji_1f614_64.c
  assets/xiaozhi/emoji_1f62f_64.c assets/xiaozhi/emoji_1f642_64.c
  assets/xiaozhi/emoji_1f914_64.c)
target_compile_features(boompi_lvgl_ui PRIVATE cxx_std_17)
target_compile_options(boompi_lvgl_ui PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${BOOMPI_STRICT_WARNINGS}>>:-Werror>)
target_include_directories(boompi_lvgl_ui PUBLIC include PRIVATE assets/xiaozhi)
target_link_libraries(boompi_lvgl_ui PUBLIC lvgl::lvgl Freetype::Freetype)

if(NOT IS_ABSOLUTE "${BOOMPI_BOOST_INCLUDE_DIR}" OR
   NOT EXISTS "${BOOMPI_BOOST_INCLUDE_DIR}/boost/asio.hpp")
  message(FATAL_ERROR "BOOMPI_BOOST_INCLUDE_DIR must be an absolute Boost header path")
endif()
if(NOT IS_ABSOLUTE "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}" OR
   NOT EXISTS "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}/webrtc_vad.h" OR
   NOT IS_ABSOLUTE "${BOOMPI_WEBRTC_VAD_LIBRARY}" OR
   NOT EXISTS "${BOOMPI_WEBRTC_VAD_LIBRARY}")
  message(FATAL_ERROR "WebRTC VAD headers/archive are missing")
endif()
if(NOT IS_ABSOLUTE "${BOOMPI_OPENSSL_ROOT}" OR NOT IS_DIRECTORY "${BOOMPI_OPENSSL_ROOT}")
  message(FATAL_ERROR "BOOMPI_OPENSSL_ROOT must be an absolute package directory")
endif()
file(REAL_PATH "${BOOMPI_OPENSSL_ROOT}" BOOMPI_OPENSSL_ROOT)
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(OpenSSL_DIR "${BOOMPI_OPENSSL_ROOT}" CACHE PATH "Pinned OpenSSL package" FORCE)
find_package(OpenSSL 3.5.7 EXACT CONFIG REQUIRED PATHS "${BOOMPI_OPENSSL_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
add_library(boompi_webrtc_vad STATIC IMPORTED)
set_target_properties(boompi_webrtc_vad PROPERTIES IMPORTED_LOCATION "${BOOMPI_WEBRTC_VAD_LIBRARY}")

add_executable(boompi_client apps/boompi_client/main.cpp
  src/application/voice_client.cpp ${BOOMPI_AUDIO_SOURCES}
  src/config/voice_client_config.cpp src/network/network.cpp
  src/network/voice_net.cpp src/network/voice_codec.cpp
  src/platform/rv1106/audio_thread.cpp src/platform/rv1106/audio_capture.cpp
  src/platform/rv1106/display_touch.cpp src/ui/device_ui.cpp)
target_compile_features(boompi_client PRIVATE cxx_std_17)
target_compile_definitions(boompi_client PRIVATE
  BOOST_ERROR_CODE_HEADER_ONLY=1 BOOST_SYSTEM_NO_DEPRECATED=1)
target_compile_options(boompi_client PRIVATE -Wall -Wextra -Wpedantic
  $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
target_include_directories(boompi_client PRIVATE include src/audio src/platform/rv1106)
target_include_directories(boompi_client SYSTEM PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party" "${BOOMPI_BOOST_INCLUDE_DIR}"
  "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}" "${BOOMPI_SNOWBOY_INCLUDE_DIR}"
  "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}")
target_link_libraries(boompi_client PRIVATE boompi_debug boompi_lvgl_ui
  boompi_vendor::snowboy boompi_vendor::rockchip_3a_aec boompi_webrtc_vad ALSA::ALSA
  "${BOOMPI_SWRESAMPLE_LIBRARY}" "${BOOMPI_AVUTIL_LIBRARY}" Freetype::Freetype
  OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS} atomic)
target_link_options(boompi_client PRIVATE $<$<CONFIG:Release>:-Wl,--strip-all>)
set_target_properties(boompi_client PROPERTIES OUTPUT_NAME boompi-client
  SKIP_BUILD_RPATH TRUE BUILD_RPATH "" INSTALL_RPATH ""
  BUILD_WITH_INSTALL_RPATH FALSE INSTALL_RPATH_USE_LINK_PATH FALSE)

install(TARGETS boompi_client RUNTIME DESTINATION /userdata/boompi/bin)
# 让系统加载器直接找到这两个SDK库，不依赖启动脚本的LD_LIBRARY_PATH。
install(FILES "${BOOMPI_ROCKCHIP_3A_AEC_LIBRARY}" "${BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY}"
  DESTINATION /usr/lib)
