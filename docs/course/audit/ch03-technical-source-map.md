# 第3章《操作系统简介》技术来源地图

建立日期：2026-08-10（Asia/Shanghai）
工作区基线：`codex/docs-course-rewrite-20260810`，提交 `7efd7694273c5e75f087dffd6511e3280ec53795`
用途：供第3章作者写作、配图和后续技术审校使用；本文不是正文。
范围：只解释操作系统的职责、内核与用户空间、进程/线程、调度、虚拟内存、系统调用、中断、文件系统和启动链。具体 Linux 命令、登录、设备节点操作、硬件测试、Buildroot 编译、烧录和音频均不进入本章。

## 1. 章节位置与写作约束

`docs/course/status/chapters.md` 当前把第3章列为“未开始”，位于第1章开发板、第2章原理图之后，第4章 Linux 系统简介之前。第3章回答“硬件上为什么还需要一层系统、系统替应用做什么”；第4章才回答“登录以后怎样在 Linux 中查看和操作资源”。

建议只设三个主要分节：

1. **硬件有了，为什么还需要操作系统**：用处理器、内存、存储和外设这四类第1/2章已经认识的资源，说明 OS 负责分配、隔离和提供统一入口。
2. **应用怎样请内核做事**：连续解释用户空间、系统调用、进程/线程、调度、虚拟内存、中断和文件系统；不要把每个名词拆成一个短小节。
3. **从上电到应用出现**：用 `BootROM → bootloader → kernel → init → app` 串起一次启动；设备树和根文件系统作为旁路物料，不强行塞进主箭头当作“会执行的程序”。

本章是概念章，不设命令练习，不用虚构终端，不让读者修改板端状态。通关目标只应是：读者能把一个应用请求放到“应用—内核—硬件”三层中，并能按顺序说出五个启动阶段各自交接什么。

## 2. 本章证据等级

| 等级 | 来源 | 本章能证明什么 | 不能证明什么 |
|---|---|---|---|
| S1 | POSIX、Linux kernel、U-Boot、Buildroot、Rockchip 等官方一手资料 | 稳定概念、上游接口模型、特定版本的设计说明、SoC 厂商能力 | boomPI 当前镜像一定启用了某配置或走过某条实际路径 |
| S2 | 当前板端只读原始证据 | 2026-08-10 这块板、该镜像的一次系统身份、挂载、进程和节点快照 | 早期 BootROM/U-Boot 完整日志、镜像来源提交、内核配置、长期状态 |
| S3 | 当前仓库配置、发布说明和候选 BSP 清单 | 当前仓库或历史候选怎样配置、构建或命名组件 | 当前板正在运行该产物；历史候选等于当前镜像 |
| S4 | 工作区教学参考 DOCX | 章节节奏、初学者词汇顺序、图文行为 | 技术事实；其中的 Luckfox 示例、分区、命令和笼统结论不能直接移植 |

所有正文句子都应先判断属于“稳定概念”还是“当前板对应”。推荐句式是：“操作系统通常负责……；在当前 boomPI 证据中，我们只观察到……”。不要把两层事实合成一句“boomPI 的系统已经完整管理所有硬件”。

## 3. 官方/一手来源登记

网络资料访问日期均为 2026-08-10。HTML 没有稳定 PDF 页码时，以下用官方章节标题或标准条款号定位；不得在正文伪造页码。

| ID | 官方来源与定位 | 可直接支撑的事实 | 版本边界 |
|---|---|---|---|
| R1 | Rockchip, *RV1106 Datasheet*, Rev 1.8，2025-03-12，本地 PDF 第6页；[RV1106 官方产品页](https://www.rock-chips.com/a/cn/product/RV11xilie/2022/0926/1661.html)。本地 SHA256 `2CAC8EDED045E404DC67D4D4662684CBEF48FFB1737E3533F52E798E5BA4A017` | RV1106 BootROM 支持从 SPI、eMMC、SD/MMC 启动，并支持 USB/UART 下载 | 第6页只是能力列表，不给 boomPI 的实际优先级、失败回退、Loader 布局或当前启动日志 |
| R2 | kernel.org [About Linux Kernel](https://www.kernel.org/linux.html)，整页 | kernel.org 明确提示内核只是可工作的 Linux 系统中的一个组件 | 该页是概念入口，不描述 boomPI BSP，也不定义完整发行版包含哪些包 |
| R3 | GNU 项目 [GNU/Linux FAQ：What is the difference between an operating system and a kernel?](https://www.gnu.org/gnu/gnu-linux-faq.html#osvskernel) | 可交叉说明“内核”和“完整可用系统”不是同一层；内核分配机器资源并支撑其他程序 | GNU 对系统命名有明确立场；正文只取层级区别，不把命名争论写进初学章 |
| R4 | Linux kernel 5.10 [The Linux kernel user-space API guide](https://docs.kernel.org/5.10/userspace-api/index.html)，开头与目录 | 内核有面向用户空间的 API；官方同时指向 Linux man-pages 项目 | 只证明接口层存在，不等于所有程序动作都是一次直接系统调用 |
| R5 | Linux man-pages 项目 [syscalls(2)](https://man7.org/linux/man-pages/man2/syscalls.2.html)，`DESCRIPTION`；[kernel.org 项目入口](https://www.kernel.org/doc/man-pages/) | 系统调用是应用与 Linux 内核之间的基础接口；许多调用通常经 C 库包装函数进入 | 当前网页是 man-pages 6.18；boomPI 的 libc、ABI 和具体调用集合仍需当前镜像/工具链确认。本章不列系统调用表 |
| R6 | The Open Group, POSIX.1-2024 [Base Definitions, Chapter 3](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap03.html)，3.279 `Process`、3.388 `Thread`、3.389 `Thread ID`、3.391 `Thread List` | 进程与线程是不同概念；线程在进程生命周期内有标识，调度模型处理 runnable threads | POSIX 是可移植接口模型，不是 Linux 内核内部结构说明；不要把 POSIX 术语机械等同于 Linux `task_struct` |
| R7 | The Open Group, POSIX.1-2024 [System Interfaces, General Information](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html)，2.8.4 `Process Scheduling`、2.9.4 `Thread Scheduling` | POSIX 把进程描述为包含一个或多个可调度线程的资源集合；调度从可运行线程中选择执行者 | 具体策略和默认参数由实现与配置决定；不能据此声称当前板运行 FIFO/RR 或满足实时性 |
| R8 | Linux kernel 5.10 [CFS Scheduler](https://docs.kernel.org/5.10/scheduler/sched-design-CFS.html)，1 `OVERVIEW`、2 `FEW IMPLEMENTATION DETAILS`、6 `SCHEDULING CLASSES` | Linux 5.10 普通任务的 CFS 用运行时间记账和调度类选择下一个 eligible task；`pick_next_task` 的职责是选择下一任务 | 这是 5.10 上游设计说明。厂商补丁、内核配置、任务 nice/策略和实际调度延迟均未由当前证据确认；本章不展开红黑树或参数调优 |
| R9 | Linux kernel 5.10 [Memory Management](https://docs.kernel.org/5.10/admin-guide/mm/index.html)，`Memory Management` 开头 | 内存管理涵盖虚拟内存、按需分页、内核/用户程序分配和文件映射到进程地址空间 | 当前板是否启用 swap、overcommit、THP、CMA 细节及可用内存量不能由概念页推出 |
| R10 | Linux kernel 5.10 [Overview of the Linux Virtual File System](https://docs.kernel.org/5.10/filesystems/vfs.html)，`Introduction` | VFS 是内核中向用户程序提供文件系统接口并允许不同文件系统共存的抽象层；`open/read/write` 等从进程上下文进入 | “VFS 统一入口”不等于所有硬件或所有 OS 资源都是普通磁盘文件，也不等于每个文件都在同一种介质上 |
| R11 | Linux kernel 5.10 [Linux generic IRQ handling](https://docs.kernel.org/5.10/core-api/genericirq.html)，`High-level IRQ flow handlers` | 内核按电平、边沿等中断流调用处理逻辑，处理中包含 mask/ack/event/unmask 等职责 | 该页面向内核开发者；第3章只写“硬件事件能通知内核”，不教 IRQ 编号、上半部/下半部或绑定细节，也不映射到未实测外设 |
| R12 | Linux kernel 5.10 [The Linux Kernel Device Model](https://docs.kernel.org/5.10/driver-api/driver-model/overview.html)，`Overview`、`User Interface` | 内核用统一设备模型描述总线、设备和驱动，并可通过 sysfs 向用户空间呈现层次 | sysfs 目录出现不等于驱动已绑定、设备可用或物理功能通过；第6章才做设备测试 |
| R13 | Linux kernel 5.10 [Linux and the Device Tree](https://docs.kernel.org/5.10/devicetree/usage-model.html)，2.1、2.3、2.4 | 设备树描述硬件，Linux 用它做平台识别、运行配置和设备填充；固件/bootloader 可把 DTB 传给内核 | 设备树不是驱动，也不会“自动让硬件工作”；本章只把 DTB画成 bootloader 交给 kernel 的硬件描述物料 |
| R14 | Das U-Boot v2023.07.02 [bootm command](https://docs.u-boot.org/en/v2023.07.02/usage/cmd/bootm.html)，`Description`、`Legacy boot`、`FIT syntax` | U-Boot 可以启动 OS，并可把 kernel、ramdisk 和 FDT 作为启动输入 | 当前板实际 U-Boot 版本、镜像格式、启动命令和验证策略未闭环；选 v2023.07.02 只是邻近版本的一手职责说明，不是 boomPI 版本证据 |
| R15 | Linux kernel 5.10 [Explaining the “No working init found.” boot hang message](https://docs.kernel.org/5.10/admin-guide/init.html)，故障顺序；Buildroot [User Manual](https://buildroot.org/downloads/manual/manual.html)，1 `About Buildroot`、6.3 `init system` | 内核需要挂载根文件系统并执行可用 init；Buildroot 文档把 init 定义为内核启动的第一个用户空间程序（PID 1），负责启动用户空间服务和程序 | Buildroot 在线手册会随版本更新；当前板标称 2023.02.6，精确选项必须回到对应标签和 BSP 配置。不能仅凭 Buildroot 默认值断言当前使用 BusyBox init |
| R16 | Buildroot 官方 GitLab [2023.02.6 标签树](https://gitlab.com/buildroot.org/buildroot/-/tree/2023.02.6)，提交 `593454c7`；当前官方 [Buildroot Manual](https://buildroot.org/downloads/manual/manual.html)，1 `About Buildroot` | Buildroot 是在主机上用交叉编译构建嵌入式 Linux 系统的工具，可生成工具链、rootfs、kernel image 和 bootloader image | Buildroot 不是板上持续运行的内核或服务；`/etc/os-release` 写 Buildroot 只标识该用户空间/系统构建来源，不证明构建可复现 |
| R17 | Ubuntu 官方 [About the Ubuntu project](https://ubuntu.com/about)，`The story of Ubuntu`；Ubuntu 官方 [Command line tutorial](https://documentation.ubuntu.com/desktop/en/latest/tutorial/the-linux-command-line-for-beginners/) | Ubuntu 是 Linux 发行版；本课程的 Ubuntu/WSL 是主机开发环境，可与板端 Buildroot 对照 | 第3章无需展开 Ubuntu 历史或命令。绝不能把 Ubuntu 写成当前 boomPI 板端系统 |

## 4. 工作区与当前板证据

### 4.1 可用证据

| ID | 路径与指纹 | 当前可写事实 | 不可外推 |
|---|---|---|---|
| E1 | `C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\evidence\board\20260810-dhcp-recovery\board-baseline-raw.txt`；SHA256 `2A288C875686F2707B017DE1867442F271146FD27C906F2E07C5A202472733FF` | 一次快照显示 `Linux 5.10.160`、`armv7l`、`Buildroot 2023.02.6`；内核命令行含 eMMC、`root=/dev/mmcblk0p7`、`rootfstype=ext4` 和 `androidboot.fwver=uboot-07/22/2026`；根挂载为 ext4 | 不证明上游 5.10.160 原样运行，不证明 BSP commit、补丁集、内核 `.config`、BootROM/U-Boot 完整身份、健康或长期稳定 |
| E2 | `C:\Users\lst\Desktop\小智文档\tmp\boompi-doc-v5\evidence\board\20260810-dhcp-recovery\board-topology-raw.txt`；SHA256 `3AF3F994D41DDFDF7EB64609F61DADE851DAB97D0861A084D094F99A78F51540` | `ps` 快照中 PID 1 为 `init`；可见内核线程、由 PID 1 派生的用户空间服务及 `rkipc` 应用；存在 `/proc`、`/sys`、`/dev` 和多种挂载 | 不能断言 PID 1 一定是 BusyBox init，不能把 `rkipc` 写成 boomPI 项目客户端，不能把一次进程表写成长稳或自启动策略验收 |
| E3 | `docs/hardware/bsp-candidate-manifest-20260729.md` | 历史候选配置曾指向 `luckfox_rv1106_uboot_defconfig`、Buildroot 2023.02.6 等 | 它是候选/历史配置，不能绑定到当前板端正在运行的二进制 |
| E4 | `docs/course/chapters/ch01/chapter01.md` 第81行及 R1 | 已有课程边界：BootROM 支持列表没有优先级或自动回退；当前板端只证实从 eMMC 启动 | 第3章不得恢复“先 eMMC 再 SD”或“失败自动 USB”之类未证实顺序 |

### 4.2 当前身份截图暂不作为冻结来源

`terminal-board-identity.png` 当前文件 SHA256 为 `91BA8A2D942E804B633F5FC568E827027F1A13849B9BE9C4F1204FDCF9C504B3`，与同目录 `technical-verdict.md` 记录的 `587CF1C86C712C1CDB006BE45ABC037E39453376736DF0ABB1E4DC25B58D9673` 不一致；当前图底部还保留英文截图提示和输入提示。第3章若要使用真实身份图，必须先对当前文件重新做视觉/哈希门禁，或重新裁出只含 Linux/Buildroot 身份的干净真实终端区域。未关闭前优先使用 E1/E2 的原始文本作内部事实依据，正文不引用旧哈希。

## 5. 稳定概念与可写边界

### 5.1 操作系统负责什么

**可写事实**

- 为课程建立清晰约定：完整可用的板端系统包括 Linux kernel 和用户空间程序/库/配置；kernel 是其中负责管理处理器、内存、设备和底层接口的核心组件。
- 处理器时间由调度器在可运行任务之间安排；内存管理为内核和用户程序分配/映射内存；VFS 统一文件系统入口；驱动与 IRQ 机制承接设备事件；系统调用等用户空间 API 让应用请求内核服务。
- OS 的价值不是“替应用完成所有业务”，而是把共享硬件资源变成有边界、可复用的系统服务。界面、摄像头预览、网络问答等业务逻辑仍属于应用。

**禁写边界**

- 不写“OS就是Linux”“Linux就是Buildroot”“Buildroot是板上正在运行的程序”。
- 不写“操作系统保证所有设备正常”“内核版本越新越稳定”“有驱动名就等于硬件通过”。
- 不把安全、实时、可靠、性能优良当作“装了 OS”自动获得的属性。

### 5.2 内核与用户空间

**可写事实**

- 内核态和用户空间是权限与职责边界：应用不能把普通函数调用直接当作任意硬件访问；它使用受定义的用户空间接口请求内核。
- 系统调用是应用与 Linux kernel 之间的基础接口之一。应用通常通过 C 库包装函数使用它，设备类接口还可能表现为文件描述符、`ioctl`、`mmap`、sysfs 等上层形式。
- 当前板 E2 同时显示方括号形式的内核线程和普通用户空间进程，可作为“同一系统中存在两类执行上下文”的板级例子；不要让初学者记具体 PID。

**禁写边界**

- 不写“每个应用函数都会切进内核一次”“每条 shell 命令等于一个系统调用”“系统调用就是中断”。
- 不画用户程序直接连接寄存器的箭头，除非以后专门讲经驱动授权的映射；第3章只画应用到 kernel API 的受控入口。
- 不假定当前板使用 glibc。当前证据未冻结板端 libc 身份，项目工具链/发布记录中的 uClibc 也不能自动代表整套当前 rootfs。

### 5.3 调度、进程与线程

**可写事实**

- 进程可在初学章中描述为“拥有地址空间和一组系统资源的运行容器”；一个进程至少有一个线程，也可以有多个线程。
- 线程是进程内的一条执行流；同一进程中的线程会共享相当一部分进程资源，但仍有各自的执行状态和线程标识。
- 调度器只在“可运行”的候选中选择接下来占用处理器的任务；等待 I/O 的任务不等于一直占着 CPU。
- 对当前 5.10 上游文档，只需写普通任务会被调度器轮换执行，不向初学者展开 CFS 红黑树、虚拟运行时间公式或实时策略参数。

**禁写边界**

- 不把“程序文件”“进程”“线程”当同义词；文件尚未运行时不是进程。
- 不写“线程一定比进程快/省内存”“多线程一定提高性能”“线程越多越实时”。
- 不写当前 boomPI 应用用了多少线程、谁拥有某设备、实际优先级是多少；这些属于第7/11章源码和板端证据。
- 不凭 `Linux 5.10.160` 断言 `PREEMPT_RT`、SCHED_FIFO、SCHED_RR、nice 值或调度延迟。

### 5.4 虚拟内存

**可写事实**

- 用户程序看到的是自己的虚拟地址空间；内核通过页表和内存管理把虚拟地址映射到实际内存或文件映射。
- 不同进程使用相同数值的虚拟地址，不代表它们访问同一物理位置；这种隔离能降低程序彼此误伤的风险，但不等于绝对安全。
- Linux 5.10 上游内存管理职责包括虚拟内存、按需分页、内核/用户空间分配和文件映射。

**禁写边界**

- 不把 RV1106G3 标称 2 Gb DDR3L 直接写成 Linux 可用 256 MB；固件保留区、CMA、内核和硬件占用会影响可用量。
- 不把虚拟内存解释成“硬盘自动扩展内存”；swap 是否存在和怎样配置需板端确认，当前章不查询、不配置。
- 不声称每个进程都拥有整个物理内存，也不在示意图中把地址/容量画成真实比例。

### 5.5 系统调用与中断

**可写事实**

- 系统调用是软件从用户空间主动请求内核服务的受控入口；中断是硬件/中断控制器把事件通知 CPU 和内核处理路径的机制。二者方向和触发来源不同。
- 一个入门级完整因果可以写成：“应用请求读取数据 → 内核检查并安排驱动 → 设备完成后以事件通知内核 → 内核把结果交还应用。”这只是概念流程，不绑定具体设备节点和调用次数。

**禁写边界**

- 不写“中断越多越快”“每个外设只有一个中断号”“中断处理程序可以任意长时间工作”。
- 不用当前 `eth0 Interrupt:51`、`[irq/*]` 线程名推导 boomPI 每个外设的 IRQ 映射；没有本轮 `/proc/interrupts` 与设备树/驱动交叉证据。
- 不在本章给 `strace`、`/proc/interrupts`、优先级或绑核命令。

### 5.6 文件系统

**可写事实**

- VFS 在内核中为用户程序提供一致的文件系统接口，同时允许 ext4、tmpfs、devtmpfs、proc、sysfs 等不同实现共存。
- 当前板 E1 可作为板级对应：根目录来自 ext4；`/dev` 是 devtmpfs，`/proc` 是 proc，`/sys` 是 sysfs，说明“一棵目录树”可挂入不同类型的文件系统。
- 设备节点是用户空间访问某些内核设备接口的一种形式；它不是硬件本体。

**禁写边界**

- 避免把“一切皆文件”当严格事实。网络接口、进程、内存映射、套接字、设备模型等都有不同 API；更准确的初学表达是“Linux 尽量用统一的文件描述符和目录接口呈现许多资源”。
- 不写 `/dev/video14`、`/dev/input/event0` 等固定节点，也不进入挂载、读写、权限或分区命令；它们留给第4/6章。
- 不把“已挂载”写成存储耐久、掉电安全或文件系统健康通过。

## 6. 启动链：可写职责、当前对应与禁写结论

主图建议使用一条水平箭头：

`BootROM → bootloader → Linux kernel → init（PID 1）→ services / app`

在 bootloader 到 kernel 的箭头旁放 `kernel image + DTB（可选 initramfs）`；在 kernel 到 init 的下方放 `root filesystem`。DTB、rootfs 和镜像是数据/物料，不画成会主动执行的角色。

| 阶段 | 可写稳定事实 | 当前 boomPI 对应 | 必须避免 |
|---|---|---|---|
| BootROM | SoC 上电后首先运行的片内固定代码，负责寻找/装入后续启动内容；R1 第6页列出 RV1106 支持的启动/下载介质 | 第1章已确认 boomPI 设计有 eMMC、microSD 和 USB 数据路径；当前 E1 只说明系统最终从 eMMC 根分区运行 | 不写启动优先级、自动回退、Maskrom/Recovery 条件，不把“支持USB下载”写成当前 USB 路径已满足恢复条件 |
| bootloader | 引导加载程序准备后续启动条件，选择/装入 kernel，并可传递 DTB、initramfs 和内核参数 | E1 命令行含 `androidboot.fwver=uboot-07/22/2026`，分区表含名为 `uboot` 的分区；E3 历史候选使用 U-Boot defconfig。这些共同指向 U-Boot，但不是当前 bootloader 二进制闭环 | 当前未保存从第一字节开始的串口日志、bootloader 文件哈希、版本输出和配置；正文宜写“当前证据指向 U-Boot”，不能写精确版本、启动命令或已验证安全启动 |
| Linux kernel | 接管 CPU/内存，解析平台描述，初始化内核子系统/驱动，并取得根文件系统后尝试启动 init | E1 观察到 Linux 5.10.160 armv7l、设备树 model/compatible、ext4 根文件系统 | 不写这是未修改上游 5.10.160，不写所有驱动成功、不用版本号证明系统健康或安全支持周期 |
| init | 内核启动的第一个用户空间程序，PID 为1；它再启动用户空间服务和应用 | E2 的 PID 1 名为 `init`，并有多项 PPID 1 的服务/应用 | 未解析 `/proc/1/exe` 与当前 rootfs 配置前，不写 BusyBox init/systemd/SysV 的确定身份；不提前教 init 脚本和自启动修改 |
| app | 在用户空间实现产品功能，借助 OS API 使用资源 | E2 有 `rkipc` 进程；它只代表当前镜像中的一个 IPC 应用。当前证据未显示最终 boomPI client 正在运行 | 不把 `rkipc` 冒充 boomPI 项目，不把进程存在写成功能、UI、相机或音频通过，不写最终项目已自启动 |

本章可以说“当前板已经运行到 Linux kernel 和 PID 1 init 之后的用户空间”；不能说“本轮完整验证了 BootROM→U-Boot→kernel→init 的每一步”。完整早期链需要同一块板、同一镜像、从上电第一字节开始的串口日志，以及 bootloader/kernel/DTB/rootfs 身份哈希。

## 7. Linux、Buildroot、Ubuntu、rootfs、应用的准确区分

| 名称 | 本章一句话定义 | 当前工作区对应 | 禁写说法 |
|---|---|---|---|
| Linux kernel | 管理底层资源并向用户空间提供接口的内核 | 当前板一次快照为 5.10.160 armv7l | “Linux 5.10.160就是完整系统” |
| Buildroot | 在开发主机上通过交叉编译生成嵌入式系统物料的构建系统 | 当前板 `/etc/os-release` 标识 Buildroot 2023.02.6；历史候选配置也使用该系列 | “Buildroot正在板上调度进程”“Buildroot等于Linux内核” |
| root filesystem | 内核进入用户空间后可见的根目录树，装有 init、库、配置和应用 | 当前根挂载为 ext4，E1/E2 显示多个虚拟文件系统挂入目录树 | “rootfs就是内核”“所有根文件系统启动后一定改为可写” |
| Ubuntu | 一种 Linux 发行版，本课程用于主机/WSL 开发环境 | 当前板端 E1 明确是 Buildroot，不是 Ubuntu | “板子运行Ubuntu”“Ubuntu是编译器” |
| app | 用户空间中的产品程序 | 当前仓库未来项目客户端；当前板快照只明确见到 `rkipc` 等现有应用 | “应用直接控制所有硬件”“进程存在等于业务闭环通过” |

## 8. 建议插图与证据要求

### 图3-1：应用—内核—硬件三层图（建议生成概念图）

- 上层只放“界面/网络/业务应用”三个普通名称；中层放 Linux kernel，并在内部只分“调度、内存、文件、驱动”四块；底层放处理器、内存、存储、外设。
- 应用到内核只画一条“系统接口”箭头，内核到硬件画“驱动/中断”双向箭头。
- 图注必须写“概念示意，不代表当前 boomPI 全部功能已实测”。不要放源码类名、设备节点、采样率或音频元素。

### 图3-2：进程与线程最小关系图（建议生成概念图）

- 一个进程框内画共享的“地址空间/打开资源”，再画两条线程执行线；另画第二个独立进程框。
- 只表达“同一进程可含多个线程、进程之间地址空间分开”，不画性能高低、优先级或真实内存地址。
- 配一行人话：“进程装着资源，线程沿着代码往前执行。”正文随后立即给 POSIX 正式边界，避免比喻替代定义。

### 图3-3：一次请求与一次硬件事件（建议生成概念图）

- 顺序为“应用提出请求→kernel/driver安排→硬件完成并通知→结果返回应用”。
- 用不同箭头区分“主动请求”和“硬件通知”，帮助读者不把 system call 与 interrupt 当同一件事。
- 不绑定摄像头、触摸、网卡或音频，不放具体 IRQ/系统调用名。

### 图3-4：五阶段启动链（本章主图）

- 主链使用 `BootROM→bootloader→kernel→init→app`；DTB 和 rootfs 作为侧边输入。
- 每个框只放“找下一段、装入内核、管理资源、启动服务、实现业务”一行动词。
- 在框下用小型证据标签区分：`SoC能力`、`当前证据指向`、`当前已观察`。BootROM/bootloader 不得误标为本轮完整实测。
- 不画未经确认的分区地址、优先级、加载地址、密钥/安全启动或自动回退。

### 图3-5：板端身份真实截图（可选，当前阻塞）

- 只有当前截图哈希重新门禁后才可使用；建议裁掉网络信息、英文截图提示和输入提示，只保留 hostname、Linux 版本和 Buildroot 标识的真实终端区域。
- 若无法形成干净可追溯截图，本章宁可不用真实终端图，也不要用重新排版的伪终端替代。板端操作和完整命令截图留到第4章。

## 9. 工作区参考文档的可借鉴行为与技术纠偏

### `文本合格参考标准/0.基础概念.docx`

- 可借鉴：从引导加载程序、kernel、设备树、rootfs 四个角色逐步进入；第一次出现缩写时同时给中文用途。
- 不直接沿用：文档一次堆入过多体系结构、GPL与内部阶段；“U-Boot固定分boot+loader两阶段”“内核总会先initramfs再只读挂根并改成读写”“Buildroot/Ubuntu/Debian/BusyBox都是同类rootfs制作工具”等表述过宽或条件缺失。
- 本章替代动作：用 R1/R13/R14/R15 把职责和实际链拆开；rootfs 挂载、init 路径和 bootloader 形式都保留条件。

### `文本合格参考标准/4.分区信息与 Linux 命令.docx`

- 可借鉴：先用 Windows 熟悉经验引出 Linux 目录树，概念后紧邻具体对象。
- 不直接沿用：Luckfox 的分区表、挂载结果、默认提示符和命令属于另一板/镜像；“一切都是文件”只能当口号，不能覆盖网络、进程、内存映射和各种专用 API。
- 本章替代动作：只用 E1 说明当前根目录树挂有 ext4、devtmpfs、proc、sysfs；命令、路径操作和分区细节全部后移到第4/5章。

两份 DOCX 的 OOXML 没有可靠的最终页码缓存，本地图以章节标题和小节号定位，不虚构页码。正式正文若需要引用其版式行为，应在章节作者完成本地渲染后再登记实际页码；技术事实仍不得由参考 DOCX 单独支撑。

## 10. 后续仍需官方网络或本板证据补齐的项目

| 项目 | 需要的官方/一手材料 | 未补齐前正文边界 |
|---|---|---|
| RV1106真实早期启动 | 与当前 BSP/loader 匹配的 Rockchip BootROM、Maskrom/Recovery、Loader、分区和烧录官方文档；完整串口日志 | 只写 R1 的支持介质，不写顺序、回退、模式进入条件或实际 U-Boot 阶段 |
| 当前 bootloader 身份 | 当前板 bootloader 文件/分区哈希、版本输出、配置与从上电开始的串口日志 | 只写“证据指向U-Boot”，不写精确版本和命令 |
| 当前 kernel 身份 | BSP/SDK commit、5.10.160 vendor patch set、`.config`、kernel/DTB 哈希 | 只写运行时版本字符串，不写上游原样、配置或支持周期 |
| 当前 init 实现 | `/proc/1/exe`、可执行文件哈希、Buildroot 2023.02.6 对应配置、`/etc/inittab`/init 脚本归属 | 只写 PID 1 名为 `init`，不写 BusyBox/systemd/SysV 确定身份 |
| 调度/内存特性 | 当前 `.config`、运行时策略、内存布局/保留区、swap 与 CMA 证据 | 不写 PREEMPT_RT、实时保证、可用256 MB、swap存在或具体延迟 |
| Ubuntu/主机历史 | 仅当正文真的需要历史时使用 Ubuntu 官方项目页；GNU历史只用GNU官方页 | 本章不需要年代叙事；只把 Ubuntu 标成主机发行版，避免抢走 OS 主线 |

## 11. 第3章技术门禁清单

- [ ] 开头承接第2章的“硬件连好了，但应用不能直接各自抢硬件”，不从操作系统历史年表开篇。
- [ ] 正文明确课程约定：Linux kernel 与完整板端系统不是同一层。
- [ ] Buildroot 写成主机构建系统；Ubuntu 写成主机发行版；两者都不与 kernel 混写。
- [ ] OS 职责覆盖 CPU调度、内存、设备/中断、文件系统和用户空间接口，但不展开命令或参数。
- [ ] 进程/线程定义与 POSIX 兼容，不写“线程一定更快”或“进程就是程序文件”。
- [ ] 虚拟内存不等于 swap，不把 2 Gb DDR3L 写成 Linux 可用256 MB。
- [ ] system call 与 interrupt 的方向、来源分开，不写成同一机制。
- [ ] 文件系统使用 VFS/挂载树的准确说法，不把“一切皆文件”写成绝对定律。
- [ ] 启动主链为 BootROM→bootloader→kernel→init→app；DTB/rootfs 画成物料，不画成执行阶段。
- [ ] 当前板只确认 Linux/Buildroot 标识、eMMC根、PID1 init和用户空间进程；BootROM/U-Boot完整链仍未实测闭环。
- [ ] 不出现具体 Linux 命令、设备节点、硬件测试、烧录动作、项目线程数或音频实现。
- [ ] 所有概念图注明为示意；真实终端截图在当前哈希漂移关闭前不进入冻结正文。

满足以上条件后，第3章才适合进入正文写作。下一章的自然入口应是：“知道应用依靠内核以后，接下来登录这套 Buildroot 用户空间，看看这些资源在 Linux 中怎样被呈现。”
