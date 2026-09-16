# 工具链/sysroot来自匹配的幸狐SDK；第三方源码和产物固定放在项目third_party。
get_filename_component(_boompi_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_boompi_third_party "${_boompi_root}/third_party")
if(NOT BOOMPI_RV1106_SDK_ROOT AND DEFINED ENV{BOOMPI_RV1106_SDK_ROOT})
  file(TO_CMAKE_PATH "$ENV{BOOMPI_RV1106_SDK_ROOT}" BOOMPI_RV1106_SDK_ROOT)
endif()
if(BOOMPI_RV1106_SDK_ROOT)
  set(BOOMPI_RV1106_TOOLCHAIN_ROOT
    "${BOOMPI_RV1106_SDK_ROOT}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"
    CACHE PATH "Luckfox GCC 8.3 toolchain")
  set(BOOMPI_RV1106_SYSROOT
    "${BOOMPI_RV1106_SDK_ROOT}/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot"
    CACHE PATH "Matching Luckfox target sysroot")
endif()
set(BOOMPI_OPENSSL_ROOT "${_boompi_third_party}/openssl/build/rv1106" CACHE PATH "OpenSSL ARM build")
set(BOOMPI_SNOWBOY_INCLUDE_DIR "${_boompi_third_party}/snowboy/include" CACHE PATH "Snowboy headers")
set(BOOMPI_SNOWBOY_LIBRARY "${_boompi_third_party}/snowboy/lib/rpi/libsnowboy-detect.a" CACHE FILEPATH "Upstream ARM archive")
set(BOOMPI_OPENBLAS_LIBRARY "${_boompi_third_party}/openblas/build/rv1106/libopenblas.a" CACHE FILEPATH "OpenBLAS ARM build")
set(BOOMPI_WEBRTC_VAD_ROOT "${_boompi_third_party}/webrtc-vad/cbits" CACHE PATH "WebRTC VAD source")
set(BOOMPI_WEBRTC_VAD_INCLUDE_DIR "${BOOMPI_WEBRTC_VAD_ROOT}/webrtc/common_audio/vad/include")
set(BOOMPI_BOOST_INCLUDE_DIR "${_boompi_third_party}/boost" CACHE PATH "Boost headers")
set(BOOMPI_LVGL_ROOT "${_boompi_third_party}/lvgl" CACHE PATH "LVGL 8.2 source")
set(BOOMPI_ROCKCHIP_3A_INCLUDE_DIR "${_boompi_third_party}/rockchip/include" CACHE PATH "Matching SDK headers")
set(BOOMPI_ROCKCHIP_3A_AEC_LIBRARY "${_boompi_third_party}/rockchip/lib/libaec_bf_process.so" CACHE FILEPATH "Matching SDK AEC library")
set(BOOMPI_ROCKCHIP_3A_COMMON_LIBRARY "${_boompi_third_party}/rockchip/lib/librkaudio_common.so" CACHE FILEPATH "Matching SDK common library")
