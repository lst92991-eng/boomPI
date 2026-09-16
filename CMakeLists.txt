cmake_minimum_required(VERSION 3.21)
project(boomPI VERSION 1.0.0 LANGUAGES C CXX)

option(BOOMPI_STRICT_WARNINGS "Treat warnings in boomPI sources as errors" ON)
include("${CMAKE_CURRENT_SOURCE_DIR}/client/cmake/BoompiSdk.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/client/cmake/BoompiAudioVendorDependencies.cmake")
boompi_configure_audio_vendor_dependencies()
add_subdirectory(client)
