# 客户端分关复现实验

每次补写一段真实行为，先看输入、顺序调用和输出，再看错误边界。服务端是只配置Key的配套程序，不作为板端课程前置知识。

这是同一份正式源码上的练习，不是九个已导出的独立阶段工程。尚未讲解部分保留参考实现；不要删空支持代码后期待完整CMake仍能配置。实验使用自己的工作副本，不修改测试来掩盖错误。

## 环境

推荐Linux/WSL。准备CMake、C++17、Python3.9+以及Host OpenSSL/Boost/cJSON、ALSA、FFmpeg开发依赖。Windows和WSL构建目录不能共用。

```sh
cmake -S . -B build/lesson-host -DCMAKE_BUILD_TYPE=Debug \
  -DBOOMPI_BUILD_UI_SIMULATOR=OFF -DBOOMPI_BUILD_TESTS=ON \
  -DBOOMPI_STRICT_WARNINGS=ON -DBOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON
python3 scripts/teaching_lab.py
python3 scripts/teaching_lab.py 1 --build-dir build/lesson-host
```

--dry-run只打印命令。所需测试未登记会明确失败，不把零测试当通过。所有实验只运行Host，不连接开发板或调用付费云端。

## 九个阅读/复现关卡

| 关卡 | 先读什么 | 本关检验 |
| --- | --- | --- |
| 1 配置 | voice_client_config.cpp：environment→decimal/ipv4→身份/pin→一次复制 | voice-client-config-contract |
| 2 固定帧 | audio_format.h、voice_codec.cpp：整数端序→PCM→控制消息 | protocol-json-contract |
| 3 播放 | playback.cpp：write→采样环→转换/写声卡→finish/drain | audio-flow格式、播放、采集 |
| 4 输入 | wake/vad→speech::reset/update→前滚及句尾 | detection、voice-preroll |
| 5 网络 | network::find_server→Connect→SendHello→OnMessage→send/end | voice-transport-loopback |
| 6 问答 | App_Init/Process/Close、receive_reply与cancel | voice-client-behavior |
| 7 插话 | 候选→hold→参考复核→START(supersede)→连续PCM | 应用、播放与WSS组合 |
| 8 页面 | page::open/show/camera→ui运行→显示触摸端口 | ui-pages、ui-runtime-lifecycle |
| 9 核心回归 | 把前述真实模块连接起来 | 核心Host测试，然后补全UI/网卡/摄像头检查 |

每关先运行参考实现，再在练习副本补写指定函数，最后说明一次正常输入与一次失败输入的结果。函数签名不是额外架构；不要新增Manager或课程条件编译。

## 必须讲清楚的几个界限

20ms对应16k单声道320samples，设备48k每通道960samples。PCM字节数与采样数不同，BPV4头部网络序，PCM固定小端；generation不是设备ID，sequence不能代替generation。

播放器保存连续采样，不再一包一槽。容量1.5秒，满时拒绝；DONE调用finish，只表示没有新PCM，转换器尾音和ALSA drain完成才是Drained。取消不能把旧尾音补进下一次播放。

普通提问300ms开口确认；播放/尾音上下文先120ms候选、暂停TTS消费、低参考/尾音等待和60ms复核。确认前不退休旧回答，失败恢复。三类语句共用500ms历史和上传路径，不代表必须共用相同准入条件。

WSS的open表示启动网络任务，READY才代表握手成功；send成功只表示本地入队。满队列不能跳PCM再发END。Host对端与网卡替身不证明真实Wi-Fi、云端或AEC效果。

UI只有语音、摄像头两个固定容器，切换不销毁重建。ui只交付一份快照和动作；运行期仅UI线程调用LVGL。初始化与join后的销毁是顺序交接，不需要另一套回执状态机。

摄像头只保留最新完整帧，短读必须拼完才交付；这一丢旧帧策略不能用于语音。进程组由摄像头worker负责回收，close请求退出并等待它完成。

## UI实验

教师提供LVGL8.2源码，安装SDL2/FreeType及Noto CJK：

```sh
cmake -S . -B build/lesson-ui -DCMAKE_BUILD_TYPE=Debug \
  -DBOOMPI_BUILD_UI_SIMULATOR=ON -DBOOMPI_STRICT_WARNINGS=ON \
  -DBOOMPI_LVGL_ROOT="$BOOMPI_LVGL_ROOT"
python3 scripts/teaching_lab.py 8 --build-dir build/lesson-ui
build/lesson-ui/client/boompi-ui-simulator --demo-voice
```

--demo-app 0/1分别选择语音、相机；--font指定可读CJK字体。模拟器运行真实页面，但不采集硬件相机。字体缺失、无效文件和缺字处理由专门回归验证。显示端口SPI/I²C、板型时序仍需真板验证。

## 合并检查与上板

```sh
python3 scripts/teaching_lab.py 9 --build-dir build/lesson-host
ctest --test-dir build/lesson-ui --output-on-failure
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
```

完整UI配置会额外生成页面、UI生命周期、摄像头和网卡检查。远程teaching-ui还运行独立netns和ASan/UBSan；每次查看当前提交，不引用历史测试数量。

最后用匹配RV1106 GCC/uClibc SDK交叉构建并检查ELF，再验证唤醒、句首、长短回复、真实插话、无人讲话时不自激、追问、断网恢复、页面触摸、音量保存和相机进退。先验2ca0cf6音频，再验非音频候选；Host绿色不是声学通过。

教学完成的标准是学生能对一个改变后的输入预测处理结果、指出负责模块和必要边界，而不是背出类名或只展示绿色测试。
