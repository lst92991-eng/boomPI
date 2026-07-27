# P0 Rockchip 3A 交叉链接验证记录（2026-07-27）

- 采集时间：2026-07-27 20:47:46 +08:00（Asia/Shanghai）。
- 结果：**交叉链接与动态符号级通过**。
- 范围：匹配 BSP 的 GCC 8.3/uClibc 工具链、固定 SHA-256 的
  `rkaudio_preprocess.h`、`libaec_bf_process.so` 与
  `librkaudio_common.so`，以及 tests-off 默认 ALL 的
  `boompi_rockchip_3a_link_check`。
- 证据边界：没有连接或运行开发板，没有执行该 ELF，没有打开 PCM、加载 3A、处理音频或
  安装运行资源。本记录不证明板端动态加载、capture slot、reference、AEC 效果、CPU/RSS
  或实时率。

## 配置与命令

私有 BSP 和工具链绝对路径不写入仓库。等价的脱敏配置与检查命令如下：

```text
cmake --preset rv1106-debug \
  -DBOOMPI_BUILD_TESTS=OFF \
  -DBOOMPI_ENABLE_ROCKCHIP_3A=ON \
  -DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON \
  -DBOOMPI_ROCKCHIP_3A_INCLUDE_DIR=<matching-bsp-3a-include> \
  -DBOOMPI_ROCKCHIP_3A_AEC_LIBRARY=<matching-bsp>/libaec_bf_process.so \
  -DBOOMPI_ROCKCHIP_3A_COMMON_LIBRARY=<matching-bsp>/librkaudio_common.so \
  -DBOOMPI_ROCKCHIP_3A_DETECT_LIBRARY=<matching-bsp>/librkaudio_detect.so \
  -DBOOMPI_ROCKCHIP_3A_CONFIG_FILE=<matching-bsp>/config_aivqe.json
cmake --build --preset rv1106-debug --parallel

<matching-cross-readelf> -h <build>/boompi_rockchip_3a_link_check
<matching-cross-readelf> -d <build>/boompi_rockchip_3a_link_check
<matching-cross-readelf> -Ws <build>/boompi_rockchip_3a_link_check
```

CMake 的 feasibility 闸门先核对目标为 Linux/ARM 交叉编译、编译器名称、uClibc sysroot、
Debug-only 配置，以及全部输入文件的固定哈希。哈希来源与 BSP 文件位置见
[P0 vendor 音频证据基线](p0-vendor-audio-inventory-20260727.md)。detect 库和 JSON 是当前
依赖闸门输入，但本 link-check 不调用 detect，也不解析 JSON；它只直接链接 AEC target，
并通过其接口依赖链接 common。

提交前回归同时通过 16 个 CTest、27 个 Python/script 测试，以及 Go 1.26.5 的
`go test ./...` 和 `go vet ./...`。其中 Linux 合成库用例会分别移除三个直接入口，确认缺少
任一符号时默认 ALL 链接失败；该用例不被当作 vendor ABI 或板端证据。

## 链接与 ELF 结果

- 在 `BOOMPI_BUILD_TESTS=OFF` 下执行默认 ALL 构建成功，并生成
  `boompi_rockchip_3a_link_check`；该 target 不依赖 CTest。
- 最终产物为匹配目标的 32-bit ARM EABI5 hard-float/uClibc ELF。
- 动态段保留 `libaec_bf_process.so` 和 `librkaudio_common.so` 两个 `NEEDED`。这证明
  `--as-needed` 没有把未使用的 vendor shared object 丢弃。
- 动态符号表保留以下三个 `UND`，并在交叉链接时由匹配的 AEC 库完成符号解析：

```text
rkaudio_preprocess_init
rkaudio_preprocess_short
rkaudio_preprocess_destory
```

- 三个函数指针类型直接由匹配 header 编译检查：
  `void *(*)(int,int,int,int,RKAUDIOParam *)`、
  `int (*)(void *,short *,short *,int,int *)` 和 `void (*)(void *)`。
- target 设置 `SKIP_BUILD_RPATH=TRUE`，最终动态段不携带指向私有 BSP 目录的
  `RPATH`/`RUNPATH`。vendor include 目录是该 target 的 `PRIVATE` 输入，不进入公共接口。
- target 没有 `add_test`、自定义执行命令或 `install(TARGETS ...)`；默认构建只编译并链接，
  不会运行或安装它。
- feasibility 闸门限定为 Debug，因此原始 ELF 的 DWARF 会保留临时 source/build 路径；通用的
  “可部署 ELF”检查会据此拒绝原始 Debug 文件。对临时副本执行 `--strip-debug` 后，完整 RV1106
  ABI、loader、无开发路径和无 RPATH/RUNPATH 检查通过。仓库不会安装或发布这个 link-check，
  也不把“PRIVATE include”误写成“Debug 信息中绝无路径”。

## 本次没有证明

- 没有证明目标镜像已经安装正确版本的 AEC/common 库，或板端 loader 能成功加载它们。
- 没有调用 `rkaudio_preprocess_init`，也没有验证 `RKAUDIOParam`、JSON、模型或释放顺序。
- 没有验证 48 kHz capture/playback、物理 slot 到
  `[mic0,mic1,ref]` 的映射、reference tap、极性或延迟。
- 没有验证 16 kHz 固定帧处理、512-byte 返回、错误恢复、AEC/NS/BF/AGC 效果、CPU、RSS、
  单帧最坏耗时或持续实时率。

因此这里只关闭“匹配 header 与 shared objects 能由 RV1106 工具链真实解析三个入口”这一项。
P0 总状态仍为部分通过，下一步仍是当前板/镜像的只读资源盘点、raw PCM 全双工与通道相关性
HIL，之后才能运行最小 3A 固定帧探针。
