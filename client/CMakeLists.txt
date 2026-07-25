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
  src/audio/audio_format.cpp)
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
  set_tests_properties(boompi_host_tests PROPERTIES TIMEOUT 10)
endif()
