# 第4章官方资料与板端证据

网络资料访问日期：2026-08-10。

| 主题 | 来源 | 使用边界 |
|---|---|---|
| Linux内核 | [kernel.org: Linux](https://www.kernel.org/linux.html) | 区分内核与完整系统，不把上游文档等同于厂商BSP |
| GNU历史 | [GNU Project history](https://www.gnu.org/gnu/gnu-history.html) | 说明基础工具的历史来源，不进入命名争论 |
| Ubuntu版本 | [Ubuntu release cycle](https://ubuntu.com/about/release-cycle) | 解释YY.MM与LTS；本轮主机身份见 `host-ubuntu-identity-20260810.txt` |
| Buildroot | [Buildroot User Manual](https://buildroot.org/downloads/manual/manual.html) | 说明它在主机生成嵌入式系统，不是板端内核或服务 |
| Linux目录 | [Filesystem Hierarchy Standard 3.0](https://refspecs.linuxfoundation.org/FHS_3.0/fhs/index.html) | 解释通用目录职责；板端实际目录以当前镜像为准 |
| procfs | [Linux 5.10 procfs](https://docs.kernel.org/5.10/filesystems/proc.html) | 说明 `/proc` 是内核信息接口，不是eMMC普通目录 |
| sysfs | [Linux 5.10 sysfs](https://docs.kernel.org/5.10/filesystems/sysfs.html) | 说明 `/sys` 与设备模型关系；条目存在不等于功能通过 |

本章随稿证据：

- `host-ubuntu-identity-20260810.txt`：开发环境的Ubuntu名称、x86_64架构与WSL2内核身份；采集日期为2026-08-10，SHA-256为 `AA5DB2A156304414379ED78CBBFC5892BD3FBFA9A35599E30801A3006BCB47E1`。
- `assets/evidence/terminal-board-identity.png`：本轮板端身份截图，SHA-256为 `91BA8A2D942E804B633F5FC568E827027F1A13849B9BE9C4F1204FDCF9C504B3`。

板端原始记录来自 `tmp/boompi-doc-v5/evidence/board/20260810-dhcp-recovery/`。这些证据只证明采证时刻的观察，不证明BSP可复现、长期稳定或全部硬件功能通过。
