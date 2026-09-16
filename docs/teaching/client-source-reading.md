# 最小教学版：从真实调用读起

当前非音频候选见[修改与验收](../test/minimal-non-audio.md)，音频保持2ca0cf6。

## 1. 启动、运行、退出

`client/apps/boompi_client/main.cpp`读取配置，调用App_Init，循环App_Process，统一App_Close。不要先从类图或线程框架开始。

## 2. 显示与触摸

先读ui/lvgl_screen.cpp：page::open一次创建语音/摄像头容器，show更新内容，camera切换容器，回调直接交付动作。再读device_ui.cpp：初始化端口/页面→启动UI线程→取快照/图像→tick/刷新→退出回收。应用运行时不接触LVGL指针。

最后读display_touch.cpp中的SPI/I²C、初始化表与旋转；它们本轮不变，作为硬件课单独讲解。

## 3. 联网

network.cpp的find_server展示有线与无线有序尝试。接口准备成功后使用固定端点或UDP发现/身份缓存。SSID/PSK直接编辑系统配置，没有AP/门户概念。voice_net.cpp负责真正WSS握手、心跳和轮次，接口绑定贯穿发现与连接。

## 4. 摄像头

camera_capture.cpp的worker创建固定管线，短读累积完整RGB565帧，发布最新帧，退出时统一终止进程组并回收。UI把最新帧复制到页面固定数组；预览允许只取最新帧，不能把这种策略用到语音PCM。

## 5. 音频与问答

沿用[音频数据流](audio-pipeline.md)与[紧凑插话](../test/compact-barge-in.md)。本次不改算法、时序、音频接口或Go；App仅适配UI函数，不重写问答状态机。

## 6. 验证

普通host-debug执行核心测试。完整UI需BOOMPI_BUILD_UI_SIMULATOR=ON及匹配LVGL_ROOT；ui-pages执行实际LVGL页面，ui-runtime-lifecycle执行实际UI线程/注册回收，camera-lifecycle执行真实管道与进程，只替换设备/外部命令。Host结果不能代替真板声学、显示、触摸或Wi-Fi验收。
