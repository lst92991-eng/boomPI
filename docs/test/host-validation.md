# 教学版 v2 验证与上板清单

## 版本配套

本次客户端与服务端只有v2协议，必须成套使用。旧版服务端不能与新客户端互通。源码基线已保存于本地 `build/teaching-baseline-20260905`，原有 `v1.0.0` tag 保持不变。

服务端产物：`build/teaching-v2-release/boompi-server.exe`。Windows上可以双击，首次配置Key；原有config.yaml和state应保留在用户可访问的私有目录。不要把Key或TLS私钥打包发出。

## 本机验证

```sh
cmake --preset host-debug -DBOOMPI_REQUIRE_HOST_TRANSPORT_TEST=ON
cmake --build --preset host-debug --parallel
ctest --preset host-debug
python3 scripts/verify_protocol_fixtures.py
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
cd server
go test ./...
go vet ./...
go test -race ./...
```

Host真实传输测试需要OpenSSL、Boost1.83和cJSON。Windows/macOS缺依赖时默认会跳过网络测试；发布验收应在Linux强制启用。C++应用测试直接执行生产VoiceApp，用可控时钟和假I/O验证六状态；VoiceAudio测试直接执行生产语义判定和音频线程，只替换硬件输入输出。

C++与Go联测可在server目录运行：

```sh
BOOMPI_CPP_WSS_SMOKE=/absolute/path/to/boompi_voice_transport_loopback_test \
  go test ./internal/app -run TestCppWSSHappyPath -v
```

该测试使用假Qwen响应，不调用付费API，验证真实TLS、错误pin拒绝、hello、PCM、文本和done。

## RV1106 构建

```sh
export BOOMPI_RV1106_SDK_ROOT=/absolute/path/to/teacher-sdk
sh scripts/build_teaching_release.sh
```

脚本在匹配GCC/uClibc工具链编译后，检查ARM EABI5 hard-float、loader、GLIBCXX上限和RPATH；成功才生成 `build/teaching-v2-release/boompi-client` 与 `rootfs/`。它不连接或修改开发板。缺少SDK时必须先补齐，不能拿旧build目录的ELF冒充新版本。

## 接板前准备

1. 保存板端旧客户端、服务端EXE、配置和TLS身份，以便整套回退。
2. 确认新客户端确实经过上述交叉构建和ELF检查。
3. 将新客户端及当前client/scripts里的控制/配网脚本成套安装；保留模型、字体和板级库。
4. 确认v2服务端已启动，客户端 `--check-config` 通过，再手工启动客户端。
5. 日志应出现 `state=offline → idle → listening → uploading → waiting → speaking`；声学参数由board_voice_profile.h决定，旧env声学键仅报迁移提示。

## 真人验收

使用同一板子、镜像、模型、位置与默认60%音量，比对基线与新版本；不凭Host通过推断声学效果。

| 场景 | 通过条件 |
| --- | --- |
| 唤醒后正常问答 | 一次发言只创建一个generation；句首和尾字完整 |
| 长回答 | 连续播放，无周期性静音、未解释的断帧或溢出 |
| 播放中短句打断 | 旧声音停止，新问题立即开始上传且回答正确，无需再说一遍 |
| 长句打断 | 旧字幕/声音/done不混入新回答 |
| 尾播阶段触屏停止 | 本地声音停止，服务端撤回未听完回答；后续提问可用 |
| 三秒追问 | 无需唤醒可继续；窗口结束回Idle |
| 追问先“嗯”再提问 | 较短前一句不会截断真正问题的句首 |
| 安静播放 | 不自激、不产生伪Barge或新generation |
| 音量0、低音量、默认音量 | 音量变化不锁死语音入口；恢复音量后无回声误打断 |
| 网络断开/恢复 | 残缺输入不提交、不重放；自动回到Idle |
| UI与摄像头 | 四个已实现入口可用，触摸音量正常，离开摄像头页释放进程 |
| 启动/停止 | 无残留客户端/配网/摄像头子进程，关闭有界 |

记录capture discontinuity、XRUN、queue overflow、generation变化、CPU/RSS、实际SCHED_FIFO优先级和主观体验。声学profile一次只改一组可解释的量，始终用同一验收流程回归。

可选HIL复用生产VoiceAudio；它是固定条件下的链路/事件探针，不能代替真人双讲和最终壳体声学验收。
