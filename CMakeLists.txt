cmake_minimum_required(VERSION 3.21)

project(boomPI VERSION 0.1.0 LANGUAGES C CXX)

# 多配置生成器忽略该值；显式读取可让同一 preset 兼容 VS 与 Unix 单配置生成器。
if(DEFINED CMAKE_BUILD_TYPE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

option(BOOMPI_BUILD_TESTS "Build deterministic host tests" ON)
option(BOOMPI_STRICT_WARNINGS "Treat warnings in boomPI sources as errors" ON)
option(BOOMPI_TARGET_RV1106 "Build for the RV1106 target environment" OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/client/cmake/BoompiAudioVendorDependencies.cmake")
boompi_configure_audio_vendor_dependencies()

if(BOOMPI_BUILD_TESTS)
  include(CTest)
  enable_testing()
  # 源码契约测试（ELOC 预算、文档数字、结构锚点）只需 Python，
  # 挂进 ctest 避免 enable_testing 后零测试的假门；无 Python 时安静跳过。
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    add_test(NAME boompi_source_contract
             COMMAND ${Python3_EXECUTABLE} -m unittest discover
                     -s "${CMAKE_SOURCE_DIR}/scripts/tests"
                     -t "${CMAKE_SOURCE_DIR}" -p "test_*.py")
  endif()
endif()

add_subdirectory(client)
