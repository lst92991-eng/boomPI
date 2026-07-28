#include <type_traits>

#include "rk_mpi_ai.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

namespace {

using SysNoArgFunction = RK_S32 (*)(RK_VOID);
using SysCreateMbFunction = RK_S32 (*)(MB_BLK*, MB_EXT_CONFIG_S*);
using MbHandleToVirtualAddressFunction = RK_VOID* (*)(MB_BLK);
using MbGetSizeFunction = RK_U64 (*)(MB_BLK);
using MbReleaseFunction = RK_S32 (*)(MB_BLK);

using AiSetPublicAttributesFunction =
    RK_S32 (*)(AUDIO_DEV, const AIO_ATTR_S*);
using AiDeviceFunction = RK_S32 (*)(AUDIO_DEV);
using AiChannelFunction = RK_S32 (*)(AUDIO_DEV, AI_CHN);
using AiSetChannelParametersFunction =
    RK_S32 (*)(AUDIO_DEV, AI_CHN, const AI_CHN_PARAM_S*);
using AiGetFrameFunction = RK_S32 (*)(AUDIO_DEV,
                                      AI_CHN,
                                      AUDIO_FRAME_S*,
                                      AEC_FRAME_S*,
                                      RK_S32);
using AiReleaseFrameFunction = RK_S32 (*)(AUDIO_DEV,
                                          AI_CHN,
                                          const AUDIO_FRAME_S*,
                                          const AEC_FRAME_S*);

using AoSetPublicAttributesFunction =
    RK_S32 (*)(AUDIO_DEV, const AIO_ATTR_S*);
using AoDeviceFunction = RK_S32 (*)(AUDIO_DEV);
using AoChannelFunction = RK_S32 (*)(AUDIO_DEV, AO_CHN);
using AoSetChannelParametersFunction =
    RK_S32 (*)(AUDIO_DEV, AO_CHN, const AO_CHN_PARAM_S*);
using AoSendFrameFunction =
    RK_S32 (*)(AUDIO_DEV, AO_CHN, const AUDIO_FRAME_S*, RK_S32);
using AoWaitEosFunction = RK_S32 (*)(AUDIO_DEV, AO_CHN, RK_S32);

static_assert(
    std::is_same_v<decltype(&RK_MPI_SYS_Init), SysNoArgFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_SYS_Exit), SysNoArgFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_SYS_CreateMB), SysCreateMbFunction>);
static_assert(std::is_same_v<decltype(&RK_MPI_MB_Handle2VirAddr),
                             MbHandleToVirtualAddressFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_MB_GetSize), MbGetSizeFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_MB_ReleaseMB), MbReleaseFunction>);

static_assert(std::is_same_v<decltype(&RK_MPI_AI_SetPubAttr),
                             AiSetPublicAttributesFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AI_Enable), AiDeviceFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AI_EnableChn), AiChannelFunction>);
static_assert(std::is_same_v<decltype(&RK_MPI_AI_SetChnParam),
                             AiSetChannelParametersFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AI_GetFrame), AiGetFrameFunction>);
static_assert(std::is_same_v<decltype(&RK_MPI_AI_ReleaseFrame),
                             AiReleaseFrameFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AI_DisableChn), AiChannelFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AI_Disable), AiDeviceFunction>);

static_assert(std::is_same_v<decltype(&RK_MPI_AO_SetPubAttr),
                             AoSetPublicAttributesFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AO_Enable), AoDeviceFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AO_EnableChn), AoChannelFunction>);
static_assert(std::is_same_v<decltype(&RK_MPI_AO_SetChnParams),
                             AoSetChannelParametersFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AO_SendFrame), AoSendFrameFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AO_WaitEos), AoWaitEosFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AO_DisableChn), AoChannelFunction>);
static_assert(
    std::is_same_v<decltype(&RK_MPI_AO_Disable), AoDeviceFunction>);

SysNoArgFunction volatile kSysInitSymbol = &RK_MPI_SYS_Init;
SysNoArgFunction volatile kSysExitSymbol = &RK_MPI_SYS_Exit;
SysCreateMbFunction volatile kSysCreateMbSymbol = &RK_MPI_SYS_CreateMB;
MbHandleToVirtualAddressFunction volatile kMbHandleToVirtualAddressSymbol =
    &RK_MPI_MB_Handle2VirAddr;
MbGetSizeFunction volatile kMbGetSizeSymbol = &RK_MPI_MB_GetSize;
MbReleaseFunction volatile kMbReleaseSymbol = &RK_MPI_MB_ReleaseMB;

AiSetPublicAttributesFunction volatile kAiSetPublicAttributesSymbol =
    &RK_MPI_AI_SetPubAttr;
AiDeviceFunction volatile kAiEnableSymbol = &RK_MPI_AI_Enable;
AiChannelFunction volatile kAiEnableChannelSymbol = &RK_MPI_AI_EnableChn;
AiSetChannelParametersFunction volatile kAiSetChannelParametersSymbol =
    &RK_MPI_AI_SetChnParam;
AiGetFrameFunction volatile kAiGetFrameSymbol = &RK_MPI_AI_GetFrame;
AiReleaseFrameFunction volatile kAiReleaseFrameSymbol = &RK_MPI_AI_ReleaseFrame;
AiChannelFunction volatile kAiDisableChannelSymbol = &RK_MPI_AI_DisableChn;
AiDeviceFunction volatile kAiDisableSymbol = &RK_MPI_AI_Disable;

AoSetPublicAttributesFunction volatile kAoSetPublicAttributesSymbol =
    &RK_MPI_AO_SetPubAttr;
AoDeviceFunction volatile kAoEnableSymbol = &RK_MPI_AO_Enable;
AoChannelFunction volatile kAoEnableChannelSymbol = &RK_MPI_AO_EnableChn;
AoSetChannelParametersFunction volatile kAoSetChannelParametersSymbol =
    &RK_MPI_AO_SetChnParams;
AoSendFrameFunction volatile kAoSendFrameSymbol = &RK_MPI_AO_SendFrame;
AoWaitEosFunction volatile kAoWaitEosSymbol = &RK_MPI_AO_WaitEos;
AoChannelFunction volatile kAoDisableChannelSymbol = &RK_MPI_AO_DisableChn;
AoDeviceFunction volatile kAoDisableSymbol = &RK_MPI_AO_Disable;

}  // namespace

int main() noexcept {
  const bool symbols_are_present =
      kSysInitSymbol != nullptr && kSysExitSymbol != nullptr &&
      kSysCreateMbSymbol != nullptr &&
      kMbHandleToVirtualAddressSymbol != nullptr &&
      kMbGetSizeSymbol != nullptr && kMbReleaseSymbol != nullptr &&
      kAiSetPublicAttributesSymbol != nullptr &&
      kAiEnableSymbol != nullptr && kAiEnableChannelSymbol != nullptr &&
      kAiSetChannelParametersSymbol != nullptr && kAiGetFrameSymbol != nullptr &&
      kAiReleaseFrameSymbol != nullptr &&
      kAiDisableChannelSymbol != nullptr && kAiDisableSymbol != nullptr &&
      kAoSetPublicAttributesSymbol != nullptr && kAoEnableSymbol != nullptr &&
      kAoEnableChannelSymbol != nullptr &&
      kAoSetChannelParametersSymbol != nullptr && kAoSendFrameSymbol != nullptr &&
      kAoWaitEosSymbol != nullptr && kAoDisableChannelSymbol != nullptr &&
      kAoDisableSymbol != nullptr;
  return symbols_are_present ? 0 : 1;
}
