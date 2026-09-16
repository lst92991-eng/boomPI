# client/ 下 include：产品与测试共用真实源，测试只替换硬件 I/O。
option(BOOMPI_BUILD_AEC_LOOP_HIL "Build the opt-in RV1106 audio probe" OFF)
option(BOOMPI_REQUIRE_HOST_TRANSPORT_TEST "Require real WSS host tests" OFF)

function(boompi_test_options target)
  target_compile_features(${target} PRIVATE cxx_std_17)
  target_include_directories(${target} PRIVATE include tests src/network)
  if(MSVC)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /utf-8
      $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
      $<$<BOOL:${BOOMPI_STRICT_WARNINGS}>:-Werror>)
  endif()
endfunction()

if(BOOMPI_BUILD_TESTS AND NOT BOOMPI_TARGET_RV1106)
  find_package(Threads REQUIRED)
  add_executable(boompi_voice_client_config_test tests/voice_client_config_test.cpp
    src/config/voice_client_config.cpp)
  boompi_test_options(boompi_voice_client_config_test)
  add_test(NAME voice-client-config-contract COMMAND boompi_voice_client_config_test)

  # 链接实际应用源；测试只提供模块 I/O，不再包含生产 .cpp 文件。
  add_executable(boompi_voice_client_harness tests/voice_client_harness_test.cpp
    src/application/voice_client.cpp src/audio/speech.cpp src/config/voice_client_config.cpp)
  boompi_test_options(boompi_voice_client_harness)
  target_include_directories(boompi_voice_client_harness PRIVATE src/platform/rv1106)
  target_link_libraries(boompi_voice_client_harness PRIVATE Threads::Threads)
  add_test(NAME voice-client-behavior COMMAND boompi_voice_client_harness)

  find_package(OpenSSL QUIET)
  find_path(BOOMPI_HOST_BOOST_INCLUDE_DIR NAMES boost/asio.hpp)
  find_path(BOOMPI_HOST_CJSON_INCLUDE_DIR NAMES cjson/cJSON.h)
  find_library(BOOMPI_HOST_CJSON_LIBRARY NAMES cjson libcjson)
  if(OpenSSL_FOUND AND BOOMPI_HOST_BOOST_INCLUDE_DIR AND
      BOOMPI_HOST_CJSON_INCLUDE_DIR AND BOOMPI_HOST_CJSON_LIBRARY)
    add_executable(boompi_voice_transport_loopback_test
      tests/voice_transport_loopback_test.cpp src/network/voice_net.cpp
      src/network/voice_codec.cpp tests/support/network_setup.cpp
      src/config/voice_client_config.cpp)
    add_executable(boompi_protocol_json_test tests/protocol_json_contract.cpp
      src/network/voice_codec.cpp src/config/voice_client_config.cpp)
    foreach(target IN ITEMS boompi_voice_transport_loopback_test boompi_protocol_json_test)
      boompi_test_options(${target})
      target_compile_definitions(${target} PRIVATE
        BOOST_ERROR_CODE_HEADER_ONLY=1 BOOST_SYSTEM_NO_DEPRECATED=1)
      target_include_directories(${target} SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
        "${BOOMPI_HOST_BOOST_INCLUDE_DIR}" "${BOOMPI_HOST_CJSON_INCLUDE_DIR}")
      target_link_libraries(${target} PRIVATE
        OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})
    endforeach()
    # cJSON只用于测试读取JSON金样元数据，不进入客户端或WSS运行实现。
    target_link_libraries(boompi_protocol_json_test PRIVATE "${BOOMPI_HOST_CJSON_LIBRARY}")
    add_test(NAME voice-transport-loopback COMMAND boompi_voice_transport_loopback_test)
    add_test(NAME protocol-json-contract COMMAND boompi_protocol_json_test
      "${PROJECT_SOURCE_DIR}/protocol/fixtures/protocol-v4-golden.json")
  elseif(BOOMPI_REQUIRE_HOST_TRANSPORT_TEST)
    message(FATAL_ERROR "Real WSS tests require OpenSSL, Boost and cJSON")
  else()
    message(STATUS "Real WSS tests unavailable; set BOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON in release verification")
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # 真实Linux系统边界也严格编译；运行时替身不应掩盖生产源的构建错误。
    find_package(ALSA REQUIRED)
    add_library(boompi_audio_platform_compile OBJECT
      src/platform/rv1106/audio_capture.cpp
      src/platform/rv1106/audio_thread.cpp src/network/network.cpp)
    boompi_test_options(boompi_audio_platform_compile)
    target_include_directories(boompi_audio_platform_compile PRIVATE
      src/platform/rv1106 tests/support)
    target_link_libraries(boompi_audio_platform_compile PRIVATE ALSA::ALSA Threads::Threads)

    # 模块回归使用真实重采样库；缺依赖时配置失败，不把未运行的场景报告成通过。
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BOOMPI_HOST_AUDIO REQUIRED IMPORTED_TARGET libswresample libavutil)
    add_executable(boompi_audio_flow_test tests/audio_flow_test.cpp
      tests/audio_modules_test.cpp
      tests/support/audio_hardware.cpp tests/support/audio_vendor.cpp
      tests/support/audio_thread.cpp ${BOOMPI_AUDIO_SOURCES})
    boompi_test_options(boompi_audio_flow_test)
    target_include_directories(boompi_audio_flow_test BEFORE PRIVATE tests/support)
    target_include_directories(boompi_audio_flow_test PRIVATE src/audio src/platform/rv1106 ${ALSA_INCLUDE_DIRS})
    # BSP使用的旧FFmpeg接口在Host FFmpeg 6中标记deprecated；仅此源允许该兼容警告。
    set_source_files_properties(src/platform/rv1106/audio_convert.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-error=deprecated-declarations")
    target_link_libraries(boompi_audio_flow_test PRIVATE Threads::Threads
      PkgConfig::BOOMPI_HOST_AUDIO)
    foreach(scenario IN ITEMS modules-format modules-detection voice-preroll playback capture)
      add_test(NAME audio-flow-${scenario} COMMAND boompi_audio_flow_test ${scenario})
    endforeach()
  endif()
endif()

if(BOOMPI_TARGET_RV1106 AND BOOMPI_BUILD_AEC_LOOP_HIL)
  add_executable(boompi_aec_loop_hil tests/hil/aec_loop_hil.cpp
    ${BOOMPI_AUDIO_SOURCES} src/platform/rv1106/audio_capture.cpp
    src/platform/rv1106/audio_thread.cpp)
  boompi_test_options(boompi_aec_loop_hil)
  target_include_directories(boompi_aec_loop_hil PRIVATE src/audio src/platform/rv1106)
  target_include_directories(boompi_aec_loop_hil SYSTEM PRIVATE
    "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}" "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}"
    "${BOOMPI_SNOWBOY_INCLUDE_DIR}")
  target_link_libraries(boompi_aec_loop_hil PRIVATE boompi_vendor::snowboy
    boompi_vendor::rockchip_3a_aec boompi_webrtc_vad ALSA::ALSA
    "${BOOMPI_SWRESAMPLE_LIBRARY}" "${BOOMPI_AVUTIL_LIBRARY}"
    Threads::Threads ${CMAKE_DL_LIBS} atomic)
  set_target_properties(boompi_aec_loop_hil PROPERTIES OUTPUT_NAME boompi-aec-loop-hil
    SKIP_BUILD_RPATH TRUE BUILD_RPATH "" INSTALL_RPATH "")
endif()

if(BOOMPI_BUILD_TESTS AND NOT BOOMPI_TARGET_RV1106)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(boompi_network_interface_test tests/network_interface_test.cpp
      src/network/network.cpp src/config/voice_client_config.cpp)
    boompi_test_options(boompi_network_interface_test)
    add_test(NAME network-interface-contract COMMAND boompi_network_interface_test)
    add_executable(boompi_camera_lifecycle_test tests/camera_lifecycle_test.cpp
      src/ui/camera_capture.cpp)
    boompi_test_options(boompi_camera_lifecycle_test)
    target_link_libraries(boompi_camera_lifecycle_test PRIVATE Threads::Threads)
    target_link_options(boompi_camera_lifecycle_test PRIVATE -Wl,--wrap=execl)
    add_test(NAME camera-lifecycle COMMAND boompi_camera_lifecycle_test)
  endif()
  if(TARGET boompi_lvgl_ui)
    add_executable(boompi_ui_page_test tests/ui_page_test.cpp)
    boompi_test_options(boompi_ui_page_test)
    target_link_libraries(boompi_ui_page_test PRIVATE boompi_lvgl_ui)
    add_test(NAME ui-pages COMMAND boompi_ui_page_test)
  endif()
endif()
