# 第4章《Linux系统简介》技术来源与当前板证据地图

建立日期：2026-08-10（Asia/Shanghai）
用途：供第4章写作、截图和技术审校使用；本文不是正文。
范围：只介绍后续操作必需的 Linux/Buildroot/Ubuntu 区分、终端、目录树、挂载、进程、权限与设备节点。烧录、分区改写、网络持久配置和硬件功能验收均后移；音频仍放在全文末段。

## 1. 本章证据分层

| 层级 | 来源 | 能证明 | 不能证明 |
|---|---|---|---|
| 上游/标准 | kernel.org、Linux 5.10上游文档、FHS 3.0、Buildroot/BusyBox/Ubuntu官方资料 | 稳定概念、上游5.10接口模型、工具的一般职责 | 厂商5.10.160的补丁/配置、当前板某功能通过 |
| 当前板原始证据 | `board-baseline-raw.txt`、`board-topology-raw.txt`及真实终端截图 | 2026-08-10这块板、该镜像的一次身份/挂载/进程/节点快照 | 长稳、重启后状态、硬件物理结果、当前BSP可复现 |
| 审计结论 | 同目录 `technical-verdict.md` | 对原始证据的PASS/FAIL/BLOCKED边界 | 不能替代原始日志；其中终端截图哈希已与当前文件漂移 |
| 工作区参考DOCX | 《登录》《分区信息与 Linux 命令》《自启动与静态IP配置》 | 截图节奏、从终端现象进入概念的写法 | 另一块Luckfox板的IP、UBIFS分区、提示符、用户名和命令结果 |

正文统一句式宜为：“上游文档说明……；在2026-08-10当前板快照中观察到……；这仍不能证明……”。

## 2. 官方/一手来源

网络访问日期均为2026-08-10。HTML无稳定页码时按标题/小节定位。

| ID | 来源与定位 | 可写事实 | 版本边界 |
|---|---|---|---|
| O1 | kernel.org [About Linux Kernel](https://www.kernel.org/linux.html)，`New to Linux?` | 内核只是可工作Linux系统的一个组件；发行版是完整系统 | 不描述boomPI BSP或Buildroot配置 |
| O2 | Linux 5.10 [VFS Overview](https://docs.kernel.org/5.10/filesystems/vfs.html)，`Introduction`、`Registering and Mounting` | VFS向用户空间提供文件系统接口；挂载把某文件系统树接到挂载点 | 上游5.10.0文档不等于厂商5.10.160实现 |
| O3 | Linux 5.10 [The /proc Filesystem](https://docs.kernel.org/5.10/filesystems/proc.html)，Chapter 1、1.1 | `/proc`是内核数据接口；每个进程有按PID命名的目录 | `/proc/sys`含可写控制项；本章只读，不把整个`/proc`称为只读 |
| O4 | Linux 5.10 [sysfs](https://docs.kernel.org/5.10/filesystems/sysfs.html)，`What it is`、`Using sysfs` | sysfs向用户空间导出内核对象、属性和层次 | 属性可能可写；目录出现不等于驱动已绑定或硬件可用 |
| O5 | Linux 5.10 [Driver Model](https://docs.kernel.org/5.10/driver-api/driver-model/overview.html)，`Overview`、`User Interface` | 设备模型统一描述总线/设备/驱动并经sysfs呈现 | 不能由名字或节点外推实物通过 |
| O6 | Linux 5.10 [Linux allocated devices](https://docs.kernel.org/5.10/admin-guide/devices.html)，开头 | 字符/块设备使用主次设备号；现代系统可动态分配编号并结合sysfs/udev命名 | 编号和`/dev/videoN`次序不是永久业务标识 |
| O7 | Linux 5.10 [tmpfs](https://docs.kernel.org/5.10/filesystems/tmpfs.html)，开头 | tmpfs内容位于虚拟内存，卸载后内容丢失 | 不等于所有RAM数据都在tmpfs，也不证明当前板启用swap |
| O8 | Linux Foundation [FHS 3.0](https://refspecs.linuxfoundation.org/FHS_3.0/fhs/ch03.html)，Chapter 3 | 根目录常见职责：`/bin`、`/dev`、`/etc`、`/lib`、`/mnt`、`/root`、`/run`、`/tmp`等 | FHS是层次规范，不保证精简Buildroot镜像拥有所有可选目录；`/oem`、`/userdata`是当前产品路径 |
| O9 | Buildroot 2023.02.6官方标签 [introduction.txt](https://gitlab.com/buildroot.org/buildroot/-/raw/2023.02.6/docs/manual/introduction.txt) | Buildroot在主机用交叉编译生成工具链、rootfs、内核或bootloader物料 | Buildroot不是板上调度进程的服务；标签资料也不证明当前镜像可复现 |
| O10 | 当前 [Buildroot Manual](https://buildroot.org/downloads/manual/manual.html)，1、6.2、6.3 | 一般构建职责、`/dev`管理和init可配置 | 当前在线手册为2026.05；精确当前板事实应回到2023.02.6标签/BSP配置 |
| O11 | BusyBox官方 [BusyBox manual](https://busybox.net/downloads/BusyBox.html)，`DESCRIPTION`、`USAGE` | BusyBox是可裁剪的多调用程序，命令及选项取决于构建配置，通常少于GNU工具 | 当前板只确认`/bin/busybox`存在，尚未冻结BusyBox版本/配置；不能照搬Ubuntu/GNU长选项 |
| O12 | Ubuntu官方 [About Ubuntu](https://ubuntu.com/about)，`The story of Ubuntu` | Ubuntu是Linux发行版，可作为课程主机/WSL环境 | 当前板`/etc/os-release`为Buildroot，不是Ubuntu；主机具体Ubuntu版本仍需主机截图 |

## 3. 当前板冻结证据

证据目录：工作区外层 `tmp/boompi-doc-v5/evidence/board/20260810-dhcp-recovery/`

| ID | 文件与SHA256 | 可写事实 | 禁止外推 |
|---|---|---|---|
| E1 | `board-baseline-raw.txt`；`2A288C875686F2707B017DE1867442F271146FD27C906F2E07C5A202472733FF` | `Linux 5.10.160 armv7l`；`Buildroot 2023.02.6`、版本串`-g994243753-dirty`；DT model为`LST RV1106 Custom Board`；cmdline指向eMMC和`/dev/mmcblk0p7` ext4根；记录一次mount/df/节点快照 | 不写未修改上游5.10.160、BSP提交已知、`dirty`代表当前板文件被改、所有节点可用 |
| E2 | `board-topology-raw.txt`；`3AF3F994D41DDFDF7EB64609F61DADE851DAB97D0861A084D094F99A78F51540` | `/bin/busybox`和若干工具存在；PID1名为`init`；一次`ps -ef`、FD、sysfs绑定、媒体图快照 | 不写BusyBox init已确认、进程长期健康、PID固定、owner=none永远无人占用 |
| E3 | `technical-verdict.md`；`54B938FC9BCE6A56240CCFC53E164A88981818A5FEC71BE23863A85117AD5CE9` | SSH/以太网/存储/触摸/显示/相机/音频等证据边界总表 | 它记录的终端图SHA为旧值，截图项不能直接沿用 |
| E4 | 当前`terminal-board-identity.png`；`91BA8A2D942E804B633F5FC568E827027F1A13849B9BE9C4F1204FDCF9C504B3` | 视觉上能读到`/etc/hostname`、Linux 5.10.160、Buildroot dirty版本、脱敏eth0地址、carrier=1 | 与E3登记的`587CF1…D9673`不一致；图中保留英文截图提示/输入提示且地址行右端被截，不作为冻结成品，须重采或重新门禁 |

板端时间显示`2021-01-01`，而证据由主机在2026-08-10采集。正文/图注必须使用主机采集日期，不用板端时间排序或计算时延。

## 4. 目录与挂载：当前可写边界

E1一次快照观察到：

- `/`：`/dev/root`，ext4，`rw`；cmdline同时写`root=/dev/mmcblk0p7`。未补`readlink`/主次设备号前，不强行把`/dev/root`写成某种固定符号链接。
- `/dev`：devtmpfs；`/proc`：proc；`/sys`：sysfs；`/tmp`、`/run`、`/dev/shm`：tmpfs。
- `/mnt/sdcard`：`/dev/mmcblk1p1`，vfat；`/userdata`、`/oem`：ext4。
- configfs和functionfs也在挂载表中；不需要在入门正文展开。

可写：“一棵目录树可接入多个文件系统；挂载表是当时状态。”不可写：

- `rw`不等于建议学生写入，更不证明掉电安全、耐久或文件系统健康。
- `df`的容量/使用率不等于芯片标称容量、坏块检查或全盘验收。
- 挂载存在不等于SD卡/eMMC已经长期稳定；第6章才做受限测试。
- `/proc`、`/sys`、`/dev`不是普通静态文件目录；禁止泛用`cat /dev/*`，也不写`echo > /proc/sys/...`或`echo > /sys/...`。

## 5. 进程：当前可写边界

- E2中PID1显示为`init`；方括号项是内核线程的`ps`表示，`syslogd`、`sshd`、`rkipc`等是一次用户空间进程快照。
- `rkipc`在该瞬间为PID425并持有多项媒体FD；PID和FD会变化，正文不要求学生记数字。
- 进程“存在”只说明快照时能列出，不证明服务健康、业务功能通过、自启动配置正确或长稳。
- 未读取`/proc/1/exe`和对应Buildroot配置前，只写“PID1名为init”，不写“当前一定是BusyBox init”。
- `ps`/`/proc/<pid>`可能暴露命令行、环境变量或凭据；截图前脱敏，不展示`/proc/<pid>/environ`。

## 6. 设备节点与sysfs：当前可写边界

- E1列出`/dev/input/event0`、多项`/dev/video*`/`media*`/`v4l-subdev*`、`/dev/i2c-*`、`/dev/spidev0.0`和`/dev/snd/*`。这些是内核接口，不是同数量的物理器件。
- E2确认`event0`对应`adc-keys`；GT911的`3-005d`存在但`driver=none`。不得把`event0`写成触摸，也不得把sysfs设备目录存在写成I2C实测应答。
- 多个`videoN`是媒体管线节点，不是多颗摄像头；节点序号可能变。`video14`只属于本轮后续单帧证据，不在本章写成固定摄像头入口。
- `spidev0.0`存在/绑定只说明通用SPI用户接口出现，不证明屏幕物理显示。
- `/dev/snd`枚举只说明声卡/PCM接口存在；音频未授权、未打开PCM，继续后置。
- 字符`c`、块`b`、主次设备号和权限可作概念；root身份也不代表操作安全。任何设备操作先发现节点、driver和owner，再由对应硬件章执行。

## 7. 本章允许的只读命令组

以下命令在**板端SSH终端**运行，目标是不写持久数据；当前证据已见`cat`、`uname`、`ps -ef`、`mount/df`、`readlink`等能力。每组截图须包含命令、输出、退出码/通关句。

~~~sh
# 身份：已由E1支撑；可重采干净截图
cat /etc/hostname
uname -srmo
cat /etc/os-release
id

# 目录与挂载：待本章干净截图
pwd
ls -ld / /bin /sbin /etc /lib /usr /dev /proc /sys /tmp /run /oem /userdata /mnt /mnt/sdcard
cat /proc/mounts
df -h
cat /proc/partitions

# 进程：ps快照已有E2；init身份仍待补
ps -ef
cat /proc/1/status
readlink -f /proc/1/exe

# 只列元数据，不打开设备流
cat /proc/bus/input/devices
ls -l /dev/input/event* /dev/media* /dev/video* /dev/snd/* /dev/i2c-* /dev/spidev* 2>/dev/null
~~~

命令退出0只说明命令完成，不能替代内容判断；无输出也不能自动写成功。若某个glob无匹配导致`ls`非0，应写“本次未发现匹配节点”，不要补造节点。失败恢复仅为停止该只读命令/关闭SSH；本章不使用`touch/mkdir/cp/mv/rm/chmod/mount/umount/dd/devmem/i2cdetect`，不改网络、init脚本或权限。

## 8. 三个环境必须分开

| 环境 | 本章定位 | 禁止混写 |
|---|---|---|
| Windows/PowerShell | 打开终端、建立SSH会话、保存截图 | PowerShell命令不在板端执行；不在截图暴露Windows用户名/路径/密码 |
| Ubuntu/WSL | 后续SDK与交叉编译主机；Ubuntu是发行版 | 当前板不是Ubuntu；未有主机证据前不写具体Ubuntu版本 |
| boomPI板端 | 当前证据为Buildroot 2023.02.6用户空间、厂商Linux 5.10.160 | 不写`apt`/`systemctl`/GNU长选项必然存在；先看实际工具与`--help` |

截图中的`$`或`#`不能单独证明权限，必须以`id`为准。当前身份截图由脚本打印`$`样式命令，不能拿它教授“普通用户提示符”。

## 9. 工作区参考DOCX的可借鉴与纠偏

- 《3.登录》：可借鉴“操作—截图—解释”节奏；不得照搬关闭Windows防火墙、RNDIS固定IP、默认凭据、Luckfox Pro/Max结论或“Ubuntu用户一定是pico”。
- 《4.分区信息与 Linux 命令》：可借鉴从Windows目录经验进入Linux树和把`ls/pwd/df`结果贴近解释；原文UBIFS/UBI分区属于另一镜像，当前板为ext4根、vfat SD及多个伪文件系统；“一切皆文件”只能改成“许多接口用文件描述符/节点呈现”。写入、删除和`chmod`练习不进本章。
- 《5.自启动与静态IP配置》：仅借鉴Shell是命令解释入口；修改`/etc/init.d`、静态IP、网关、DNS和权限均是写状态操作，全部后移。原文硬编码地址与启动顺序不能套到当前板。

## 10. 建议正文结构与截图

1. 用一张重新采集的真实板端身份图区分“Ubuntu主机”和“Buildroot板端”；截图必须先关闭E4哈希/裁切问题。
2. 用当前板根目录与挂载表说明“一棵目录树、多个文件系统”；只解释`/`、`/etc`、`/dev`、`/proc`、`/sys`、`/tmp`、`/oem`、`/userdata`、`/mnt/sdcard`。
3. 用真实`ps -ef`截图说明PID1、内核线程与用户进程；不评价CPU性能/服务健康。
4. 用“event0实际是adc-keys、GT911未绑定”和“很多video节点不等于很多摄像头”建立设备节点安全边界；不打开节点。
5. 结尾停在“能读懂路径、挂载、进程和节点”，下一章才进入Buildroot/镜像/HelloWorld；不提前烧录或给硬件PASS。

## 11. 仍待补证

- 当前`terminal-board-identity.png`须重采或重新冻结哈希；E3旧哈希不可继续引用。
- 补一次`id`、根目录、`/proc/mounts`、`ps -ef`和设备元数据的干净真实终端截图。
- 补`/bin/busybox`版本与配置证据，才能准确列命令选项；未补前只使用本轮实际跑过的短选项。
- 补`readlink -f /proc/1/exe`和Buildroot配置，才能确定init实现。
- 如正文要写Ubuntu具体版本，必须补当前Ubuntu/WSL主机的`/etc/os-release`真实截图；官方YY.MM命名规则不能替代本机证据。
- 厂商5.10.160源码、补丁集和`.config`仍未归档；上游5.10资料只解释概念。

满足以上边界后，第4章可以介绍Linux系统对象，但不得写“看到节点=硬件正常”“挂载rw=可随意写”“root=操作安全”或“Buildroot正在板上构建系统”。
