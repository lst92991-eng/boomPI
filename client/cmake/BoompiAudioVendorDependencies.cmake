include_guard(GLOBAL)

# 板端只依赖已经在 BSP 上验证过的 3A 与 Snowboy 二进制。
# 模型和运行参数由 /userdata/boompi/config/client.env 指定，不属于构建输入。
set(BOOMPI_ROCKCHIP_3A_INCLUDE_DIR "" CACHE PATH
  "Directory containing rkaudio_preprocess.h")
set(BOOMPI_ROCKCHIP_3A_AEC_LIBRARY "" CACHE FILEPATH
  "Rockchip libaec_bf_process.so")
set(BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY "" CACHE FILEPATH
  "Rockchip librkaudio_common.so")
set(BOOMPI_SNOWBOY_INCLUDE_DIR "" CACHE PATH
  "Directory containing snowboy-detect.h")
set(BOOMPI_SNOWBOY_LIBRARY "" CACHE FILEPATH
  "Snowboy static library for RV1106")
set(BOOMPI_OPENBLAS_LIBRARY "" CACHE FILEPATH
  "OpenBLAS static library required by Snowboy")

function(boompi_require_directory name path output)
  if(path STREQUAL "" OR NOT IS_ABSOLUTE "${path}" OR
      NOT IS_DIRECTORY "${path}")
    message(FATAL_ERROR "${name} must be an existing absolute directory")
  endif()
  file(REAL_PATH "${path}" resolved)
  set(${output} "${resolved}" PARENT_SCOPE)
endfunction()

function(boompi_require_file name path output)
  if(path STREQUAL "" OR NOT IS_ABSOLUTE "${path}" OR
      NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
    message(FATAL_ERROR "${name} must be an existing absolute file")
  endif()
  file(REAL_PATH "${path}" resolved)
  set(${output} "${resolved}" PARENT_SCOPE)
endfunction()

function(boompi_configure_audio_vendor_dependencies)
  if(NOT BOOMPI_TARGET_RV1106)
    return()
  endif()

  boompi_require_directory(BOOMPI_ROCKCHIP_3A_INCLUDE_DIR
    "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}" rockchip_include)
  boompi_require_file(BOOMPI_ROCKCHIP_3A_HEADER
    "${rockchip_include}/rkaudio_preprocess.h" rockchip_header)
  boompi_require_file(BOOMPI_ROCKCHIP_3A_AEC_LIBRARY
    "${BOOMPI_ROCKCHIP_3A_AEC_LIBRARY}" rockchip_aec)
  boompi_require_file(BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY
    "${BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY}" rockchip_common)

  add_library(boompi_vendor_rockchip_common SHARED IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_rockchip_common PROPERTIES
    IMPORTED_LOCATION "${rockchip_common}"
    IMPORTED_SONAME "librkaudio_common.so")
  add_library(boompi_vendor::rockchip_common ALIAS
    boompi_vendor_rockchip_common)

  add_library(boompi_vendor_rockchip_aec SHARED IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_rockchip_aec PROPERTIES
    IMPORTED_LOCATION "${rockchip_aec}"
    IMPORTED_SONAME "libaec_bf_process.so"
    INTERFACE_INCLUDE_DIRECTORIES "${rockchip_include}"
    INTERFACE_LINK_LIBRARIES "boompi_vendor::rockchip_common")
  add_library(boompi_vendor::rockchip_3a_aec ALIAS
    boompi_vendor_rockchip_aec)

  boompi_require_directory(BOOMPI_SNOWBOY_INCLUDE_DIR
    "${BOOMPI_SNOWBOY_INCLUDE_DIR}" snowboy_include)
  boompi_require_file(BOOMPI_SNOWBOY_HEADER
    "${snowboy_include}/snowboy-detect.h" snowboy_header)
  boompi_require_file(BOOMPI_SNOWBOY_LIBRARY
    "${BOOMPI_SNOWBOY_LIBRARY}" snowboy_library)
  boompi_require_file(BOOMPI_OPENBLAS_LIBRARY
    "${BOOMPI_OPENBLAS_LIBRARY}" openblas_library)

  add_library(boompi_vendor_openblas STATIC IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_openblas PROPERTIES
    IMPORTED_LOCATION "${openblas_library}")
  add_library(boompi_vendor::openblas ALIAS boompi_vendor_openblas)

  add_library(boompi_vendor_snowboy STATIC IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_snowboy PROPERTIES
    IMPORTED_LOCATION "${snowboy_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${snowboy_include}"
    INTERFACE_LINK_LIBRARIES "boompi_vendor::openblas")
  add_library(boompi_vendor::snowboy ALIAS boompi_vendor_snowboy)

  # 让缺失的头文件参与 CMake 重新配置，但不把本机路径写入目标 ELF。
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${rockchip_header}" "${snowboy_header}")
endfunction()
