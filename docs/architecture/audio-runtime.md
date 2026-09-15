# 音频任务与数据所有权

主流程直接调用三个方向：audio_capture::read取得已处理帧；speech::update决定开口、句尾和插话；服务器回复直接写playback。语句模块没有播放入口、线程句柄或网络队列。

| 边界 | 所有者 | 保留原因与超限行为 |
| --- | --- | --- |
| 原始四槽、转换/3A工作区 | capture线程及各算法namespace | 硬件格式、256点vendor块和滤波历史，断点一起复位 |
| capture交接 | audio_capture，4×20ms | 跨线程交付；满时明确断点，不能拼成连续语音 |
| pre-roll/插话历史 | speech，同一个32帧环 | 普通保留25帧，探测最多32帧；未准入历史允许滚动覆盖 |
| speech结果 | 调用方临时Result | 只借用历史/当前帧指针，不复制PCM；下一update/listen/reset前消费 |
| 下行播放 | playback，75×20ms | 网络与ALSA消费解耦；满时取消整轮，不覆盖语音正文 |
| 网络交接 | voice_net | 有界发送与接收；代际隔离、背压和生命周期 |

## 正常输入

采集线程读取48kHz四槽数据，共同降采样到16kHz，再依次执行3A、Snowboy、VAD。断流丢弃不连续片段并明确报告。3A输出和metadata一起延迟，不能拿当前参考判断上一帧声音。

speech::listen只选择Wake/FollowUp准入；App在调用前通过capture帧边界复位检测。speech::update返回Start时，当前帧已在历史中；应用先START，再按原顺序发送借用PCM。实时阶段只借用当前帧，最后一帧发送成功后再END。遇到背压/断点使用CANCEL，不把残缺句子提交。

## 播放与打断

首AUDIO到达时playback::begin等待旧取消收尾，并在采集帧边界武装AEC，随后允许播放线程prepare。每个PCM包直接入有界队列；没有VoiceAudio/Engine/Backend多次校验和复制。

首播蓄水180ms，欠载宽限30ms后蓄水40ms。DONE调用playback::finish，短回答可立即放行。线程先排出有效滤波尾音再ALSA drain，最后发布Drained。主动cancel中断write/drain、丢弃队列，新一轮必须等旧操作收尾。

speech保留120ms候选、短暂静音、等待低参考60ms（最多300ms）、清尾音60ms、再次确认60ms的插话探测。Result的playback_scale由应用应用到播放模块；确认Barge后应用取消旧播放，发送新generation的START(supersede=true)，立即上传保留人声。用户音量与探测scale分别保存。

网络END结束上行，CANCEL在等待回复和播放期间仍有效。新协议没有下行音频END字段，DONE是唯一播放输入终点，故Drained一定在DONE之后，应用不再维护两份完成标志。失败终态与主动取消分开。

Host运行真实任务、转换、EOS和算法胶水；只替换ALSA设备调用与vendor核心。真板调度、回采位置、AEC和实际音色仍须由用户指定时间验收。
