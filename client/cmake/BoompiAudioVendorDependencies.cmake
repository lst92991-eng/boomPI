include_guard(GLOBAL)

option(BOOMPI_ENABLE_ROCKCHIP_3A
  "Enable the pinned Rockchip RV1106 3A feasibility dependency" OFF)
option(BOOMPI_ENABLE_SNOWBOY
  "Enable the pinned Snowboy wake-word feasibility dependency" OFF)
option(BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS
  "Allow pinned, non-release audio vendor candidates in Debug probes" OFF)

set(BOOMPI_ROCKCHIP_3A_INCLUDE_DIR "" CACHE PATH
  "Directory containing the pinned Rockchip 3A header")
set(BOOMPI_ROCKCHIP_3A_AEC_LIBRARY "" CACHE FILEPATH
  "Pinned Rockchip AEC/BF shared library")
set(BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY "" CACHE FILEPATH
  "Pinned Rockchip audio common shared library")
set(BOOMPI_ROCKCHIP_3A_DETECT_LIBRARY "" CACHE FILEPATH
  "Pinned Rockchip audio detection shared library")
set(BOOMPI_ROCKCHIP_3A_CONFIG_FILE "" CACHE FILEPATH
  "Pinned Rockchip 3A runtime configuration")

set(BOOMPI_SNOWBOY_INCLUDE_DIR "" CACHE PATH
  "Directory containing the pinned Snowboy header")
set(BOOMPI_SNOWBOY_LIBRARY "" CACHE FILEPATH
  "Pinned Snowboy static library")
set(BOOMPI_SNOWBOY_RESOURCE_FILE "" CACHE FILEPATH
  "Pinned Snowboy common resource file")
set(BOOMPI_SNOWBOY_MODEL_FILE "" CACHE FILEPATH
  "Pinned Snowboy wake-word model")
set(BOOMPI_OPENBLAS_LIBRARY "" CACHE FILEPATH
  "Pinned OpenBLAS static library required by Snowboy")

# These pins identify P0 feasibility candidates, not release-approved inputs.
# They are ordinary variables so a local cache entry cannot redefine them.
set(_BOOMPI_ROCKCHIP_3A_HEADER_SHA256
  "b9bbf723d8e5bfdc421cf45fdf5853fec1584737d0e20682cd0db6bae5a7b54d")
set(_BOOMPI_ROCKCHIP_3A_AEC_SHA256
  "5abbcf518ffa39900dd78352547ebf5feab83d2f9b30a82c1e2dc1dc44b25e07")
set(_BOOMPI_ROCKCHIP_3A_COMMON_SHA256
  "4f4c9d78028a592174c3e959e35d231317d0ed2a864a1ed230f8adab42960246")
set(_BOOMPI_ROCKCHIP_3A_DETECT_SHA256
  "f84b66a2d1d561fbb3c36e288a57f1e9ef50990974d9be9accbb0aaebcbae396")
set(_BOOMPI_ROCKCHIP_3A_CONFIG_SHA256
  "1d160fde184935cf43a49feae7be0dfd24efdc82ff9de2ea8b35aba6318074f9")

set(_BOOMPI_SNOWBOY_HEADER_SHA256
  "f203e88bccd3782b9fdfaa5f02ea2fab402671f415c9eae3609b67c1e622a363")
set(_BOOMPI_SNOWBOY_LIBRARY_SHA256
  "346db1193490a9cc404d49fcfb22ca612cd3a0e649c4863f411553eb1c4f9f1f")
set(_BOOMPI_SNOWBOY_RESOURCE_SHA256
  "5dd5258678182f2e055fa7a6167eba50ded3bf8b41f70faab11fd9b221de488b")
set(_BOOMPI_SNOWBOY_MODEL_SHA256
  "7ccc61effbe05c27d8fd3428bf27e71578d2eddcc97ac9c1437fa0f9cacc64f1")
set(_BOOMPI_OPENBLAS_LIBRARY_SHA256
  "fabfc588e0e0d94f3655d4ad5515e0c90fd161f016be5261e2f11d3df77a3e9d")

# This helper is intentionally callable by an isolated CMake test fixture.
# logical_name is the only caller-controlled value included in diagnostics;
# input_path is never echoed.
function(boompi_audio_vendor_require_pinned_file
    logical_name input_path expected_sha256 output_variable)
  if(NOT ARGC EQUAL 4 OR logical_name STREQUAL "" OR
      output_variable STREQUAL "")
    message(FATAL_ERROR
      "audio vendor pinned-file helper received an invalid argument contract")
  endif()

  string(LENGTH "${expected_sha256}" _expected_length)
  if(NOT _expected_length EQUAL 64 OR
      NOT expected_sha256 MATCHES "^[0-9A-Fa-f]+$")
    message(FATAL_ERROR "${logical_name}: expected SHA-256 pin is invalid")
  endif()
  if(input_path STREQUAL "")
    message(FATAL_ERROR "${logical_name}: an explicit file path is required")
  endif()
  if(input_path MATCHES ";")
    message(FATAL_ERROR "${logical_name}: the file path is invalid")
  endif()
  if(NOT IS_ABSOLUTE "${input_path}")
    message(FATAL_ERROR "${logical_name}: the file path must be absolute")
  endif()
  if(NOT EXISTS "${input_path}")
    message(FATAL_ERROR "${logical_name}: the file does not exist")
  endif()
  if(IS_DIRECTORY "${input_path}")
    message(FATAL_ERROR "${logical_name}: a regular file is required")
  endif()

  file(REAL_PATH "${input_path}" _resolved_path)
  file(SHA256 "${_resolved_path}" _observed_sha256)
  string(TOLOWER "${expected_sha256}" _expected_sha256)
  string(TOLOWER "${_observed_sha256}" _observed_sha256)
  if(NOT _observed_sha256 STREQUAL _expected_sha256)
    message(FATAL_ERROR "${logical_name}: SHA-256 mismatch")
  endif()

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_resolved_path}")
  set(${output_variable} "${_resolved_path}" PARENT_SCOPE)
endfunction()

function(_boompi_audio_vendor_require_directory
    logical_name input_path output_variable)
  if(NOT ARGC EQUAL 3 OR logical_name STREQUAL "" OR
      output_variable STREQUAL "")
    message(FATAL_ERROR
      "audio vendor directory helper received an invalid argument contract")
  endif()
  if(input_path STREQUAL "")
    message(FATAL_ERROR
      "${logical_name}: an explicit directory path is required")
  endif()
  if(input_path MATCHES ";")
    message(FATAL_ERROR "${logical_name}: the directory path is invalid")
  endif()
  if(NOT IS_ABSOLUTE "${input_path}")
    message(FATAL_ERROR "${logical_name}: the directory path must be absolute")
  endif()
  if(NOT IS_DIRECTORY "${input_path}")
    message(FATAL_ERROR "${logical_name}: the directory does not exist")
  endif()

  file(REAL_PATH "${input_path}" _resolved_path)
  set(${output_variable} "${_resolved_path}" PARENT_SCOPE)
endfunction()

function(_boompi_audio_vendor_require_rv1106_environment)
  if(NOT BOOMPI_TARGET_RV1106)
    message(FATAL_ERROR
      "audio vendor dependencies require BOOMPI_TARGET_RV1106=ON")
  endif()

  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _target_processor)
  if(NOT CMAKE_CROSSCOMPILING OR
      NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
      NOT _target_processor STREQUAL "arm")
    message(FATAL_ERROR
      "audio vendor dependencies require a cross-compiled Linux/ARM target")
  endif()

  if(CMAKE_CXX_COMPILER STREQUAL "" OR
      CMAKE_CXX_COMPILER MATCHES ";" OR
      NOT IS_ABSOLUTE "${CMAKE_CXX_COMPILER}" OR
      NOT EXISTS "${CMAKE_CXX_COMPILER}" OR
      IS_DIRECTORY "${CMAKE_CXX_COMPILER}")
    message(FATAL_ERROR
      "audio vendor dependencies require the pinned RV1106 compiler")
  endif()
  get_filename_component(_compiler_name "${CMAKE_CXX_COMPILER}" NAME)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
      NOT _compiler_name STREQUAL
        "arm-rockchip830-linux-uclibcgnueabihf-g++")
    message(FATAL_ERROR
      "audio vendor dependencies require the pinned RV1106 GNU compiler")
  endif()

  if(CMAKE_SYSROOT STREQUAL "" OR CMAKE_SYSROOT MATCHES ";" OR
      NOT IS_ABSOLUTE "${CMAKE_SYSROOT}" OR
      NOT IS_DIRECTORY "${CMAKE_SYSROOT}" OR
      NOT EXISTS "${CMAKE_SYSROOT}/lib/ld-uClibc.so.0")
    message(FATAL_ERROR
      "audio vendor dependencies require an RV1106 uClibc sysroot")
  endif()
endfunction()

function(_boompi_audio_vendor_require_feasibility_mode)
  if(NOT BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS)
    message(FATAL_ERROR
      "current audio vendor pins are feasibility-only; explicitly set "
      "BOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON for a non-release probe")
  endif()
  if(CMAKE_CONFIGURATION_TYPES)
    list(LENGTH CMAKE_CONFIGURATION_TYPES _configuration_count)
    list(GET CMAKE_CONFIGURATION_TYPES 0 _only_configuration)
    if(NOT _configuration_count EQUAL 1 OR
        NOT _only_configuration STREQUAL "Debug")
      message(FATAL_ERROR
        "feasibility audio vendor inputs require a Debug-only build")
    endif()
  elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
      "feasibility audio vendor inputs require a Debug-only build")
  endif()
endfunction()

function(_boompi_configure_rockchip_3a_dependency)
  get_property(_already_configured GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_3A_CONFIGURED)
  if(_already_configured)
    return()
  endif()

  _boompi_audio_vendor_require_directory(
    "BOOMPI_ROCKCHIP_3A_INCLUDE_DIR"
    "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}"
    _include_dir)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_3A_INCLUDE_DIR/rkaudio_preprocess.h"
    "${_include_dir}/rkaudio_preprocess.h"
    "${_BOOMPI_ROCKCHIP_3A_HEADER_SHA256}"
    _header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_3A_AEC_LIBRARY"
    "${BOOMPI_ROCKCHIP_3A_AEC_LIBRARY}"
    "${_BOOMPI_ROCKCHIP_3A_AEC_SHA256}"
    _aec_library)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY"
    "${BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY}"
    "${_BOOMPI_ROCKCHIP_3A_COMMON_SHA256}"
    _common_library)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_3A_DETECT_LIBRARY"
    "${BOOMPI_ROCKCHIP_3A_DETECT_LIBRARY}"
    "${_BOOMPI_ROCKCHIP_3A_DETECT_SHA256}"
    _detect_library)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_3A_CONFIG_FILE"
    "${BOOMPI_ROCKCHIP_3A_CONFIG_FILE}"
    "${_BOOMPI_ROCKCHIP_3A_CONFIG_SHA256}"
    _config_file)

  foreach(_target IN ITEMS
      boompi_vendor_rockchip_3a_common
      boompi_vendor_rockchip_3a_aec
      boompi_vendor_rockchip_3a_detect
      boompi_vendor::rockchip_3a_common
      boompi_vendor::rockchip_3a_aec
      boompi_vendor::rockchip_3a_detect)
    if(TARGET ${_target})
      message(FATAL_ERROR "Rockchip 3A imported target name collision")
    endif()
  endforeach()

  add_library(boompi_vendor_rockchip_3a_common SHARED IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_rockchip_3a_common PROPERTIES
    IMPORTED_LOCATION "${_common_library}"
    IMPORTED_SONAME "librkaudio_common.so")
  add_library(boompi_vendor::rockchip_3a_common ALIAS
    boompi_vendor_rockchip_3a_common)

  add_library(boompi_vendor_rockchip_3a_aec SHARED IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_rockchip_3a_aec PROPERTIES
    IMPORTED_LOCATION "${_aec_library}"
    IMPORTED_SONAME "libaec_bf_process.so"
    INTERFACE_LINK_LIBRARIES "boompi_vendor::rockchip_3a_common")
  add_library(boompi_vendor::rockchip_3a_aec ALIAS
    boompi_vendor_rockchip_3a_aec)

  add_library(boompi_vendor_rockchip_3a_detect SHARED IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_rockchip_3a_detect PROPERTIES
    IMPORTED_LOCATION "${_detect_library}"
    IMPORTED_SONAME "librkaudio_detect.so"
    INTERFACE_LINK_LIBRARIES "boompi_vendor::rockchip_3a_common")
  add_library(boompi_vendor::rockchip_3a_detect ALIAS
    boompi_vendor_rockchip_3a_detect)

  # Keep the validated non-library inputs part of configure dependency
  # tracking without adding host paths to target interfaces.
  unset(_header)
  unset(_config_file)
  set_property(GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_3A_CONFIGURED TRUE)
endfunction()

function(_boompi_configure_snowboy_dependency)
  get_property(_already_configured GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_SNOWBOY_CONFIGURED)
  if(_already_configured)
    return()
  endif()

  _boompi_audio_vendor_require_directory(
    "BOOMPI_SNOWBOY_INCLUDE_DIR"
    "${BOOMPI_SNOWBOY_INCLUDE_DIR}"
    _include_dir)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_SNOWBOY_INCLUDE_DIR/snowboy-detect.h"
    "${_include_dir}/snowboy-detect.h"
    "${_BOOMPI_SNOWBOY_HEADER_SHA256}"
    _header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_SNOWBOY_LIBRARY"
    "${BOOMPI_SNOWBOY_LIBRARY}"
    "${_BOOMPI_SNOWBOY_LIBRARY_SHA256}"
    _snowboy_library)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_SNOWBOY_RESOURCE_FILE"
    "${BOOMPI_SNOWBOY_RESOURCE_FILE}"
    "${_BOOMPI_SNOWBOY_RESOURCE_SHA256}"
    _resource_file)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_SNOWBOY_MODEL_FILE"
    "${BOOMPI_SNOWBOY_MODEL_FILE}"
    "${_BOOMPI_SNOWBOY_MODEL_SHA256}"
    _model_file)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_OPENBLAS_LIBRARY"
    "${BOOMPI_OPENBLAS_LIBRARY}"
    "${_BOOMPI_OPENBLAS_LIBRARY_SHA256}"
    _openblas_library)

  foreach(_target IN ITEMS
      boompi_vendor_openblas
      boompi_vendor_snowboy
      boompi_vendor::openblas
      boompi_vendor::snowboy)
    if(TARGET ${_target})
      message(FATAL_ERROR "Snowboy imported target name collision")
    endif()
  endforeach()

  add_library(boompi_vendor_openblas STATIC IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_openblas PROPERTIES
    IMPORTED_LOCATION "${_openblas_library}")
  add_library(boompi_vendor::openblas ALIAS boompi_vendor_openblas)

  add_library(boompi_vendor_snowboy STATIC IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_snowboy PROPERTIES
    IMPORTED_LOCATION "${_snowboy_library}"
    INTERFACE_LINK_LIBRARIES "boompi_vendor::openblas")
  add_library(boompi_vendor::snowboy ALIAS boompi_vendor_snowboy)

  # Resource paths are validated and tracked, but are deliberately not exposed
  # through compile definitions or transitive target properties.
  unset(_header)
  unset(_resource_file)
  unset(_model_file)
  set_property(GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_SNOWBOY_CONFIGURED TRUE)
endfunction()

function(boompi_configure_audio_vendor_dependencies)
  if(NOT BOOMPI_ENABLE_ROCKCHIP_3A AND NOT BOOMPI_ENABLE_SNOWBOY)
    return()
  endif()

  _boompi_audio_vendor_require_rv1106_environment()
  _boompi_audio_vendor_require_feasibility_mode()

  if(BOOMPI_ENABLE_ROCKCHIP_3A)
    _boompi_configure_rockchip_3a_dependency()
  endif()
  if(BOOMPI_ENABLE_SNOWBOY)
    _boompi_configure_snowboy_dependency()
  endif()
endfunction()
