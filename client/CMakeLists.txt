include(GNUInstallDirs)
find_package(Threads REQUIRED)
option(BOOMPI_BUILD_RV1106_AUDIO_SMOKE
  "Build the explicit RV1106 ALSA full-duplex smoke executable" OFF)

set(_boompi_alsa_playback_default OFF)
if(BOOMPI_TARGET_RV1106)
  set(_boompi_alsa_playback_default ON)
endif()
option(BOOMPI_ENABLE_ALSA_PLAYBACK
  "Build the explicit Linux ALSA playback device adapter"
  ${_boompi_alsa_playback_default})
unset(_boompi_alsa_playback_default)

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
  src/audio/playback_control.cpp
  src/audio/playback_frame.cpp
  src/audio/playback_gain_limiter.cpp
  src/audio/playback_generation.cpp
  src/audio/playback_renderer_24_to_48.cpp
  src/audio/playback_resampler_24_to_48.cpp
  src/audio/playback_worker.cpp
  src/audio/vad_utterance_controller.cpp
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

boompi_add_library(boompi_platform_audio
  src/platform/audio_pcm_playback.cpp)
target_link_libraries(boompi_platform_audio
  PUBLIC
    boompi_audio_core
    boompi_event)

boompi_add_library(boompi_platform_rv1106
  src/platform/rv1106/audio_pcm_transfer.cpp
  src/platform/rv1106/build_environment.cpp
  src/platform/rv1106/rockchip_3a_audio_dsp_adapter.cpp)
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
if(BOOMPI_ENABLE_SNOWBOY)
  add_library(boompi_snowboy_legacy_bridge STATIC
    src/platform/rv1106/snowboy_legacy_bridge.cpp)
  boompi_configure_target(boompi_snowboy_legacy_bridge)
  target_compile_definitions(boompi_snowboy_legacy_bridge
    PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
  target_include_directories(boompi_snowboy_legacy_bridge
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/rv1106"
      "${BOOMPI_SNOWBOY_INCLUDE_DIR}")
  target_link_libraries(boompi_snowboy_legacy_bridge
    PRIVATE boompi_vendor::snowboy)

  target_sources(boompi_platform_rv1106 PRIVATE
    src/platform/rv1106/snowboy_wake_word_engine.cpp)
  target_include_directories(boompi_platform_rv1106
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/rv1106")
  target_link_libraries(boompi_platform_rv1106
    PRIVATE
      boompi_audio_core
      boompi_snowboy_legacy_bridge)

  add_executable(boompi_snowboy_offline_probe EXCLUDE_FROM_ALL
    apps/snowboy_offline_probe/main.cpp)
  boompi_configure_target(boompi_snowboy_offline_probe)
  target_link_libraries(boompi_snowboy_offline_probe
    PRIVATE
      boompi_audio_core
      boompi_platform_rv1106)
  set_target_properties(boompi_snowboy_offline_probe PROPERTIES
    OUTPUT_NAME "boompi-snowboy-offline-probe"
    SKIP_BUILD_RPATH TRUE)
endif()

if(BOOMPI_ENABLE_ALSA_PLAYBACK)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
      "BOOMPI_ENABLE_ALSA_PLAYBACK requires a Linux target")
  endif()
  find_package(ALSA REQUIRED)
  boompi_add_library(boompi_platform_alsa
    src/platform/rv1106/alsa_pcm_playback.cpp)
  target_link_libraries(boompi_platform_alsa
    PUBLIC boompi_platform_audio
    PRIVATE ALSA::ALSA)

  # This target is intentionally part of ALL even when tests are disabled. It
  # makes the target linker resolve the adapter's ALSA and clock symbols before
  # the runtime composition root starts constructing the adapter.
  add_executable(boompi_alsa_playback_link_check
    tests/link/alsa_playback_link_check.cpp)
  boompi_configure_target(boompi_alsa_playback_link_check)
  target_link_libraries(boompi_alsa_playback_link_check
    PRIVATE boompi_platform_alsa)
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
if(BOOMPI_ENABLE_ALSA_PLAYBACK)
  target_link_libraries(boompi_client PRIVATE boompi_platform_alsa)
endif()
if(BOOMPI_TARGET_RV1106)
  target_compile_definitions(boompi_client PRIVATE BOOMPI_TARGET_RV1106=1)
endif()
set_target_properties(boompi_client PROPERTIES OUTPUT_NAME "boompi-client")

add_executable(boompi_supervisor apps/boompi_supervisor/main.cpp)
boompi_configure_target(boompi_supervisor)
target_link_libraries(boompi_supervisor PRIVATE boompi_update)
set_target_properties(boompi_supervisor PROPERTIES OUTPUT_NAME "boompi-supervisor")

if(BOOMPI_ENABLE_ROCKCHIP_3A)
  target_sources(boompi_platform_rv1106 PRIVATE
    src/platform/rv1106/rockchip_3a_preprocess_session.cpp)
  target_include_directories(boompi_platform_rv1106
    PRIVATE "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}")
  target_link_libraries(boompi_platform_rv1106
    PRIVATE boompi_vendor::rockchip_3a_aec)

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

  add_executable(boompi_rockchip_3a_adapter_probe EXCLUDE_FROM_ALL
    apps/rockchip_3a_adapter_probe/main.cpp)
  boompi_configure_target(boompi_rockchip_3a_adapter_probe)
  target_link_libraries(boompi_rockchip_3a_adapter_probe
    PRIVATE
      boompi_audio_core
      boompi_platform_rv1106)
  set_target_properties(boompi_rockchip_3a_adapter_probe PROPERTIES
    OUTPUT_NAME "boompi-rockchip-3a-adapter-probe"
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

  add_executable(boompi_audio_pcm_playback_adapter_tests
    tests/unit/audio_pcm_playback_adapter_tests.cpp)
  boompi_configure_target(boompi_audio_pcm_playback_adapter_tests)
  target_link_libraries(boompi_audio_pcm_playback_adapter_tests
    PRIVATE
      boompi_platform_audio
      boompi_test_support)
  add_test(
    NAME boompi_audio_pcm_playback_adapter_tests
    COMMAND boompi_audio_pcm_playback_adapter_tests)

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

  add_executable(boompi_audio_playback_control_tests
    tests/unit/audio_playback_control_tests.cpp)
  boompi_configure_target(boompi_audio_playback_control_tests)
  target_link_libraries(boompi_audio_playback_control_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support
      Threads::Threads)
  add_test(
    NAME boompi_audio_playback_control_tests
    COMMAND boompi_audio_playback_control_tests)

  add_executable(boompi_audio_playback_worker_tests
    tests/unit/audio_playback_worker_tests.cpp)
  boompi_configure_target(boompi_audio_playback_worker_tests)
  target_link_libraries(boompi_audio_playback_worker_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_playback_worker_tests
    COMMAND boompi_audio_playback_worker_tests)

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

  if(BOOMPI_ENABLE_ALSA_PLAYBACK)
    add_executable(boompi_alsa_null_playback_smoke
      tests/smoke/alsa_null_playback_smoke.cpp)
    boompi_configure_target(boompi_alsa_null_playback_smoke)
    target_link_libraries(boompi_alsa_null_playback_smoke
      PRIVATE boompi_platform_alsa)
    add_test(
      NAME boompi_alsa_null_playback_accepted_only
      COMMAND boompi_alsa_null_playback_smoke)
    set_tests_properties(
      boompi_alsa_null_playback_accepted_only
      PROPERTIES
        LABELS "alsa-null-accepted-only"
        TIMEOUT 20)
  endif()

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

  add_executable(boompi_audio_vad_utterance_controller_tests
    tests/unit/audio_vad_utterance_controller_tests.cpp)
  boompi_configure_target(boompi_audio_vad_utterance_controller_tests)
  target_link_libraries(boompi_audio_vad_utterance_controller_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_audio_vad_utterance_controller_tests
    COMMAND boompi_audio_vad_utterance_controller_tests)

  add_executable(boompi_rockchip_3a_audio_dsp_adapter_tests
    tests/unit/rockchip_3a_audio_dsp_adapter_tests.cpp)
  boompi_configure_target(boompi_rockchip_3a_audio_dsp_adapter_tests)
  target_link_libraries(boompi_rockchip_3a_audio_dsp_adapter_tests
    PRIVATE
      boompi_platform_rv1106
      boompi_test_support)
  add_test(
    NAME boompi_rockchip_3a_audio_dsp_adapter_tests
    COMMAND boompi_rockchip_3a_audio_dsp_adapter_tests)

  add_executable(boompi_snowboy_wake_word_engine_tests
    src/platform/rv1106/snowboy_wake_word_engine.cpp
    tests/fixtures/snowboy_legacy_bridge_fake.cpp
    tests/unit/snowboy_wake_word_engine_tests.cpp)
  boompi_configure_target(boompi_snowboy_wake_word_engine_tests)
  target_include_directories(boompi_snowboy_wake_word_engine_tests
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/rv1106"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(boompi_snowboy_wake_word_engine_tests
    PRIVATE
      boompi_audio_core
      boompi_test_support)
  add_test(
    NAME boompi_snowboy_wake_word_engine_tests
    COMMAND boompi_snowboy_wake_word_engine_tests)

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
    boompi_audio_pcm_playback_adapter_tests
    boompi_audio_playback_committer_tests
    boompi_audio_playback_control_tests
    boompi_audio_playback_worker_tests
    boompi_audio_backend_contract_tests
    boompi_audio_dsp_frame_bridge_tests
    boompi_audio_vad_utterance_controller_tests
    boompi_rockchip_3a_audio_dsp_adapter_tests
    boompi_audio_pcm_transfer_tests
    boompi_snowboy_wake_word_engine_tests
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
