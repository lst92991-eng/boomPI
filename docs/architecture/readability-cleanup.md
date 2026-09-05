# 客户端可读性整理

本轮从已提交的 `f35ed82` 继续整理，目标是模块职责和阅读顺序清楚、语法常见、注释准确。v2协议、声学参数、缓冲容量、超时和功能保持原值。

## 改动

- 产品网络代码不再混合Host替身。CMake为板端选择真实网卡实现，为回归选择tests/support/network_setup.cpp。
- Linux音频线程调度移到platform/rv1106/audio_thread.cpp，Host回归使用普通线程替身。音频算法内不再出现平台宏。
- 删除无调用的诊断平面、STDT开关和AEC delay宏覆盖。固定帧格式与板级标定分开，公开VoiceAudio接口不暴露硬件路径和标定参数。
- 主应用按语音上传、回答播放、音频故障分出小函数；状态与UI的映射用显式switch。首帧、末帧、替换旧回答使用具名局部变量。
- AudioBackend正常采集按极性修正、重采样、3A、判定与发布读取；播放就绪条件改成顺序判断。
- 摄像头进程、采集线程、帧超时与最新帧交接移到私有CameraCapture；DeviceUi保留显示、触摸和UI线程。
- 删除无人消费的SpeechEnd事件，仍由最后一个Pcm.end结束输入；删除仅用于两处邮箱读取的小模板。
- 用.clang-format统一一行一个语句和带大括号的条件/循环；注释说明线程归属、硬件时序、单位和失败后果。

## 同口径统计

统计范围为client/src、client/include、client/apps/boompi_client，包含私有驱动；不含资源数组、第三方、测试和构建文件。预处理指标不计入CMake的平台选源判断。

| 指标 | f35ed82 | 本轮 |
| --- | ---: | ---: |
| 生产C/C++文件 | 28 | 34 |
| 物理行 | 5717 | 6975 |
| ELOC（非空非注释行） | 4563 | 5671 |
| 条件编译组 | 11 | 0 |
| 非for头部的一行多语句 | 89 | 0 |

本轮行数增加主要来自展开压缩语句、补齐大括号和明确校验步骤。ELOC包含单独的大括号行，因此不能用它单独判定本轮是否更容易读。没有通过删除保护逻辑或调整统计范围降低数字。

新增的生产文件对应平台调度、帧格式、网络设置声明和摄像头生命周期，应用仍只依赖VoiceAudio、VoiceLink和DeviceUi。未新增manager、接口层、消息总线或线程。

复现：

    python scripts/measure_client.py --revision f35ed82
    python scripts/measure_client.py

## 验证

- GCC 13.3严格构建，Linux CTest 22/22；Windows MSVC严格构建，两测试组通过。
- 真TLS loopback、共享fixture和实际C++↔Go WSS联测通过；服务端源码未修改。
- Python 11/11和v2 fixture验证通过。
- 真实Linux网卡源、音频调度源、ALSA源通过Host严格语法检查；HIL通过Host编译及--help。
- DeviceUi与CameraCapture严格编译通过；SDL小智、摄像头、WiFi页已检查，小智/WiFi预览与上一版相同。
- clang-format检查、git diff --check通过。
- 独立审查确认主应用提取和摄像头拆分没有改变既有控制顺序与时间参数。

RV1106匹配SDK仍未配置，因此本轮没有新的板端ELF或硬件验收结果。下一步按 [验收清单](../test/host-validation.md) 交叉编译并在板端验证。
