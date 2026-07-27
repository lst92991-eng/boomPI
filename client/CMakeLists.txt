include(GNUInstallDirs)
find_package(Threads REQUIRED)
option(BOOMPI_BUILD_RV1106_AUDIO_SMOKE
  "Build the explicit RV1106 ALSA full-duplex smoke executable" OFF)

function(boompi_configure_target target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_17)
  target_include_directories(${target_name}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
    if(BOOMPI_STRICT_WARNINGS)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    if(BOOMPI_STRICT_WARNINGS)
      target_compile_options(${target_name} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(boompi_add_library target_name)
  add_library(${target_name} STATIC ${ARGN})
  boompi_configure_target(${target_name})
endfunction()

boompi_add_library(boompi_event
  src/event/event.cpp
  src/event/status.cpp)
target_link_libraries(boompi_event PRIVATE Threads::Threads)

boompi_add_library(boompi_config
  src/config/client_config.cpp)
target_link_libraries(boompi_config PUBLIC boompi_event)

boompi_add_library(boompi_audio_core
  src/audio/audio_dsp_frame_bridge.cpp
  src/audio/audio_dsp_engine.cpp
  src/audio/audio_format.cpp
  src/audio/audio_frame.cpp
  src/audio/capture_dsp_frontend.cpp
  src/audio/capture_channel_mapper.cpp
  src/audio/fir_decimator_48_to_16.cpp
  src/audio/frame_continuity_gate.cpp
  src/audio/pcm_playback_sink.cpp
  src/audio/playback_buffer.cpp
  src/audio/playback_committer.cpp
  src/audio/playback_frame.cpp
  src/audio/playback_gain_limiter.cpp
  src/audio/playback_generation.cpp
  src/audio/playback_renderer_24_to_48.cpp
  src/audio/playback_resampler_24_to_48.cpp
  src/audio/wake_word_engine.cpp)
target_link_libraries(boompi_audio_core PUBLIC boompi_event)

boompi_add_library(boompi_protocol
  src/protocol/audio_packet.cpp
  src/protocol/envelope.cpp)
target_link_libraries(boompi_protocol PUBLIC boompi_event)

boompi_add_library(boompi_network
  src/network/reconnect_backoff.cpp)

boompi_add_library(boompi_ui
  src/ui/ui_model.cpp)
target_link_libraries(boompi_ui PUBLIC boompi_event)

boompi_add_library(boompi_update
  src/update/app_slot.cpp)
target_link_libraries(boompi_update PUBLIC boompi_event)

boompi_add_library(boompi_platform_posix
  src/platform/posix/monotonic_clock.cpp)

boompi_add_library(boompi_platform_rv1106
  src/platform/rv1106/audio_pcm_transfer.cpp
  src/platform/rv1106/build_environment.cpp)
target_link_libraries(boompi_platform_rv1106
  PUBLIC
    boompi_audio_core
    boompi_event)
if(BOOMPI_TARGET_RV1106)
  find_package(ALSA REQUIRED)
  target_sources(boompi_platform_rv1106 PRIVATE
    src/platform/rv1106/alsa_pcm_stream.cpp)
  target_link_libraries(boompi_platform_rv1106 PRIVATE ALSA::ALSA)
  target_compile_definitions(boompi_platform_rv1106 PRIVATE BOOMPI_TARGET_RV1106=1)
endif()

boompi_add_library(boompi_application
  src/application/runtime.cpp)
target_link_libraries(boompi_application
  PUBLIC
    boompi_config
    boompi_event
    boompi_platform_posix
  PRIVATE
    boompi_audio_core
    boompi_network
    boompi_protocol
    boompi_ui
    boompi_update)

add_executable(boompi_client apps/boompi_client/main.cpp)
boompi_configure_target(boompi_client)
target_link_libraries(boompi_client PRIVATE boompi_application boompi_platform_rv1106)
if(BOOMPI_TARGET_RV1106)
  target_compile_definitions(boompi_client PRIVATE BOOMPI_TARGET_RV1106=1)
endif()
set_target_properties(boompi_client PROPERTIES OUTPUT_NAME "boompi-client")

add_executable(boompi_supervisor apps/boompi_supervisor/main.cpp)
boompi_configure_target(boompi_supervisor)
target_link_libraries(boompi_supervisor PRIVATE boompi_update)
set_target_properties(boompi_supervisor PROPERTIES OUTPUT_NAME "boompi-supervisor")

if(BOOMPI_ENABLE_ROCKCHIP_3A)
  add_executable(boompi_rockchip_3a_probe EXCLUDE_FROM_ALL
    apps/rockchip_3a_probe/main.cpp)
  boompi_configure_target(boompi_rockchip_3a_probe)
  target_include_directories(boompi_rockchip_3a_probe
    PRIVATE "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}")
  target_link_libraries(boompi_rockchip_3a_probe
    PRIVATE boompi_vendor::rockchip_3a_aec)
  set_target_properties(boompi_rockchip_3a_probe PROPERTIES
    OUTPUT_NAME "boompi-rockchip-3a-probe"
    SKIP_BUILD_RPATH TRUE)
endif()

install(TARGETS boompi_client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS boompi_supervisor RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

if(BOOMPI_BUILD_TESTS)
  add_library(boompi_test_support STATIC
    tests/support/scripted_pcm_playback_sink.cpp
    tests/support/fake_audio_engines.cpp
    tests/support/test_context.cpp)
  target_include_directories(boompi_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/include)
  boompi_configure_target(boompi_test_support)

  add_executable(boompi_host_tests
    tests/unit/client_core_tests.cpp)
  boompi_configure_target(boompi_host_tests)
  target_link_libraries(boompi_host_tests
    PRIVATE
      boompi_application
      boompi_audio_core
      boompi_config
      boompi_event
      boompi_network
      boompi_protocol
      boompi_test_support
      boompi_ui
      boompi_update)

  add_test(
    NAME boompi_host_tests
    COMMAND boompi_host_tests
      ${PROJECT_SOURCE_DIR}/protocol/fixtures/protocol-v1-golden.json)

  add_executable(boompi_audio_queue_tests
    tests/unit/audio_queue_tests.cpp)
  boompi_configure_target(boompi_audio_queue_tests)
  target_link_libraries(boompi_audio_queue_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support
      Threads::Threads)
  add_test(
    NAME boompi_audio_queue_tests
    COMMAND boompi_audio_queue_tests)

  add_executable(boompi_audio_continuity_tests
    tests/unit/audio_continuity_tests.cpp)
  boompi_configure_target(boompi_audio_continuity_tests)
  target_link_libraries(boompi_audio_continuity_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_continuity_tests
    COMMAND boompi_audio_continuity_tests)

  add_executable(boompi_audio_channel_mapper_tests
    tests/unit/audio_channel_mapper_tests.cpp)
  boompi_configure_target(boompi_audio_channel_mapper_tests)
  target_link_libraries(boompi_audio_channel_mapper_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_channel_mapper_tests
    COMMAND boompi_audio_channel_mapper_tests)

  add_executable(boompi_audio_fir_decimator_tests
    tests/unit/audio_fir_decimator_tests.cpp)
  boompi_configure_target(boompi_audio_fir_decimator_tests)
  target_link_libraries(boompi_audio_fir_decimator_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_fir_decimator_tests
    COMMAND boompi_audio_fir_decimator_tests)

  add_executable(boompi_audio_capture_dsp_frontend_tests
    tests/unit/audio_capture_dsp_frontend_tests.cpp)
  boompi_configure_target(boompi_audio_capture_dsp_frontend_tests)
  target_link_libraries(boompi_audio_capture_dsp_frontend_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_capture_dsp_frontend_tests
    COMMAND boompi_audio_capture_dsp_frontend_tests)

  add_executable(boompi_audio_playback_buffer_tests
    tests/unit/audio_playback_buffer_tests.cpp)
  boompi_configure_target(boompi_audio_playback_buffer_tests)
  target_link_libraries(boompi_audio_playback_buffer_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support
      Threads::Threads)
  add_test(
    NAME boompi_audio_playback_buffer_tests
    COMMAND boompi_audio_playback_buffer_tests)

  add_executable(boompi_audio_playback_resampler_tests
    tests/unit/audio_playback_resampler_tests.cpp)
  boompi_configure_target(boompi_audio_playback_resampler_tests)
  target_link_libraries(boompi_audio_playback_resampler_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_playback_resampler_tests
    COMMAND boompi_audio_playback_resampler_tests)

  add_executable(boompi_audio_playback_gain_limiter_tests
    tests/unit/audio_playback_gain_limiter_tests.cpp)
  boompi_configure_target(boompi_audio_playback_gain_limiter_tests)
  target_link_libraries(boompi_audio_playback_gain_limiter_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_playback_gain_limiter_tests
    COMMAND boompi_audio_playback_gain_limiter_tests)

  add_executable(boompi_audio_playback_renderer_tests
    tests/unit/audio_playback_renderer_tests.cpp)
  boompi_configure_target(boompi_audio_playback_renderer_tests)
  target_link_libraries(boompi_audio_playback_renderer_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_playback_renderer_tests
    COMMAND boompi_audio_playback_renderer_tests)

  add_executable(boompi_audio_pcm_playback_sink_tests
    tests/unit/audio_pcm_playback_sink_tests.cpp)
  boompi_configure_target(boompi_audio_pcm_playback_sink_tests)
  target_link_libraries(boompi_audio_pcm_playback_sink_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_pcm_playback_sink_tests
    COMMAND boompi_audio_pcm_playback_sink_tests)

  add_executable(boompi_audio_playback_committer_tests
    tests/unit/audio_playback_committer_tests.cpp)
  boompi_configure_target(boompi_audio_playback_committer_tests)
  target_link_libraries(boompi_audio_playback_committer_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_playback_committer_tests
    COMMAND boompi_audio_playback_committer_tests)

  add_executable(boompi_audio_backend_contract_tests
    tests/unit/audio_backend_contract_tests.cpp)
  boompi_configure_target(boompi_audio_backend_contract_tests)
  target_link_libraries(boompi_audio_backend_contract_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_backend_contract_tests
    COMMAND boompi_audio_backend_contract_tests)

  add_executable(boompi_audio_dsp_frame_bridge_tests
    tests/unit/audio_dsp_frame_bridge_tests.cpp)
  boompi_configure_target(boompi_audio_dsp_frame_bridge_tests)
  target_link_libraries(boompi_audio_dsp_frame_bridge_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_dsp_frame_bridge_tests
    COMMAND boompi_audio_dsp_frame_bridge_tests)

  add_executable(boompi_audio_pcm_transfer_tests
    tests/unit/audio_pcm_transfer_tests.cpp)
  boompi_configure_target(boompi_audio_pcm_transfer_tests)
  target_link_libraries(boompi_audio_pcm_transfer_tests
    PRIVATE
      boompi_platform_rv1106
      boompi_test_support)
  add_test(
    NAME boompi_audio_pcm_transfer_tests
    COMMAND boompi_audio_pcm_transfer_tests)

  set_tests_properties(
    boompi_host_tests
    boompi_audio_queue_tests
    boompi_audio_continuity_tests
    boompi_audio_channel_mapper_tests
    boompi_audio_fir_decimator_tests
    boompi_audio_capture_dsp_frontend_tests
    boompi_audio_playback_buffer_tests
    boompi_audio_playback_resampler_tests
    boompi_audio_playback_gain_limiter_tests
    boompi_audio_playback_renderer_tests
    boompi_audio_pcm_playback_sink_tests
    boompi_audio_playback_committer_tests
    boompi_audio_backend_contract_tests
    boompi_audio_dsp_frame_bridge_tests
    boompi_audio_pcm_transfer_tests
    PROPERTIES TIMEOUT 20)
endif()

if(BOOMPI_TARGET_RV1106 AND BOOMPI_BUILD_RV1106_AUDIO_SMOKE)
  add_executable(boompi_alsa_smoke
    tests/smoke/rv1106_alsa_smoke.cpp)
  boompi_configure_target(boompi_alsa_smoke)
  target_link_libraries(boompi_alsa_smoke
    PRIVATE
      boompi_audio_core
      boompi_platform_rv1106
      Threads::Threads)
  set_target_properties(boompi_alsa_smoke PROPERTIES
    OUTPUT_NAME "boompi-alsa-smoke")
endif()
