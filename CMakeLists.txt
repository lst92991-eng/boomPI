cmake_minimum_required(VERSION 3.21)

project(boomPI VERSION 0.1.0 LANGUAGES CXX)

# 多配置生成器忽略该值；显式读取可让同一 preset 兼容 VS 与 Unix 单配置生成器。
if(DEFINED CMAKE_BUILD_TYPE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

option(BOOMPI_BUILD_TESTS "Build deterministic host tests" ON)
option(BOOMPI_STRICT_WARNINGS "Treat warnings in boomPI sources as errors" ON)
option(BOOMPI_TARGET_RV1106 "Build for the RV1106 target environment" OFF)

if(BOOMPI_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()

add_subdirectory(client)
