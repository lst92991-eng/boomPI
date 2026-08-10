# 第3章审阅记录

审阅基线：`第03章_操作系统简介_审阅稿_v0.1.docx` / `chapter03-v0.1.pdf`
最终页数：5页
正文结构：5个二级标题
正文图片：3张内嵌教学图，0张浮动图

## 本轮完成内容

- 从第2章“硬件已连接”自然进入“为什么仍需要操作系统”，没有从百科定义或术语表起笔。
- 以“应用怎样使用硬件—多个工作怎样轮流—内存、中断与文件系统—从上电到应用”形成连续因果链。
- 删除虚拟地址、页表、`ioctl`、原子操作、锁和临界区等本章不需要的实现细节；并发同步留到第11章。
- 网络检索结果已落实到版本明确的一手来源：Rockchip RV1106资料、上游Linux 5.10、POSIX、U-Boot、Buildroot与Ubuntu官方资料；正文明确上游5.10不等于厂商5.10.160。
- 图3-1含请求与结果返回方向；图3-2只使用任务A/B/C，不暗示当前项目架构；图3-3重制为紧凑六框横条，避免幻灯片式大标题和尾页空白。
- 本轮板端身份只写成一次观察：Linux 5.10.160、armv7l、Buildroot 2023.02.6和PID 1为init；不外推BSP可复现或全部外设可用。

## 三重审校

- 技术审校：PASS，P0=0、P1=0；报告见 `docs/course/audit/ch03-v01-technical-audit.md`。
- 可读性审校：PASS；怕术语读者的主线、5节粒度、5页分页和章末过渡均通过；报告见 `docs/course/audit/ch03-v01-readability-audit.md`。
- 视觉审校：PASS；3图100%可读、图注同页、无孤页、无隐私信息；报告见 `docs/course/audit/ch03-visual-plan.md`。
- DOCX结构：3个inline图片、0个anchor图片；核心属性未包含本机用户名或本地路径。

## 最终文件指纹

| 文件 | SHA-256 |
|---|---|
| `chapter03.md` | `C0AA483F39807AAF671FC6A1401BE641002C88CB56F3CB95406E9D803A2FA75E` |
| `os-stack.png` | `5BDA8EC5DB17723ABDCD3234E8C5E0C2509B806BD461C72CC4156BAADB6CE726` |
| `scheduler-timeline.png` | `A67DB56BF54EBE093B2ABF8EEFF7AEC7E1B9080A5D25CDBC660C7E268D4A8102` |
| `boot-chain.png` | `187A4C1A8A99E970016AAF505CB5AE310C46C29D0D76AE164358F60FBC8E55C4` |
| `sources/README.md` | `873E4E19B4F9E361F24B51DE5180E68C1A78A7174AB40DD0D60180B061A98245` |
| `第03章_操作系统简介_审阅稿_v0.1.docx` | `E1AEF31DE076D746294F6AAE9303EFDFFABF844F40025D1875FD41D7FFC8AB26` |
| `chapter03-v0.1.pdf` | `50EC6F4BFC46C7B1287FAF2B835943A959FAD7CD201986CAD490E3400691A8DA` |

## 非阻断后续项

- 图3-1、图3-2仍有轻微演示模板感，可在全书最终统一图形风格时一起调整；不阻断本轮逐章审阅。
- 当前章节为独立审阅稿，等待用户反馈，不等同于整书正式发布。
