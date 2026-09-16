# 非音频最小教学版：第一轮交付

## 版本与范围

用户授权“改好后提交”。基线为`2ca0cf63d241a032292f4d83a3120002b91a066e`，候选分支为`codex/minimal-non-audio`；原音频验收分支`codex/p1-fast-vertical-integration`不移动。

此次重写非音频实现。音频源码/头文件/profile、Go服务端、BPV4协议、第三方源码和资源均不改；CI对这些目录做固定基线diff。App只更换UI调用接口，原语音状态与时序不重写。

## 实际删减与取舍

UI由DeviceUi/LvglScreen/PImpl多层转发改为ui/page函数模块。两页一次创建，切换只隐藏容器；一次show更新状态、字幕与音量。保留中文缺字过滤、静态表情、触摸及音量持久化。

删除桌面四宫格、独立时钟页、二维码/AP/门户配网、配网子进程及启动交接。保留语音与摄像头两页，摄像头不删除。上述是用户确认的产品范围收敛，不宣称功能完全等价。

UI资源在启动工作线程前顺序初始化，退出join后释放；不再启动线程后用start_done/start_ok回执等待。摄像头从4级图像存储收敛到采集工作数组、共享最新帧和页面稳定像素数组，去掉UI中间的115200字节副本；不是整个进程RSS下降量。

摄像头工作线程独占管道与进程组回收；read累积完整帧，最多等待2秒首帧/1秒后续帧，50ms轮询响应停止。固定V4L2/FFmpeg格式未变，原始管线仍由外部工具实现。此项测试不表示真实SC3336已经运行。

network.h/.cpp直接负责网卡准备、发现/缓存及接口绑定。默认有线→无线；有线有IP但连接未READY时下轮先试无线，发现和WSS均绑定所选接口。WSS连接前在单一IPv4 socket上绑定后发起连接，后续TLS/HTTP/WebSocket继续使用原库流程。协议校验、pin和轮次隔离未删除。

Wi-Fi直接编辑/etc/wpa_supplicant.conf，真实凭据不上Git，权限0600。复用BSP已有supplicant；否则按需启动，DHCP/发现有限等待。现有服务修改配置后需维护者重载或重启设备，不靠客户端重连反复启动系统服务。

配置读取改为先借用字符串校验，成功后复制一次；保留UUID、标准IPv4、端口和SPKI配对规则。入口删除--save-wifi，保留运行/配置检查。clientctl删除AP生命周期，保留单实例锁、三次有限重启、残留客户端清理和更新失败回滚。

## 接口迁移

| 原接口/结构 | 当前接口 | 调用和数据 |
| --- | --- | --- |
| DeviceUi及Impl | ui::open/show/poll_action/close/load_volume | App按值交付UiView、消费UiAction；不持有UI对象 |
| LvglScreen及Impl/页面路由 | page::open/show/camera/present/close | UI线程操作固定控件；show一次更新全部显示 |
| SetCameraFrame再次复制 | page::pixels + camera_capture::read | UI稳定像素数组一次接收最新完整帧 |
| CameraCapture对象/外部停止PID | camera_capture::open/read/close | UI控制生命周期，worker独占回收 |
| network_setup::FindServer | network::find_server、Endpoint | 仅网络线程调用；返回服务器及实际接口 |
| 隐含默认路由 | network::bind_socket | UDP发现与TCP连接使用一致接口 |
| save_wifi/--save-wifi/AP门户 | 系统wpa_supplicant.conf | 学生直接编辑SSID/PSK，无运行时配置框架 |
| 字符串配置拷贝后再校验 | string_view校验后一次拷贝 | 不修改合法输入范围，不记录字段值 |

## 计数与预算

ELOC为非空、非纯注释的物理代码行，含括号、声明、include、内联和私有硬件代码。范围为client/apps/boompi_client、client/src、client/include。测试/模拟器、脚本、Go、资源和第三方不混入板端C/C++总量。

本轮逐文件和逐模块数字由以下命令产生，CI保留同一提交的client-metrics.json和modules.json，不能沿用旧绿色CI或把预算当作实际：

```sh
python3 scripts/measure_teaching.py
python3 scripts/measure_teaching.py --before 2ca0cf63d241a032292f4d83a3120002b91a066e --after HEAD
```

第一次压缩重点是UI和配网，显示触摸驱动保持347 ELOC；WSS为了真实接口绑定/失败回退反而略增。约2960–3305 ELOC是事前预算，不是验收硬配额；最后提交报告给出实际计数，不为凑数删边界。纯音频仍为1175 ELOC/1384物理行，不是将这些数字互换。

运行脚本另算：旧S99+clientctl+两个配网脚本707物理行，现为S99+clientctl197物理行；此减少不再加到C/C++删减百分比。保留的预览工具、发布脚本、测试未混入该小计。

## 验证入口与证据边界

- 原ci：Linux/Windows/macOS严格Host构建与CTest、Python、Go test/vet；Linux另跑race与C++/Go真实TLS/WSS联测。
- teaching-ui：固定LVGL8.2 `0b5a1d4b23975b920ff841ea9cd038802f51711b`，编译实际页面、SDL与Linux显示端口。
- ui-pages：真实LVGL/FreeType，反复open/close、300次两页往返、触摸动作、音量及缺字回归；输出真实渲染的voice/camera预览。
- ui-runtime-lifecycle：执行实际UI线程/注册/回收，替换硬件与页面内容，检查失败初始化和反复开关。
- camera-lifecycle：真实fork/pipe/poll/线程/进程组，仅替换execl外部工具；完整帧、短读、部分帧退出、关闭、子进程回收。
- network-interface-contract：停止/错误接口；独立netns中两张dummy网卡验证有线优先、无线回退和实际socket绑定，不宣称完成RF/Wi-Fi关联或真实DHCP验收。
- ASan/UBSan：UI页面/运行、摄像头、网络回归，不关闭泄漏检测，不改vendor。
- Python脚本回归：临时目录中执行实际clientctl，检查启动、杀掉supervisor后清理child、更新失败回滚；不接触用户板子。

初次sanitizer找出FreeType重复初始化和draw_ctx释放遗漏，已在自有调用层修复：lv_init只建立一次进程级缓存，页面释放字体但不重复创建/销毁全局库；字体先通过FreeType预检，绕开旧适配器无效字体的名字引用泄漏；端口显式释放LVGL8.2没有随display删除的draw_ctx。进程结束的测试统一释放全局缓存。保留原库源码。

本地已运行15项Python回归、协议fixture、GCC应用/配置/摄像头/网络检查及应用/摄像头ASan/UBSan；完整依赖检查由该提交的远程CI核对。不能将上一提交结果当成最终SHA结果。

匹配RV1106 SDK未配置：独立crosscheck因BOOMPI_RV1106_SDK_ROOT/TOOLCHAIN_ROOT缺失停止，未生成ARM ELF、未检查目标loader/依赖、未部署、未调用收费云端。Host成功只代表对应测试通过，真实声学/显示/触摸/联网仍待用户验收。

## 上板顺序

先使用原分支的2ca0cf6验收音频，再单独构建此分支进行非音频验收；本分支已经包含原音频补丁，不需再次复制修改音频。

**覆盖文件前，先用旧版boompi-clientctl stop停止旧客户端和AP配网服务。** 再安装本分支客户端和启动脚本；不删除已有Wi-Fi配置、server.conf或服务端身份。不在本轮自动刷新镜像/设备树。

板端检查：语音页面/字幕/音量、反复切相机并返回、有线正常、有线不可达时无线连接、断网恢复、退出与重启。Wi-Fi密码错误、错误server pin、拔网线都应明确失败/回退而非卡住；观察日志区分网络错误与音频自打断。
