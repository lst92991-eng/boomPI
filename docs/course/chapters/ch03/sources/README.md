# 第3章官方资料索引

访问日期：2026-08-10。第3章优先使用与板端主版本相邻的上游Linux 5.10文档解释稳定概念。boomPI快照中的厂商Linux 5.10.160可能包含补丁、配置和驱动差异，不能与上游5.10实现画等号；POSIX、Buildroot和Ubuntu当前网页也只用于各自注明的概念边界。

| 主题 | 官方来源 | 正文用途 |
|---|---|---|
| RV1106启动能力 | Rockchip *RV1106 Datasheet* Rev 1.8（本地归档见第1、2章来源目录）；[RV1106官方产品页](https://www.rock-chips.com/a/cn/product/RV11xilie/2022/0926/1661.html) | 只支撑BootROM支持介质列表，不推导boomPI启动优先级或失败回退 |
| 内核与完整系统 | [kernel.org: About Linux Kernel](https://www.kernel.org/linux.html) | 区分内核与可工作的完整系统 |
| 用户空间API | [Linux 5.10 userspace API](https://docs.kernel.org/5.10/userspace-api/index.html) | 解释应用通过约定接口使用内核功能 |
| 进程与线程 | [POSIX.1-2024 Base Definitions](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap03.html) | 核对进程、线程与线程标识的稳定定义 |
| 调度 | [Linux 5.10 CFS Scheduler](https://docs.kernel.org/5.10/scheduler/sched-design-CFS.html) | 说明内核会记录运行时间并选择下一任务；不据此声称板端实时性 |
| 内存管理 | [Linux 5.10 Memory Management](https://docs.kernel.org/5.10/admin-guide/mm/index.html) | 解释虚拟内存、用户空间和文件映射 |
| 虚拟文件系统 | [Linux 5.10 VFS](https://docs.kernel.org/5.10/filesystems/vfs.html) | 说明不同文件系统怎样向应用提供共同入口 |
| 系统调用 | [Linux 5.10 Adding a New System Call](https://docs.kernel.org/5.10/process/adding-syscalls.html) | 说明系统调用是用户空间与内核的交互点 |
| 中断 | [Linux 5.10 Generic IRQ](https://docs.kernel.org/5.10/core-api/genericirq.html) | 说明硬件事件怎样进入内核；正文不展开IRQ实现层次 |
| 设备模型 | [Linux 5.10 Device Model](https://docs.kernel.org/5.10/driver-api/driver-model/overview.html) | 说明设备与驱动的统一关系，不外推板端绑定状态 |
| 设备树 | [Linux 5.10 Device Tree](https://docs.kernel.org/5.10/devicetree/usage-model.html) | 说明设备树向内核描述硬件，不把设备树等同于驱动 |
| 引导程序 | [U-Boot v2023.07.02 bootm](https://docs.u-boot.org/en/v2023.07.02/usage/cmd/bootm.html) | 说明引导程序把内核、ramdisk和设备树交给操作系统；不是当前板版本证据 |
| init启动 | [Linux 5.10 No working init found](https://docs.kernel.org/5.10/admin-guide/init.html) | 说明根文件系统、驱动、init和依赖的交接关系 |
| Buildroot | [Buildroot User Manual](https://buildroot.org/downloads/manual/manual.html) | 说明Buildroot在主机生成嵌入式Linux系统，不是板端内核 |
| Ubuntu | [Ubuntu official introduction](https://ubuntu.com/about) | 区分主机开发环境与boomPI板端Buildroot用户空间 |

正文中的3张框图均由本章脚本根据上述概念重新绘制，不复制第三方示意图，也不表示boomPI已经完成对应板端测试。
