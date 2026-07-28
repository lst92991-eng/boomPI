# P0 Rockchip MPI 音频 HIL 构建验证记录（2026-07-28）

- 采集时间：2026-07-28 14:38:40 +08:00（Asia/Shanghai，以 host 时钟为准）。
- 结果：**离线回归、GCC 8.3/uClibc 严格交叉构建和 ELF 审计通过；板端 HIL 未运行**。
- 板端状态：`rv1106-board` SSH 连接超时，因此没有部署或执行 ARM ELF。
- 证据边界：没有打开 PCM、调用板端 vendor API、修改 mixer、采集/保存录音或播放声音；也没有
  采集板端 dmesg delta。本记录只关闭构建、链接、默认不执行和静态产物边界。

## 最终输入与 provenance

验证使用最终 `src3` clean 临时源码树、匹配 BSP 的 GCC 8.3/uClibc 工具链，以及 CMake 已逐项
核对 SHA-256 的八个 MPI/类型头文件和 Rockit/MPP/RGA 库。私有 BSP、工具链、sysroot 和构建目录
的绝对路径均未写入仓库；下面记录的命令只保留等价的脱敏形式。

| 对象 | SHA-256 |
| --- | --- |
| `client/tests/hil/rockchip_mpi_audio_hil.cpp` | `ba36ceb083e7e8d21912b3af4dcc37dc307fa84689c03a9a2fe2ccf559fdbbb8` |
| `boompi_rockchip_mpi_audio_hil` ELF | `6cc32a936cfdd740fdfa5c605afb5781590b0af063c10befe453d2e3dbab5205` |
| `boompi_rockchip_mpi_audio_link_check` ELF | `f32d06dbf194469c2602c23cc5e5b485e57d6ac6f1ac5da70601aa1e2fb5aa7d` |
| HIL canonical pinset manifest | `2b0257693416e2d97023a6c45c6302727a0fa3a007e554bb9ee053bd35166e01` |

pinset hash 是 CMake 对固定文件名和期望哈希组成的 canonical manifest 求得的 provenance，不含
开发机绝对路径；source hash 与 pinset hash 都编译进 HIL 报告。ELF hash 对应本记录所述最终
clean build，源码或链接输入变化后不得沿用。

## 脱敏验证命令

```text
cmake --preset rv1106-debug \
  -DBOOMPI_BUILD_TESTS=OFF \
  -DBOOMPI_STRICT_WARNINGS=ON \
  -DBOOMPI_ENABLE_ALSA_PLAYBACK=OFF \
  -DBOOMPI_ENABLE_ROCKCHIP_MPI_AUDIO=ON \
  -DBOOMPI_BUILD_ROCKCHIP_MPI_AUDIO_HIL=ON \
  -DBOOMPI_ALLOW_FEASIBILITY_AUDIO_VENDOR_INPUTS=ON \
  -DBOOMPI_ROCKCHIP_MPI_INCLUDE_DIR=<matching-bsp>/media/rockit/rockit/mpi/sdk/include \
  -DBOOMPI_ROCKCHIP_MPI_ROCKIT_LIBRARY=<matching-bsp>/media/out/lib/librockit.so \
  -DBOOMPI_ROCKCHIP_MPI_MPP_LIBRARY=<matching-bsp>/media/out/lib/librockchip_mpp.so.0 \
  -DBOOMPI_ROCKCHIP_MPI_RGA_LIBRARY=<matching-bsp>/media/out/lib/librga.so

cmake --build <clean-rv1106-build> --parallel
# 确认默认 ALL 没有生成 boompi_rockchip_mpi_audio_hil
cmake --build <clean-rv1106-build> --target boompi_rockchip_mpi_audio_hil --parallel

<matching-cross-readelf> -h <build>/boompi_rockchip_mpi_audio_hil
<matching-cross-readelf> -l <build>/boompi_rockchip_mpi_audio_hil
<matching-cross-readelf> -d <build>/boompi_rockchip_mpi_audio_hil
<matching-cross-readelf> -Ws <build>/boompi_rockchip_mpi_audio_hil
<matching-cross-readelf> -Ws <build>/boompi_rockchip_mpi_audio_link_check
sha256sum <build>/boompi_rockchip_mpi_audio_hil \
  <build>/boompi_rockchip_mpi_audio_link_check
```

本轮显式关闭与 raw MPI 探针无关的 ALSA playback adapter，使该交叉链接证据只依赖 pinned
Rockchip MPI 闭包；这不代表板端 ALSA 不可用，也不改变既有 ALSA 独立验证记录。

测试命令等价于：

```text
ctest --test-dir <clean-host-build> --output-on-failure
python3 scripts/tests/test_rockchip_mpi_cmake.py -v
python3 scripts/tests/test_audio_vendor_cmake.py -v
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v

cd server
go test ./...
go vet ./...
go build -trimpath ./cmd/boompi-server
```

## 回归与构建结果

- Linux Rockchip MPI 专用回归：10/10 通过。
- Linux vendor 音频 CMake 回归：12/12 通过。
- 全部 Python/script 回归：53/53 通过。
- Host CTest：16/16 通过。
- Go 1.26.5：`go test ./...`、`go vet ./...` 和
  `go build -trimpath ./cmd/boompi-server` 全部通过。
- GCC 8.3/uClibc 的 strict clean 配置和构建无 warning/error。
- HIL target 保持 `EXCLUDE_FROM_ALL`：默认 ALL 成功但不生成 HIL；随后显式指定
  `boompi_rockchip_mpi_audio_hil` target 构建成功。没有 install、CTest 或 post-build 自动运行。

这些 Python fixture 包含 host 合成 header/library 和不执行 sentinel，用于关闭 CMake 开关、
默认 target、dry-run 与错误失败边界；它们不是 RV1106 ABI 或硬件行为证据。

## ELF 与动态链接结果

HIL 与扩展后的 link-check 都是 `ELF32`、ARM `EXEC`、EABI5、hard-float，程序解释器为
`/lib/ld-uClibc.so.0`。两者的动态段均无 `RPATH`/`RUNPATH`，字符串审计未发现 pinned MPI
include 目录。Debug DWARF 仍包含交叉编译器、sysroot 和临时源码树的常规绝对路径，因此这些
未 strip 的验证 ELF 只保留在临时构建目录，不得直接作为发布产物。最终 `NEEDED` 集合为：

```text
librockit.so
librockchip_mpp.so.1
librga.so
libstdc++.so.6
libgcc_s.so.1
libc.so.0
```

HIL 和 link-check 的动态未定义 raw MPI 符号各为 22 个；两者均包含新增的
`RK_MPI_MB_GetSize`，完整集合是：

```text
RK_MPI_SYS_Init
RK_MPI_SYS_Exit
RK_MPI_SYS_CreateMB
RK_MPI_MB_Handle2VirAddr
RK_MPI_MB_GetSize
RK_MPI_MB_ReleaseMB

RK_MPI_AI_SetPubAttr
RK_MPI_AI_Enable
RK_MPI_AI_SetChnParam
RK_MPI_AI_EnableChn
RK_MPI_AI_GetFrame
RK_MPI_AI_ReleaseFrame
RK_MPI_AI_DisableChn
RK_MPI_AI_Disable

RK_MPI_AO_SetPubAttr
RK_MPI_AO_Enable
RK_MPI_AO_SetChnParams
RK_MPI_AO_EnableChn
RK_MPI_AO_SendFrame
RK_MPI_AO_WaitEos
RK_MPI_AO_DisableChn
RK_MPI_AO_Disable
```

这证明最终两个 ELF 都保留了同一组 raw AI/AO 生命周期动态引用，而不是被编译器或
`--as-needed` 化成空检查；它不证明这些符号能在当前板端镜像中成功执行。

## 本次明确没有证明

- 未运行任何 ARM ELF，未打开 capture/playback PCM，也未修改 mixer。
- 未证明真实声音、可听扬声器输出、hardware presentation 或无 pop。
- 未证明 `AUDIO_BIT_WIDTH_16` 的精确 S16_LE 字节序、`u32Len`/MB packing、padding、交织或
  hardware period。
- 未证明两个 slot 都是麦克风，也未证明数字 reference、极性、时钟漂移或通道相关性。
- 未取得板端连续 dmesg 前后快照，因此没有证明无新增 xrun/underrun/overrun 或驱动错误。

恢复 `rv1106-board` SSH 后，仍须由外层停止/锁定相关音频服务，对探针 PID 设置约 20 秒 watchdog，
人工确认功放安全，并采集连续 dmesg delta，才可以开始登记真实板端 HIL。即使本地 probe 的
transport/eos/cleanup facet 通过，也不能单独替代这些外层证据。
