# 教师统一准备 SDK；学生只设置一个根目录。已有显式 BOOMPI_* 配置仍优先。
if(NOT BOOMPI_RV1106_SDK_ROOT AND DEFINED ENV{BOOMPI_RV1106_SDK_ROOT})
  file(TO_CMAKE_PATH "$ENV{BOOMPI_RV1106_SDK_ROOT}" BOOMPI_RV1106_SDK_ROOT)
endif()
if(NOT BOOMPI_RV1106_SDK_ROOT)
  return()
endif()
if(NOT IS_DIRECTORY "${BOOMPI_RV1106_SDK_ROOT}")
  message(FATAL_ERROR "BOOMPI_RV1106_SDK_ROOT must name an existing SDK directory")
endif()
file(REAL_PATH "${BOOMPI_RV1106_SDK_ROOT}" BOOMPI_RV1106_SDK_ROOT)
if(EXISTS "${BOOMPI_RV1106_SDK_ROOT}/boompi-sdk.cmake")
  # 可用教师维护的清单映射既有 SDK，无需复制私有二进制。
  include("${BOOMPI_RV1106_SDK_ROOT}/boompi-sdk.cmake")
else()
  set(_boompi_sdk_entries
    "RV1106_TOOLCHAIN_ROOT|toolchain"
    "RV1106_SYSROOT|sysroot"
    "ROCKCHIP_3A_INCLUDE_DIR|rockchip/include"
    "ROCKCHIP_3A_AEC_LIBRARY|rockchip/lib/libaec_bf_process.so"
    "ROCKCHIP_3A_COMMON_LIBRARY|rockchip/lib/librkaudio_common.so"
    "SNOWBOY_INCLUDE_DIR|snowboy/include"
    "SNOWBOY_LIBRARY|snowboy/lib/libsnowboy-detect.a"
    "OPENBLAS_LIBRARY|snowboy/lib/libopenblas.a"
    "WEBRTC_VAD_INCLUDE_DIR|webrtc/include"
    "WEBRTC_VAD_LIBRARY|webrtc/lib/libwebrtc_vad.a"
    "BOOST_INCLUDE_DIR|boost/include"
    "OPENSSL_ROOT|openssl"
    "LVGL_ROOT|lvgl")
  foreach(entry IN LISTS _boompi_sdk_entries)
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 name)
    list(GET pair 1 relative)
    if(NOT BOOMPI_${name})
      set(BOOMPI_${name} "${BOOMPI_RV1106_SDK_ROOT}/${relative}" CACHE PATH
        "Path supplied by the teaching SDK" FORCE)
    endif()
  endforeach()
endif()
