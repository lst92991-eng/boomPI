cmake_minimum_required(VERSION 3.21)

project(boomPI VERSION 1.0.0 LANGUAGES C CXX)

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
  # 源码职责、ELOC 预算、唯一候选 preset 和文档链接属于教学版的可复现契约。
  # 该检查只依赖 Python 标准库；没有解释器时不妨碍板端交叉配置。
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    add_test(NAME boompi_source_contract
             COMMAND ${Python3_EXECUTABLE}
                     "${CMAKE_SOURCE_DIR}/scripts/tests/test_client_source_contract.py")
  endif()
endif()

add_subdirectory(client)
