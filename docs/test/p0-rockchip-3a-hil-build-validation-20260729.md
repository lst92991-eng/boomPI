# P0 Rockchip 3A 固定帧 HIL 构建验证（2026-07-29）

- 最终复验时间：2026-07-29 16:23:47 +08:00（Asia/Shanghai）。
- 结论：**离线 fake 生命周期与匹配 RV1106 交叉构建通过；真实板端执行未进行。**
- 范围：显式 opt-in 的 direct Rockchip 3A 单帧内存探针、CMake 安全闸门、Linux fake
  shared object、GCC 8.3/uClibc 目标链接和 ELF 检查。
- 排除项：没有把 ELF 复制或运行到开发板，没有打开 ALSA/MPI、采集 PCM、调用真实 3A
  API、修改 mixer/设备树/启动项，也没有执行付费 provider 请求。

## 固定输入与来源

匹配 SDK 输入继续使用现有 feasibility pins：

| 逻辑输入 | SHA-256 |
| --- | --- |
| `rkaudio_preprocess.h` | `b9bbf723d8e5bfdc421cf45fdf5853fec1584737d0e20682cd0db6bae5a7b54d` |
| `libaec_bf_process.so` | `5abbcf518ffa39900dd78352547ebf5feab83d2f9b30a82c1e2dc1dc44b25e07` |
| `librkaudio_common.so` | `4f4c9d78028a592174c3e959e35d231317d0ed2a864a1ed230f8adab42960246` |
| `librkaudio_detect.so` | `f84b66a2d1d561fbb3c36e288a57f1e9ef50990974d9be9accbb0aaebcbae396` |
| `config_aivqe.json` | `1d160fde184935cf43a49feae7be0dfd24efdc82ff9de2ea8b35aba6318074f9` |

本轮探针源码 SHA-256 为
`fec4248ba8f1b8c7eeb71540cd92f5d09955d03825a6385daa03165033a90710`，离线测试源码为
`dfcef5946ecf7d1d44cb93498a31cc2d2e016a835c025457159fe33c0d6501e7`，CMake 模块为
`5b3e65acf9f0728d2cc29ca354df6aa9f0627558318194cf2c223efe352191a6`。稳定逻辑名 pinset
SHA-256 为
`08c765b4958785597325cfc0e089bbb0fc3a89eccae60cfab80eaeb300d1ceac`。探针源码哈希与
pinset 哈希均由 CMake 写入 ELF 的 dry-run/execute provenance；绝对 SDK 路径不进入该字段。

direct API 不解析 `config_aivqe.json`。源码先调用匹配 header 的参数 helper 分配嵌套结构，
再把 pinned config 中启用的 AEC、FastAEC、dereverb、AES、AGC、ANR 与 howling profile
固定写入内存；运行时不读取 JSON，也不引入配置解析器。

Luckfox BSP 的 target rootfs staging 中存在
`output/out/oem/usr/lib/libaec_bf_process.so`；生产 HIL 因此只从目标绝对路径
`/oem/usr/lib/libaec_bf_process.so` 加载。该路径证据不等于运行时字节身份，板端 execute 前仍须
按指南独立核对 AEC/common/detect 三个文件哈希。

## 离线 fake 结果

Ubuntu 上执行：

```text
python3 -B scripts/tests/test_rockchip_3a_hil.py -v
```

最终结果为 `6/6` 通过，用时 `1.369 s`。覆盖事实包括：

- 默认 `OFF` 时不访问不存在的 vendor 路径，也不创建 link-check/HIL target；
- HIL 开启但 3A 依赖关闭、native host、伪造目标布尔值和直接调用私有 constructor 均在
  私有路径访问前失败；
- HIL 是 `EXCLUDE_FROM_ALL`，不由默认 build、install 或 CTest 生成/执行；
- 带 constructor/destructor 哨兵的 fake 证明正确清理 loader override 后的无参数 dry-run 不会
  主动加载 vendor；六个调试路径变量
  均在主动 `dlopen` 前以退出码 `3` 拒绝；三个 loader override 变量也会被残留检查拒绝，且
  真实 `LD_PRELOAD` 反例证明 constructor 会在 `main()`/退出码 `3` 前运行，不能把进程内检查
  写成 pre-exec 防线；
- 缺少固定库时以退出码 `4` 失败且不加载；缺少第二个待解析符号时记录 load 后立即 unload，
  不调用任何 helper；
- fake `.so` 只导出真实 link-check 已证明的 init/process/destory 三个动态入口，不额外导出
  pinned header 内的 `static inline` 参数 helper；
- execute 精确调用 load、header 参数 helper、`init(16000,16,2,1)`、单帧
  `process(...,768,...)`，fake 返回 `512 bytes` 后依次调用拼写如此的 `destory`、parameter
  deinit 与 unload；dereverb/AES/ANR/AGC/howling profile 逐字段核对；
- init-null 只释放 parameter tree，不调用 process/destroy；process 返回 `511` 时仍先
  destroy handle 再释放 parameter tree，并以退出码 `5` 失败；
- 未执行 process 的分支明确报告 guard 未评估；已执行分支检查输入/输出 guard，fake ELF 无
  vendor `NEEDED`、RPATH/RUNPATH 与私有路径。

fake 只证明调用方契约、顺序与错误分支，不能证明真实算法或目标 ABI。

同一 clean 临时树随后执行全部 `scripts/tests/test_*.py`，最终结果为 `69/69` 通过，用时
`17.457 s`；这证明新增专项没有破坏已有离线脚本回归，但仍不代表真实板端 3A 已通过。

## 匹配 RV1106 交叉构建

使用 Luckfox BSP 自带的
`arm-rockchip830-linux-uclibcgnueabihf-g++ 8.3.0`、显式 uClibc sysroot、Debug-only
feasibility opt-in 和 `BOOMPI_STRICT_WARNINGS=ON` 配置，然后只构建：

```text
cmake --build --preset rv1106-debug \
  --target boompi_rockchip_3a_hil --parallel 2
```

构建成功；最终 ELF SHA-256 为
`f5946150979c587de660445d12ddc7d8c2f80861fe2a7ae3f446092087e368c1`。检查结果：

- ELF header 为 32-bit ARM，flags 为 `0x5000400, Version5 EABI, hard-float ABI`，
  interpreter 为 `/lib/ld-uClibc.so.0`；
- `NEEDED` 只有目标 libstdc++/libgcc/libc，不包含 vendor 库；`dlopen`/`dlsym`/`dlclose` 为
  动态 `UND`，固定 `/oem/usr/lib/libaec_bf_process.so` 与三个已验证动态 API 名称作为字符串
  进入 ELF；参数 helper 由 header 内联，不是待解析的 vendor 动态符号；
- 已有 direct link-check 继续单独保留 `libaec_bf_process.so`、`librkaudio_common.so` 的
  `NEEDED` 以及 `rkaudio_preprocess_init`、`rkaudio_preprocess_short`、
  `rkaudio_preprocess_destory` 三个 `UND`，HIL 不重复承担该证据；
- 动态段没有 `RPATH`/`RUNPATH`；strings 未发现匹配 BSP 的私有 `media` 或 fake/test 路径；
- 产物是未 strip 的 Debug probe，不安装、不发布，也未在板端运行。

## 仍未关闭的门

当前板仍运行旧 `RV1106-Atguigu` 镜像。虽然 direct ALSA 临时切到 `DiffadcLR` 后两个 PCM
slot 均有动态，但物理 U9/U12 到 slot、极性、唯一 playback reference、延迟和时钟关系仍未
通过正确镜像与受控刺激关闭。因此本轮不能勾选真实 3A 加载、一次真实 process、AEC/BF/ANR/
AGC 效果、错误恢复、16 ms deadline、连续实时率或 CPU/RSS。

下一步必须先从最新 `netlist.json` 生成可审计 BSP manifest，取得当次明确烧录授权并验收目标
镜像，再按 [固定帧 HIL 指南](p0-rockchip-3a-hil-guide.md)先 dry-run、后在外层 10 秒 watchdog
下执行单帧。非法尺寸恢复、375 帧持续实时率和声学效果应使用独立 handle/场景，不能塞进本轮
最小探针或冒充压力测试通过。
