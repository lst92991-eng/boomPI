/** @file rkaudio_preprocess.h
 * @brief FIFO 单元测试使用的最小声明；不代表厂商结构布局，不验证真实 ABI 或 AEC。
 *
 * 由 tests 的 include 优先级替换 vendor 头；audio_modules_test.cpp 提供函数实现。
 * 成员只覆盖rockchip_3a.cpp访问的参数，结构名不能用于推断真实库的内存布局。
 */
#pragma once

enum {
  RKAUDIO_EN_AEC = 1,
  RKAUDIO_EN_BF = 2,
  EN_Fastaec = 1,
  EN_AES = 4,
  EN_Anr = 16,
  EN_Dereverberation = 64,
  EN_STDT = 1024
};
/// 模拟生产初始化时写入的顶层参数关系；指针指向测试进程内的静态参数对象。
struct RKAUDIOParam {
  int model_en, read_size;
  void* aec_param;
  void* bf_param;
  void* rx_param;
};
struct SKVAECParameter {
  int pos, model_aec_en, drop_ref_channel, delay_len;
  void* delay_para;
};
struct SKVPreprocessParam {
  int model_bf_en, Targ, ref_pos, num_ref_channel, drop_ref_channel;
  void* dereverb_para;
  void* aes_para;
  void* anr_para;
  void* dtd_para;
};
struct RKDTDParam {
  float ksiThd_high, ksiThd_low;
};
struct RKAudioDereverbParam {
  int curveLg;
  float T60;
};
struct RKAudioAESParameter {
  float Beta_Up_Low;
  int THD_Flag, HARD_Flag;
};
struct SKVANRParam {
  int swU, InterV;
  float fGmin;
};
// 窄 C 接口仅供生产 DSP 包装层链接；处理替身输出确定的三路线性组合而非消回声结果。
extern "C" {
void* rkaudio_aec_param_init();
void* rkaudio_preprocess_param_init();
void rkaudio_param_deinit(RKAUDIOParam*);
void* rkaudio_preprocess_init(int, int, int, int, RKAUDIOParam*);
int rkaudio_preprocess_short(void*, short*, short*, int, int*);
void rkaudio_preprocess_destory(void*);
}
