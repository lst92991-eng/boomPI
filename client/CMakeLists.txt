include(GNUInstallDirs)
find_package(Threads REQUIRED)

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
  src/audio/audio_format.cpp
  src/audio/audio_frame.cpp
  src/audio/capture_channel_mapper.cpp
  src/audio/fir_decimator_48_to_16.cpp
  src/audio/frame_continuity_gate.cpp)
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
  src/platform/rv1106/build_environment.cpp)
target_link_libraries(boompi_platform_rv1106 PUBLIC boompi_event)
if(BOOMPI_TARGET_RV1106)
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

install(TARGETS boompi_client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS boompi_supervisor RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

if(BOOMPI_BUILD_TESTS)
  add_library(boompi_test_support STATIC
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

  set_tests_properties(
    boompi_host_tests
    boompi_audio_queue_tests
    boompi_audio_continuity_tests
    boompi_audio_channel_mapper_tests
    boompi_audio_fir_decimator_tests
    PROPERTIES TIMEOUT 20)
endif()
