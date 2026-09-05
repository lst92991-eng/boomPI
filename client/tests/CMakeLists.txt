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

  # 此文件包含实际 application 源，只将时钟和两个模块的 I/O 替换为脚本。
  add_executable(boompi_voice_client_harness tests/voice_client_harness_test.cpp)
  boompi_test_options(boompi_voice_client_harness)
  target_link_libraries(boompi_voice_client_harness PRIVATE Threads::Threads)
  add_test(NAME voice-client-behavior COMMAND boompi_voice_client_harness)

  find_package(OpenSSL QUIET)
  find_path(BOOMPI_HOST_BOOST_INCLUDE_DIR NAMES boost/asio.hpp)
  find_path(BOOMPI_HOST_CJSON_INCLUDE_DIR NAMES cjson/cJSON.h)
  find_library(BOOMPI_HOST_CJSON_LIBRARY NAMES cjson libcjson)
  if(OpenSSL_FOUND AND BOOMPI_HOST_BOOST_INCLUDE_DIR AND
      BOOMPI_HOST_CJSON_INCLUDE_DIR AND BOOMPI_HOST_CJSON_LIBRARY)
    add_executable(boompi_voice_transport_loopback_test
      tests/voice_transport_loopback_test.cpp src/network/voice_link.cpp
      src/network/voice_codec.cpp src/network/network_setup.cpp
      src/config/voice_client_config.cpp)
    add_executable(boompi_protocol_json_test tests/protocol_json_contract.cpp
      src/network/voice_codec.cpp src/config/voice_client_config.cpp)
    foreach(target IN ITEMS boompi_voice_transport_loopback_test boompi_protocol_json_test)
      boompi_test_options(${target})
      target_compile_definitions(${target} PRIVATE
        BOOST_ERROR_CODE_HEADER_ONLY=1 BOOST_SYSTEM_NO_DEPRECATED=1)
      target_include_directories(${target} SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
        "${BOOMPI_HOST_BOOST_INCLUDE_DIR}" "${BOOMPI_HOST_CJSON_INCLUDE_DIR}")
      target_link_libraries(${target} PRIVATE "${BOOMPI_HOST_CJSON_LIBRARY}"
        OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS})
    endforeach()
    add_test(NAME voice-transport-loopback COMMAND boompi_voice_transport_loopback_test)
    add_test(NAME protocol-json-contract COMMAND boompi_protocol_json_test
      "${PROJECT_SOURCE_DIR}/protocol/fixtures/protocol-v2-golden.json")
  elseif(BOOMPI_REQUIRE_HOST_TRANSPORT_TEST)
    message(FATAL_ERROR "Real WSS tests require OpenSSL, Boost and cJSON")
  else()
    message(STATUS "Real WSS tests unavailable; set BOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON in release verification")
  endif()

  if(UNIX)
    add_executable(boompi_audio_engine_harness tests/audio_engine_harness_test.cpp
      tests/support/audio_backend.cpp src/audio/audio_engine.cpp src/audio/voice_audio.cpp)
    boompi_test_options(boompi_audio_engine_harness)
    target_include_directories(boompi_audio_engine_harness BEFORE PRIVATE tests/support)
    target_link_libraries(boompi_audio_engine_harness PRIVATE Threads::Threads)
    foreach(scenario IN ITEMS sub-grace-jitter confirmed-gap short-tail
        bounded-close bounded-capture bounded-command reset-preserves-pcm
        queue-results open-clears-error playback-clears-error
        playback-owner-order playback-prepare-failure drop-blocked-render drop-blocked-drain
        close-blocked-render voice-preroll voice-follow-up-boundary voice-barge-lifecycle)
      add_test(NAME audio-engine-${scenario} COMMAND boompi_audio_engine_harness ${scenario})
    endforeach()
  endif()
endif()

if(BOOMPI_TARGET_RV1106 AND BOOMPI_BUILD_AEC_LOOP_HIL)
  add_executable(boompi_aec_loop_hil tests/hil/aec_loop_hil.cpp
    src/audio/audio_engine.cpp src/audio/voice_audio.cpp
    src/platform/rv1106/audio_backend.cpp src/platform/rv1106/alsa_audio.cpp
    src/platform/rv1106/rockchip_voice_dsp.cpp)
  boompi_test_options(boompi_aec_loop_hil)
  target_include_directories(boompi_aec_loop_hil PRIVATE src/platform/rv1106)
  target_include_directories(boompi_aec_loop_hil SYSTEM PRIVATE
    "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}" "${BOOMPI_WEBRTC_VAD_INCLUDE_DIR}")
  target_link_libraries(boompi_aec_loop_hil PRIVATE boompi_snowboy_bridge
    boompi_vendor::rockchip_3a_aec boompi_webrtc_vad ALSA::ALSA
    "${BOOMPI_SWRESAMPLE_LIBRARY}" "${BOOMPI_AVUTIL_LIBRARY}"
    Threads::Threads ${CMAKE_DL_LIBS} atomic)
  set_target_properties(boompi_aec_loop_hil PROPERTIES OUTPUT_NAME boompi-aec-loop-hil
    SKIP_BUILD_RPATH TRUE BUILD_RPATH "" INSTALL_RPATH "")
endif()
