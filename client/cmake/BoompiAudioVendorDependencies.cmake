include_guard(GLOBAL)

option(BOOMPI_ENABLE_ROCKCHIP_3A
  "Enable the pinned Rockchip RV1106 3A feasibility dependency" OFF)
option(BOOMPI_BUILD_ROCKCHIP_3A_HIL
  "Build the explicit Rockchip RV1106 fixed-frame 3A HIL probe" OFF)
option(BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO
  "Enable the pinned Rockchip RV1106 MPI audio feasibility dependency" OFF)
option(BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL
  "Build the explicit Rockchip RV1106 MPI audio hardware-in-the-loop probe" OFF)
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

set(BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR "" CACHE PATH
  "Directory containing the pinned Rockchip MPI audio headers")
set(BOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY "" CACHE FILEPATH
  "Pinned Rockchip Rockit shared library")
set(BOOMPI_ROCKCHIP_MPI_MPP_LIBRARY "" CACHE FILEPATH
  "Pinned Rockchip MPP shared library")
set(BOOMPI_ROCKCHIP_MPI_RGA_LIBRARY "" CACHE FILEPATH
  "Pinned Rockchip RGA shared library")

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

set(_BOOMPI_ROCKCHIP_MPI_AI_HEADER_SHA256
  "5eb52c01056bdf6cdb4948a2a39d58172460dbcf7700e279774942f507b011cd")
set(_BOOMPI_ROCKCHIP_MPI_AO_HEADER_SHA256
  "e297104409a67f5d794bc111f900faae91b453f7255a0ed858f163a21201d618")
set(_BOOMPI_ROCKCHIP_MPI_AIO_HEADER_SHA256
  "95a76ae4d8dbd29563094c2e33ed5e200aeeef8ef6bc4426ff0ab34239d91867")
set(_BOOMPI_ROCKCHIP_MPI_SYS_HEADER_SHA256
  "0b7d08b59d437acfb2bbbdabfbb39b77631b34cd904b2ebd041ba34c98fcbac9")
set(_BOOMPI_ROCKCHIP_MPI_MB_HEADER_SHA256
  "0c54ef75e4904096165e6229469e75bb981ebb535ee8cd1699b6bb27857375cf")
set(_BOOMPI_ROCKCHIP_MPI_COMM_MB_HEADER_SHA256
  "7ba6b839615f7c62340562d93db2d01e2cf5d3e47f90d6f7b68e0b685a4ddd39")
set(_BOOMPI_ROCKCHIP_MPI_COMMON_HEADER_SHA256
  "ef3da84bf65e727de587be7665f29dbdb135326549736dbed0c1366d53e2b418")
set(_BOOMPI_ROCKCHIP_MPI_TYPE_HEADER_SHA256
  "1ca5eabff89c39034a5be31185a13709da0f697f3f9cac7637e41ea59bed924f")
set(_BOOMPI_ROCKCHIP_MPI_ROCKIT_SHA256
  "3f92f8c41ffe9ad72e407b68750906fcff89ea06758f14a3fc2a3d87061e3d0f")
set(_BOOMPI_ROCKCHIP_MPI_MPP_SHA256
  "e8183339fff1dd466adc9567be5c4c98239c567157eaebefe4a2fe50f793fec8")
set(_BOOMPI_ROCKCHIP_MPI_RGA_SHA256
  "13cf7d10210cdf43a998a07a9bf0033821dfec61b31d9c50195848c0480010c7")

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
      boompi_vendor::rockchip_3a_detect
      boompi_rockchip_3a_link_check)
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

  # This executable is part of ALL whenever the feasibility-only Rockchip 3A
  # dependency is enabled. It is never installed or run automatically; its
  # only purpose is to force the target linker to resolve the three direct 3A
  # entry points instead of accepting unused imported targets under
  # --as-needed.
  add_executable(boompi_rockchip_3a_link_check
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tests/link/rockchip_3a_link_check.cpp")
  target_compile_features(boompi_rockchip_3a_link_check PRIVATE cxx_std_17)
  target_include_directories(boompi_rockchip_3a_link_check PRIVATE
    "${_include_dir}")
  target_link_libraries(boompi_rockchip_3a_link_check PRIVATE
    boompi_vendor::rockchip_3a_aec)
  set_target_properties(boompi_rockchip_3a_link_check PROPERTIES
    SKIP_BUILD_RPATH TRUE)

  # Keep the validated non-library inputs part of configure dependency
  # tracking without adding host paths to target interfaces.
  unset(_header)
  unset(_config_file)
  set_property(GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_3A_CONFIGURED TRUE)
endfunction()

function(_boompi_configure_rockchip_3a_hil)
  if(NOT BOOMPI_BUILD_ROCKCHIP_3A_HIL)
    return()
  endif()
  if(NOT BOOMPI_ENABLE_ROCKCHIP_3A)
    message(FATAL_ERROR
      "BOOMPI_BUILD_ROCKCHIP_3A_HIL requires "
      "BOOMPI_ENABLE_ROCKCHIP_3A=ON")
  endif()

  # Repeat both gates here because isolated test fixtures intentionally call
  # this private constructor directly. No private input may be inspected until
  # the target ABI and Debug-only feasibility opt-in are established.
  _boompi_audio_vendor_require_rv1106_environment()
  _boompi_audio_vendor_require_feasibility_mode()

  get_property(_already_configured GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_3A_HIL_CONFIGURED)
  if(_already_configured)
    return()
  endif()

  if(NOT TARGET boompi_vendor::rockchip_3a_aec)
    message(FATAL_ERROR
      "Rockchip 3A dependency must be configured before its HIL probe")
  endif()
  if(TARGET boompi_rockchip_3a_hil)
    message(FATAL_ERROR "Rockchip 3A HIL target name collision")
  endif()

  _boompi_audio_vendor_require_directory(
    "BOOMPI_ROCKCHIP_3A_INCLUDE_DIR"
    "${BOOMPI_ROCKCHIP_3A_INCLUDE_DIR}"
    _include_dir)

  set(_hil_source
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tests/hil/rockchip_3a_hil.cpp")
  if(NOT EXISTS "${_hil_source}" OR IS_DIRECTORY "${_hil_source}")
    message(FATAL_ERROR "Rockchip 3A HIL source is missing")
  endif()
  file(SHA256 "${_hil_source}" _hil_source_sha256)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_hil_source}")

  # Only stable logical names and pins enter the executable. The direct API
  # does not read the VQE JSON, but its audited feasibility pin remains part of
  # the provenance set; no workstation or SDK path is embedded.
  string(CONCAT _pinset_manifest
    "rkaudio_preprocess.h=${_BOOMPI_ROCKCHIP_3A_HEADER_SHA256}\n"
    "libaec_bf_process.so=${_BOOMPI_ROCKCHIP_3A_AEC_SHA256}\n"
    "librkaudio_common.so=${_BOOMPI_ROCKCHIP_3A_COMMON_SHA256}\n"
    "librkaudio_detect.so=${_BOOMPI_ROCKCHIP_3A_DETECT_SHA256}\n"
    "config_aivqe.json=${_BOOMPI_ROCKCHIP_3A_CONFIG_SHA256}\n")
  string(SHA256 _pinset_sha256 "${_pinset_manifest}")

  # The target must be named explicitly. It is neither installed nor added to
  # CTest, and configure/build never runs it as a side effect.
  add_executable(boompi_rockchip_3a_hil EXCLUDE_FROM_ALL
    "${_hil_source}")
  target_compile_features(boompi_rockchip_3a_hil PRIVATE cxx_std_17)
  target_compile_options(boompi_rockchip_3a_hil PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    "-fdebug-prefix-map=${_include_dir}=boompi-rv1106-3a-include")
  if(BOOMPI_STRICT_WARNINGS)
    target_compile_options(boompi_rockchip_3a_hil PRIVATE -Werror)
  endif()
  target_compile_definitions(boompi_rockchip_3a_hil PRIVATE
    "BOOMPI_ROCKCHIP_3A_HIL_PINSET_SHA256=\"${_pinset_sha256}\""
    "BOOMPI_ROCKCHIP_3A_HIL_SOURCE_SHA256=\"${_hil_source_sha256}\"")
  target_include_directories(boompi_rockchip_3a_hil PRIVATE
    "${_include_dir}")
  # Keep the default dry-run genuinely vendor-load-free. The existing
  # boompi_rockchip_3a_link_check target owns direct link/ABI evidence; this
  # probe resolves the pinned API only after both runtime opt-ins and safety
  # preconditions have passed.
  target_link_libraries(boompi_rockchip_3a_hil PRIVATE
    ${CMAKE_DL_LIBS})
  set_target_properties(boompi_rockchip_3a_hil PROPERTIES
    SKIP_BUILD_RPATH TRUE
    BUILD_RPATH ""
    INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH_USE_LINK_PATH FALSE)

  unset(_pinset_manifest)
  set_property(GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_3A_HIL_CONFIGURED TRUE)
endfunction()

function(_boompi_configure_rockchip_mpi_audio_dependency)
  get_property(_already_configured GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_MPI_AUDIO_CONFIGURED)
  if(_already_configured)
    return()
  endif()

  _boompi_audio_vendor_require_directory(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR"
    "${BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR}"
    _include_dir)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR/rk_mpi_ai.h"
    "${_include_dir}/rk_mpi_ai.h"
    "${_BOOMPI_ROCKCHIP_MPI_AI_HEADER_SHA256}"
    _ai_header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR/rk_mpi_ao.h"
    "${_include_dir}/rk_mpi_ao.h"
    "${_BOOMPI_ROCKCHIP_MPI_AO_HEADER_SHA256}"
    _ao_header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR/rk_comm_aio.h"
    "${_include_dir}/rk_comm_aio.h"
    "${_BOOMPI_ROCKCHIP_MPI_AIO_HEADER_SHA256}"
    _aio_header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR/rk_mpi_sys.h"
    "${_include_dir}/rk_mpi_sys.h"
    "${_BOOMPI_ROCKCHIP_MPI_SYS_HEADER_SHA256}"
    _sys_header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR/rk_mpi_mb.h"
    "${_include_dir}/rk_mpi_mb.h"
    "${_BOOMPI_ROCKCHIP_MPI_MB_HEADER_SHA256}"
    _mb_header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR/rk_comm_mb.h"
    "${_include_dir}/rk_comm_mb.h"
    "${_BOOMPI_ROCKCHIP_MPI_COMM_MB_HEADER_SHA256}"
    _comm_mb_header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR/rk_common.h"
    "${_include_dir}/rk_common.h"
    "${_BOOMPI_ROCKCHIP_MPI_COMMON_HEADER_SHA256}"
    _common_header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR/rk_type.h"
    "${_include_dir}/rk_type.h"
    "${_BOOMPI_ROCKCHIP_MPI_TYPE_HEADER_SHA256}"
    _type_header)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY"
    "${BOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY}"
    "${_BOOMPI_ROCKCHIP_MPI_ROCKIT_SHA256}"
    _rockit_library)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_MPP_LIBRARY"
    "${BOOMPI_ROCKCHIP_MPI_MPP_LIBRARY}"
    "${_BOOMPI_ROCKCHIP_MPI_MPP_SHA256}"
    _mpp_library)
  boompi_audio_vendor_require_pinned_file(
    "BOOMPI_ROCKCHIP_MPI_RGA_LIBRARY"
    "${BOOMPI_ROCKCHIP_MPI_RGA_LIBRARY}"
    "${_BOOMPI_ROCKCHIP_MPI_RGA_SHA256}"
    _rga_library)

  foreach(_target IN ITEMS
      boompi_vendor_rockchip_mpp
      boompi_vendor_rockchip_rga
      boompi_vendor_rockchip_rockit
      boompi_vendor::rockchip_mpp
      boompi_vendor::rockchip_rga
      boompi_vendor::rockchip_rockit
      boompi_rockchip_mpi_audio_link_check)
    if(TARGET ${_target})
      message(FATAL_ERROR "Rockchip MPI audio imported target name collision")
    endif()
  endforeach()

  add_library(boompi_vendor_rockchip_mpp SHARED IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_rockchip_mpp PROPERTIES
    IMPORTED_LOCATION "${_mpp_library}"
    IMPORTED_SONAME "librockchip_mpp.so.1")
  add_library(boompi_vendor::rockchip_mpp ALIAS
    boompi_vendor_rockchip_mpp)

  add_library(boompi_vendor_rockchip_rga SHARED IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_rockchip_rga PROPERTIES
    IMPORTED_LOCATION "${_rga_library}"
    IMPORTED_SONAME "librga.so")
  add_library(boompi_vendor::rockchip_rga ALIAS
    boompi_vendor_rockchip_rga)

  add_library(boompi_vendor_rockchip_rockit SHARED IMPORTED GLOBAL)
  set_target_properties(boompi_vendor_rockchip_rockit PROPERTIES
    IMPORTED_LOCATION "${_rockit_library}"
    IMPORTED_SONAME "librockit.so"
    INTERFACE_LINK_LIBRARIES
      "boompi_vendor::rockchip_mpp;boompi_vendor::rockchip_rga")
  add_library(boompi_vendor::rockchip_rockit ALIAS
    boompi_vendor_rockchip_rockit)

  # This executable is part of ALL whenever the feasibility-only Rockchip MPI
  # audio dependency is enabled. It is never installed or run automatically;
  # it only checks the pinned headers' types and forces the target linker to
  # resolve the raw SYS/MB, AI, and AO entry points.
  add_executable(boompi_rockchip_mpi_audio_link_check
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tests/link/rockchip_mpi_audio_link_check.cpp")
  target_compile_features(boompi_rockchip_mpi_audio_link_check PRIVATE
    cxx_std_17)
  target_include_directories(boompi_rockchip_mpi_audio_link_check PRIVATE
    "${_include_dir}")
  target_link_libraries(boompi_rockchip_mpi_audio_link_check PRIVATE
    boompi_vendor::rockchip_rockit)
  set_target_properties(boompi_rockchip_mpi_audio_link_check PROPERTIES
    SKIP_BUILD_RPATH TRUE)

  unset(_ai_header)
  unset(_ao_header)
  unset(_aio_header)
  unset(_sys_header)
  unset(_mb_header)
  unset(_comm_mb_header)
  unset(_common_header)
  unset(_type_header)
  set_property(GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_MPI_AUDIO_CONFIGURED TRUE)
endfunction()

function(_boompi_configure_rockchip_mpi_audio_hil)
  if(NOT BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL)
    return()
  endif()
  if(NOT BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO)
    message(FATAL_ERROR
      "BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL requires "
      "BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON")
  endif()

  # Keep this gate local as well as at the public entry point. This function is
  # intentionally callable by isolated fixtures, and no private vendor input
  # may be inspected until the target ABI and Debug-only feasibility opt-in
  # have both been established.
  _boompi_audio_vendor_require_rv1106_environment()
  _boompi_audio_vendor_require_feasibility_mode()

  get_property(_already_configured GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_MPI_AUDIO_HIL_CONFIGURED)
  if(_already_configured)
    return()
  endif()

  if(NOT TARGET boompi_vendor::rockchip_rockit)
    message(FATAL_ERROR
      "Rockchip MPI audio dependency must be configured before its HIL probe")
  endif()
  if(TARGET boompi_rockchip_mpi_audio_hil)
    message(FATAL_ERROR "Rockchip MPI audio HIL target name collision")
  endif()

  _boompi_audio_vendor_require_directory(
    "BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR"
    "${BOOMPI_ROCKCHIP_MPI_INCLUDE_DIR}"
    _include_dir)

  set(_hil_source
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tests/hil/rockchip_mpi_audio_hil.cpp")
  if(NOT EXISTS "${_hil_source}" OR IS_DIRECTORY "${_hil_source}")
    message(FATAL_ERROR "Rockchip MPI audio HIL source is missing")
  endif()
  file(SHA256 "${_hil_source}" _hil_source_sha256)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_hil_source}")

  # The manifest contains only stable logical names and content pins. In
  # particular, it must never absorb a workstation, SDK, or sysroot path.
  string(CONCAT _pinset_manifest
    "rk_mpi_ai.h=${_BOOMPI_ROCKCHIP_MPI_AI_HEADER_SHA256}\n"
    "rk_mpi_ao.h=${_BOOMPI_ROCKCHIP_MPI_AO_HEADER_SHA256}\n"
    "rk_comm_aio.h=${_BOOMPI_ROCKCHIP_MPI_AIO_HEADER_SHA256}\n"
    "rk_mpi_sys.h=${_BOOMPI_ROCKCHIP_MPI_SYS_HEADER_SHA256}\n"
    "rk_mpi_mb.h=${_BOOMPI_ROCKCHIP_MPI_MB_HEADER_SHA256}\n"
    "rk_comm_mb.h=${_BOOMPI_ROCKCHIP_MPI_COMM_MB_HEADER_SHA256}\n"
    "rk_common.h=${_BOOMPI_ROCKCHIP_MPI_COMMON_HEADER_SHA256}\n"
    "rk_type.h=${_BOOMPI_ROCKCHIP_MPI_TYPE_HEADER_SHA256}\n"
    "librockit.so=${_BOOMPI_ROCKCHIP_MPI_ROCKIT_SHA256}\n"
    "librockchip_mpp.so.1=${_BOOMPI_ROCKCHIP_MPI_MPP_SHA256}\n"
    "librga.so=${_BOOMPI_ROCKCHIP_MPI_RGA_SHA256}\n")
  string(SHA256 _pinset_sha256 "${_pinset_manifest}")

  if(NOT TARGET Threads::Threads)
    find_package(Threads REQUIRED)
  endif()

  # This probe is deliberately opt-in and must be requested by target name. It
  # is neither installed nor registered with CTest, and nothing runs it as a
  # configure/build side effect.
  add_executable(boompi_rockchip_mpi_audio_hil EXCLUDE_FROM_ALL
    "${_hil_source}")
  target_compile_features(boompi_rockchip_mpi_audio_hil PRIVATE cxx_std_17)
  target_compile_options(boompi_rockchip_mpi_audio_hil PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    "-fdebug-prefix-map=${_include_dir}=boompi-rv1106-mpi-include")
  if(BOOMPI_STRICT_WARNINGS)
    target_compile_options(boompi_rockchip_mpi_audio_hil PRIVATE -Werror)
  endif()
  target_compile_definitions(boompi_rockchip_mpi_audio_hil PRIVATE
    "BOOMPI_ROCKCHIP_MPI_HIL_PINSET_SHA256=\"${_pinset_sha256}\""
    "BOOMPI_ROCKCHIP_MPI_HIL_SOURCE_SHA256=\"${_hil_source_sha256}\"")
  target_include_directories(boompi_rockchip_mpi_audio_hil PRIVATE
    "${_include_dir}")
  target_link_libraries(boompi_rockchip_mpi_audio_hil PRIVATE
    Threads::Threads
    boompi_vendor::rockchip_rockit)
  set_target_properties(boompi_rockchip_mpi_audio_hil PROPERTIES
    SKIP_BUILD_RPATH TRUE
    BUILD_RPATH ""
    INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH_USE_LINK_PATH FALSE)

  unset(_pinset_manifest)
  set_property(GLOBAL PROPERTY
    BOOMPI_AUDIO_VENDOR_ROCKCHIP_MPI_AUDIO_HIL_CONFIGURED TRUE)
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
  if(BOOMPI_BUILD_ROCKCHIP_3A_HIL AND
      NOT BOOMPI_ENABLE_ROCKCHIP_3A)
    message(FATAL_ERROR
      "BOOMPI_BUILD_ROCKCHIP_3A_HIL requires "
      "BOOMPI_ENABLE_ROCKCHIP_3A=ON")
  endif()
  if(BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL AND
      NOT BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO)
    message(FATAL_ERROR
      "BOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL requires "
      "BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON")
  endif()

  if(NOT BOOMPI_ENABLE_ROCKCHIP_3A AND
      NOT BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO AND
      NOT BOOMPI_ENABLE_SNOWBOY)
    return()
  endif()

  _boompi_audio_vendor_require_rv1106_environment()
  _boompi_audio_vendor_require_feasibility_mode()

  if(BOOMPI_ENABLE_ROCKCHIP_3A)
    _boompi_configure_rockchip_3a_dependency()
    _boompi_configure_rockchip_3a_hil()
  endif()
  if(BOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO)
    _boompi_configure_rockchip_mpi_audio_dependency()
    _boompi_configure_rockchip_mpi_audio_hil()
  endif()
  if(BOOMPI_ENABLE_SNOWBOY)
    _boompi_configure_snowboy_dependency()
  endif()
endfunction()
