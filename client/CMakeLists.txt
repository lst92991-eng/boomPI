include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(_boompi_alsa_playback_default OFF)
if(BOOMPI_TARGET_RV1106)
  set(_boompi_alsa_playback_default ON)
endif()
option(BOOMPI_ENABLE_ALSA_PLAYBACK
  "Build the explicit Linux ALSA playback device adapter"
  ${_boompi_alsa_playback_default})
unset(_boompi_alsa_playback_default)
option(BOOMPI_ENABLE_WSS_CLIENT
  "Build the pinned OpenSSL development WSS client" OFF)
set(BOOMPI_OPENSSL_ROOT "" CACHE PATH
  "Absolute root of the explicit OpenSSL 3.5.7 package used by WSS")
set(BOOMPI_GO_EXECUTABLE "" CACHE FILEPATH
  "Optional Go executable used by the cross-language WSS smoke test")

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
  src/audio/wake_word_engine.cpp)
target_link_libraries(boompi_audio_core PUBLIC boompi_event)

boompi_add_library(boompi_protocol
  src/protocol/audio_packet.cpp
  src/protocol/envelope.cpp
  src/protocol/server_control.cpp)
target_link_libraries(boompi_protocol PUBLIC boompi_event)

boompi_add_library(boompi_network
  src/network/reconnect_backoff.cpp)
if(BOOMPI_ENABLE_WSS_CLIENT)
  if(BOOMPI_OPENSSL_ROOT STREQUAL "" AND
     DEFINED ENV{BOOMPI_OPENSSL_ROOT} AND
     NOT "$ENV{BOOMPI_OPENSSL_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{BOOMPI_OPENSSL_ROOT}" _boompi_openssl_root)
  else()
    set(_boompi_openssl_root "${BOOMPI_OPENSSL_ROOT}")
  endif()
  string(FIND "${_boompi_openssl_root}" ";" _boompi_openssl_semicolon)
  if(_boompi_openssl_root STREQUAL "" OR
     NOT _boompi_openssl_semicolon EQUAL -1 OR
     NOT IS_ABSOLUTE "${_boompi_openssl_root}" OR
     NOT IS_DIRECTORY "${_boompi_openssl_root}")
    message(FATAL_ERROR
      "BOOMPI_OPENSSL_ROOT must name one explicit absolute OpenSSL 3.5.7 package root")
  endif()
  file(REAL_PATH "${_boompi_openssl_root}" _boompi_openssl_root)
  set(BOOMPI_OPENSSL_ROOT "${_boompi_openssl_root}" CACHE PATH
    "Absolute root of the explicit OpenSSL 3.5.7 package used by WSS" FORCE)

  foreach(_boompi_openssl_file IN ITEMS
      OpenSSLConfig.cmake
      OpenSSLConfigVersion.cmake
      include/openssl/opensslv.h
      include/openssl/configuration.h
      libssl.a
      libcrypto.a)
    if(NOT EXISTS "${_boompi_openssl_root}/${_boompi_openssl_file}")
      message(FATAL_ERROR
        "Pinned OpenSSL package is missing ${_boompi_openssl_file}")
    endif()
  endforeach()

  if(BOOMPI_TARGET_RV1106)
    function(boompi_verify_openssl_sha256 relative_path expected_sha256)
      file(SHA256 "${_boompi_openssl_root}/${relative_path}" _actual_sha256)
      if(NOT _actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
          "Pinned RV1106 OpenSSL file failed SHA-256 verification: ${relative_path}")
      endif()
    endfunction()
    boompi_verify_openssl_sha256(libssl.a
      9d40ee1a49721f880f2b47da71908b231411f713c0db1bd9b64a0713ded864ec)
    boompi_verify_openssl_sha256(libcrypto.a
      26d1122294bac0ef8963708e5956bedf62df169360b9e9fe9cb7c682bb326aea)
    boompi_verify_openssl_sha256(include/openssl/opensslv.h
      256985d6271826835673eb59dec8369dbbead0b5055864b8b7e69effac426e52)
    boompi_verify_openssl_sha256(include/openssl/configuration.h
      25cc4d05c4a6ccd42e1441b40c5804392e588e0d0ff31087f47a926de0ccec96)
    boompi_verify_openssl_sha256(OpenSSLConfig.cmake
      cb1444c7d611b7fe9e65352a89c6c4da2a457846df370191fa526e68368b2469)
    boompi_verify_openssl_sha256(OpenSSLConfigVersion.cmake
      352f614feee96a22155c0427d8fe64f100ad71dac5fcf1dfb468f7a1060f48d5)

    file(GLOB_RECURSE _boompi_openssl_headers
      LIST_DIRECTORIES FALSE
      RELATIVE "${_boompi_openssl_root}/include/openssl"
      "${_boompi_openssl_root}/include/openssl/*")
    list(SORT _boompi_openssl_headers)
    list(LENGTH _boompi_openssl_headers _boompi_openssl_header_count)
    set(_boompi_openssl_header_manifest "")
    foreach(_boompi_openssl_header IN LISTS _boompi_openssl_headers)
      file(SHA256
        "${_boompi_openssl_root}/include/openssl/${_boompi_openssl_header}"
        _boompi_openssl_header_sha256)
      string(APPEND _boompi_openssl_header_manifest
        "${_boompi_openssl_header}:${_boompi_openssl_header_sha256}\n")
    endforeach()
    string(SHA256 _boompi_openssl_header_manifest_sha256
      "${_boompi_openssl_header_manifest}")
    if(NOT _boompi_openssl_header_count EQUAL 172 OR
       NOT _boompi_openssl_header_manifest_sha256 STREQUAL
           "8b5bdc503105385c25139ee99ea37d0902cc097f3a47ce09f699d07dd822d761")
      message(FATAL_ERROR
        "BOOMPI_OPENSSL_HEADER_MANIFEST_MISMATCH: pinned RV1106 include/openssl tree differs")
    endif()
  endif()

  if(TARGET OpenSSL::SSL OR TARGET OpenSSL::Crypto)
    message(FATAL_ERROR
      "OpenSSL targets were defined before the pinned WSS dependency gate")
  endif()
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  set(OpenSSL_DIR "${_boompi_openssl_root}" CACHE PATH
    "Pinned OpenSSL package directory" FORCE)
  find_package(OpenSSL 3.5.7 EXACT CONFIG REQUIRED
    PATHS "${_boompi_openssl_root}"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH)
  if(NOT OpenSSL_VERSION STREQUAL "3.5.7")
    message(FATAL_ERROR
      "boomPI WSS requires exactly OpenSSL 3.5.7; found ${OpenSSL_VERSION}")
  endif()
  foreach(_boompi_openssl_target IN ITEMS OpenSSL::SSL OpenSSL::Crypto)
    if(NOT TARGET "${_boompi_openssl_target}")
      message(FATAL_ERROR
        "Pinned OpenSSL package did not define ${_boompi_openssl_target}")
    endif()
    get_target_property(_boompi_openssl_imported
      "${_boompi_openssl_target}" IMPORTED)
    get_target_property(_boompi_openssl_type
      "${_boompi_openssl_target}" TYPE)
    if(NOT _boompi_openssl_imported OR
       NOT _boompi_openssl_type STREQUAL "STATIC_LIBRARY")
      message(FATAL_ERROR
        "${_boompi_openssl_target} must be an imported static library")
    endif()
    get_target_property(_boompi_openssl_location
      "${_boompi_openssl_target}" IMPORTED_LOCATION)
    if(NOT EXISTS "${_boompi_openssl_location}")
      message(FATAL_ERROR
        "${_boompi_openssl_target} has no usable imported archive")
    endif()
    file(REAL_PATH "${_boompi_openssl_location}"
      _boompi_openssl_location)
    if(_boompi_openssl_target STREQUAL "OpenSSL::SSL")
      set(_boompi_openssl_expected "${_boompi_openssl_root}/libssl.a")
    else()
      set(_boompi_openssl_expected "${_boompi_openssl_root}/libcrypto.a")
    endif()
    file(REAL_PATH "${_boompi_openssl_expected}"
      _boompi_openssl_expected)
    if(NOT _boompi_openssl_location STREQUAL _boompi_openssl_expected)
      message(FATAL_ERROR
        "${_boompi_openssl_target} escaped the pinned OpenSSL package root")
    endif()
  endforeach()
  target_sources(boompi_network PRIVATE
    src/network/wss_client.cpp)
  target_link_libraries(boompi_network
    PUBLIC boompi_event boompi_protocol
    PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  if(WIN32)
    target_link_libraries(boompi_network PRIVATE ws2_32)
  endif()
endif()

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
  src/platform/rv1106/build_environment.cpp)
target_link_libraries(boompi_platform_rv1106 PUBLIC boompi_event)
if(BOOMPI_TARGET_RV1106)
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
    OUTPUT_NAME "boompi-snowboy-offline-probe")
endif()

if(BOOMPI_ENABLE_ALSA_PLAYBACK)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
      "BOOMPI_ENABLE_ALSA_PLAYBACK requires a Linux target")
  endif()
  find_package(ALSA REQUIRED)
  boompi_add_library(boompi_platform_alsa
    src/platform/rv1106/alsa_pcm_playback.cpp
    src/platform/rv1106/alsa_single_turn_io.cpp)
  target_link_libraries(boompi_platform_alsa
    PUBLIC boompi_platform_audio
    PRIVATE ALSA::ALSA)
  if(BOOMPI_BUILD_TESTS AND NOT BOOMPI_TARGET_RV1106)
    target_compile_definitions(boompi_platform_alsa PRIVATE
      BOOMPI_ALLOW_ALSA_NULL_TEST_PCM=1)
  endif()

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
if(BOOMPI_ENABLE_WSS_CLIENT AND BOOMPI_ENABLE_ALSA_PLAYBACK)
  target_sources(boompi_client PRIVATE
    src/application/manual_single_turn.cpp)
  target_compile_definitions(boompi_client PRIVATE
    BOOMPI_ENABLE_MANUAL_SINGLE_TURN=1)
  target_link_libraries(boompi_client PRIVATE
    boompi_audio_core
    boompi_network
    boompi_protocol
    Threads::Threads)
endif()
if(BOOMPI_TARGET_RV1106)
  target_compile_definitions(boompi_client PRIVATE BOOMPI_TARGET_RV1106=1)
endif()
set_target_properties(boompi_client PROPERTIES OUTPUT_NAME "boompi-client")
set_target_properties(boompi_client PROPERTIES
  SKIP_BUILD_RPATH TRUE
  BUILD_RPATH ""
  INSTALL_RPATH ""
  BUILD_WITH_INSTALL_RPATH FALSE
  INSTALL_RPATH_USE_LINK_PATH FALSE)

add_executable(boompi_supervisor apps/boompi_supervisor/main.cpp)
boompi_configure_target(boompi_supervisor)
target_link_libraries(boompi_supervisor PRIVATE boompi_update)
set_target_properties(boompi_supervisor PROPERTIES OUTPUT_NAME "boompi-supervisor")

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

  add_executable(boompi_server_control_tests
    tests/unit/server_control_tests.cpp)
  boompi_configure_target(boompi_server_control_tests)
  target_link_libraries(boompi_server_control_tests
    PRIVATE
      boompi_protocol
      boompi_test_support)
  add_test(
    NAME boompi_server_control_tests
    COMMAND boompi_server_control_tests)

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

    if(NOT BOOMPI_TARGET_RV1106)
      add_executable(boompi_alsa_single_turn_io_smoke
        tests/smoke/alsa_single_turn_io_smoke.cpp)
      boompi_configure_target(boompi_alsa_single_turn_io_smoke)
      target_link_libraries(boompi_alsa_single_turn_io_smoke
        PRIVATE boompi_platform_alsa)
      add_test(
        NAME boompi_alsa_single_turn_io_null_smoke
        COMMAND boompi_alsa_single_turn_io_smoke)
      set_tests_properties(
        boompi_alsa_single_turn_io_null_smoke
        PROPERTIES
          LABELS "alsa-null-api-flow-only"
          TIMEOUT 20)
    endif()
  endif()

  set_tests_properties(
    boompi_host_tests
    boompi_server_control_tests
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
    boompi_snowboy_wake_word_engine_tests
    PROPERTIES TIMEOUT 20)
endif()
if(BOOMPI_ENABLE_WSS_CLIENT)
  # This executable is intentionally part of ALL. It proves that the selected
  # target toolchain can resolve the complete static OpenSSL/WSS link closure,
  # and doubles as the bounded offline integration client.
  add_executable(boompi_wss_client_smoke
    tests/smoke/wss_client_smoke.cpp)
  boompi_configure_target(boompi_wss_client_smoke)
  target_link_libraries(boompi_wss_client_smoke
    PRIVATE boompi_network boompi_protocol Threads::Threads)
  set_target_properties(boompi_wss_client_smoke PROPERTIES
    OUTPUT_NAME "boompi-wss-client-smoke"
    SKIP_BUILD_RPATH TRUE
    BUILD_RPATH ""
    INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH_USE_LINK_PATH FALSE)

  if(BOOMPI_BUILD_TESTS)
    if(BOOMPI_GO_EXECUTABLE STREQUAL "")
      find_program(BOOMPI_GO_EXECUTABLE NAMES go)
    endif()
    if(BOOMPI_GO_EXECUTABLE)
      add_test(
        NAME boompi_cpp_wss_cross_language_smoke
        COMMAND "${CMAKE_COMMAND}" -E env
          "BOOMPI_CPP_WSS_SMOKE=$<TARGET_FILE:boompi_wss_client_smoke>"
          "GOWORK=off"
          "GOTOOLCHAIN=local"
          "GOPROXY=off"
          "GOSUMDB=off"
          "${BOOMPI_GO_EXECUTABLE}" test -count=1
          -run "^TestCppWSSHappyPath$" ./internal/app)
      set_tests_properties(boompi_cpp_wss_cross_language_smoke PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/server"
        TIMEOUT 30)
      if(BOOMPI_ENABLE_ALSA_PLAYBACK)
        add_test(
          NAME boompi_cpp_manual_single_turn_cross_language_smoke
          COMMAND "${CMAKE_COMMAND}" -E env
            "BOOMPI_CPP_MANUAL_SINGLE_TURN=$<TARGET_FILE:boompi_client>"
            "GOWORK=off"
            "GOTOOLCHAIN=local"
            "GOPROXY=off"
            "GOSUMDB=off"
            "${BOOMPI_GO_EXECUTABLE}" test -count=1
            -run "^TestCppManualSingleTurnHappyPath$" ./internal/app)
        set_tests_properties(
          boompi_cpp_manual_single_turn_cross_language_smoke
          PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/server"
            TIMEOUT 30)
      endif()
    else()
      message(STATUS
        "Go was not found; cross-language WSS smoke test is not registered")
    endif()
  endif()
endif()
