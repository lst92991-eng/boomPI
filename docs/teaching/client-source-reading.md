# 顺着真实数据流阅读客户端

从`client/apps/boompi_client/main.cpp`开始。配置校验成功后，程序依次执行`App_Init → App_Process循环 → App_Close`；初始化失败与正常退出共用回收路径。板端自有C/C++包括入口、src和include，第三方算法只读调用边界。

## 1. 先读初始化和一轮对话

打开`client/src/application/voice_client.cpp`：

- 初始化：读取音量 → UI → 输入模块 → 播放模块 → 输入线程 → 网络线程。两路声卡配置好后才首次采集。
- 每轮循环：收取回复 → 读取采集结果 → 检查尾播/超时/断流 → 整理语句 → 交付网络 → 处理触摸动作。
- 退出：停止网络 → 停止播放 → 停止输入 → 关闭UI；模块内部先结束线程，再释放它独占的资源。

应用只有Idle、Listening、WaitingReply、Speaking四态。Listening既包含等人开口，也包含上传已经确认的语句；是否已START由speech结果和voice_net的上传阶段表达。相同的聆听画面不代表没有上传边界。

## 2. 顺着一块麦克风PCM走

`client/src/audio/voice_input.cpp`中的`CaptureTask`按以下顺序执行：

```text
audio_capture::read     原始48k四通道；短读和断点在声卡边界处理
audio_convert::capture 双麦与refL一起转为16k，保持通道对齐
rockchip_3a::process    厂商256点块适配为320点单声道
wake::detect           唤醒词命中，错误与未命中区分
vad::process           逐帧人声判断，错误与静音区分
有界帧交接             应用来不及取帧时报告断点
```

应用调用`speech::update`，从同一个500ms历史环交付句首与当前帧，然后依次START、PCM、END。插话候选只请求暂停消费播放；复核成功才退休旧回复，失败恢复。详细边界见[紧凑插话](../test/compact-barge-in.md)。算法配置、参考通道与时序保持已验收基线。

## 3. 网络运输与扬声器消费

先看`network.cpp::find_server`：有线/无线准备 → 固定端点或UDP发现/身份缓存。重新发现的端点没变就不重写缓存。Wi-Fi凭据使用板端系统配置，不进入业务协议。

再看`voice_net.cpp`：网络线程建链、校验TLS身份、完成HELLO/READY，再传输业务帧。它拥有generation、sequence、上传阶段和连接状态，应用不再分配另一套轮次号。格式解析在`voice_codec.cpp`，不接触声卡或云端模型。

回复经过`voice_net::poll → playback::write → 重采样/音量 → ALSA`。网络DONE只关闭播放输入；声卡Drained之后应用才开放追问。必要的播放环吸收到包节奏差异，满队列不能覆盖语音正文。

## 4. UI显示与动作反向返回

先看`ui/lvgl_screen.cpp`：一次创建语音/摄像头两页；状态、字幕和音量各自显示，只有变化的控件更新。

```text
应用UiView → ui::show → 短锁交接 → UI线程 → page::show → LVGL刷新
触摸控件 → 页面Event → UI动作交接 → App_Process → 原有业务动作
```

待机点“开始对话”或表情产生Wake；思考/回答阶段点“停止”或表情产生Interrupt。聆听和离线时按钮禁用，不发送应用无法消费的动作。音量拖动立即更新数字，释放时保存；旧快照不能把正在拖动的滑块拉回去。

`device_ui.cpp`负责LVGL端口、线程及短锁交接。页面在启动线程前创建，join后释放。`display_touch.cpp`最后阅读：SPI旋转刷屏、GT911读取/确认/恢复都属于硬件边界，业务代码不重复这些规则。

## 5. 摄像头独立交付完整图像

`camera_capture.cpp`的线程创建固定采集管线，累积短读直到完整RGB565帧，再发布最新帧。UI复制到页面稳定像素数组，离开页面时结束管线并回收子进程组。捕获中、最新完成、正在显示的数组各有用途；预览允许只取最新帧，音频不能照搬这一丢帧策略。

## 6. 用所有权理解剩余状态

| 所有者 | 保存的事实 |
| --- | --- |
| App | 对话四态、追问/回复期限、字幕累积 |
| 输入线程及算法模块 | 声卡、转换器、3A/wake/VAD句柄与连续性 |
| speech | 句首历史、人声/静音计数、插话复核 |
| voice_net | 连接、协议轮次/序号、在途旧结果隔离 |
| playback | 播放样本、取消屏障、暂停消费、实际尾播 |
| UI线程 | LVGL对象、页面显示和触摸；快照不是第二个业务状态机 |

## 7. 阅读验证边界

Host场景验证真实主流程的交付和资源边界；ui-pages用实际LVGL/FreeType渲染并检查控件动作，ui-runtime-lifecycle检查UI线程退出。摄像头和网卡测试中的外部I/O有替身，不能代替真板。

客户端课程只学习连接配套服务端所必需的BPV4输入输出与失败响应。服务端仍作为教师维护的黑箱，不要求学生学习Go或云端ASR/LLM/TTS编排。
