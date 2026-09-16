# 非音频第一轮压缩：实测结果

生产实现提交：`db451d74affe7444f798eb2b72ef2f27f4c8fdda`。本结果记录只新增文档，不再改变生产源码。分支`codex/minimal-non-audio`，原音频验收基线`2ca0cf63d241a032292f4d83a3120002b91a066e`。

## 实际代码量

统计由该实现提交的teaching-ui CI产生，下载同次CI的git archive源码后又独立执行measure_client.py，数值一致。范围为板端入口、src/include中的自有生产C/C++，含头文件/内联/驱动；ELOC是去掉纯注释和空行后的物理代码行，括号和预处理仍计入。

| 模块 | 修改前ELOC | 修改后ELOC |
| --- | ---: | ---: |
| 音频及相关头文件（冻结） | 1175 | 1175 |
| 入口与CLI | 52 | 41 |
| 问答应用 | 226 | 225 |
| 配置与设备身份 | 154 | 112 |
| 网卡准备、发现与配置 | 282 | 226 |
| WSS与协议 | 573 | 596 |
| LVGL页面 | 418 | 208 |
| UI运行与显示交接 | 418 | 257 |
| 显示触摸端口 | 347 | 347 |
| 摄像头预览 | 251 | 149 |
| 非音频小计 | 2721 | 2161 |
| 板端合计 | 3896 | 3336 |

总共41个生产文件，5031→3902物理行，3896→3336 ELOC；净减560有效行（总量14.37%、非音频20.58%）。测试、Go、脚本、模拟器、资源和vendor单列，不能说整个Git仓库只有3336行。

此前2960–3305 ELOC预算未全部达到：实际比上限多31行。显示触摸驱动未为配额修改，WSS为实际接口绑定/失败回退增加23 ELOC；UI补齐初始化和释放边界后合计465 ELOC。没有压行或移动代码凑数。

运行管理脚本另计：S99、clientctl和两个AP配网脚本由707→197物理行。删除AP/门户是已确认的功能范围缩减；脚本删减不重复算入C/C++节省。

复算：`python3 scripts/measure_teaching.py --before 2ca0cf63d241a032292f4d83a3120002b91a066e --after db451d74affe7444f798eb2b72ef2f27f4c8fdda`。

## 已执行验证

实现SHA的ci run `35060469920`：Ubuntu、Windows、macOS全部成功；包括各自C++ Host/CTest、Python、Go test/vet/build，Linux额外的Go race和真实C++/Go TLS/WSS联测成功。

teaching-ui run `35060469861`全部成功：实际LVGL8.2/FreeType页面、SDL和Linux端口编译，固定音频/服务端/协议/vendor/resource diff为空，实际页面事件与反复切换、UI运行生命周期、摄像头管线/子进程、独立netns接口选择/绑定测试，以及ASan/UBSan通过。未关闭泄漏检测，未修改第三方库。

摄像头测试只替换外部execl工具；UI运行测试替换硬件/页面内容，页面本身另外用实际LVGL测试；netns使用dummy网卡。因此没有把替身/Host结果称为SC3336、触摸、实际Wi-Fi关联或声学验收。

本地15项Python脚本回归和协议fixture通过；脚本测试在临时目录验证真实clientctl的启动、残留child清理和失败更新回滚。实际界面截图由同一CI中的生产page代码渲染，不是设计示意图。

## 尚未验证及使用顺序

未交叉构建ARM ELF：独立RV1106配置因本环境没有匹配SDK/toolchain路径停止。未部署、未连接开发板、未调用收费云端。仍须在用户既有SDK上编译并检查实际显示、触摸、网卡、摄像头与声音。

原分支保持2ca0cf6，先完成音频测试，再切换本候选进行非音频验收。新候选已经包含原紧凑插话代码，无需再次搬运音频补丁。

升级之前先用旧版boompi-clientctl stop停止旧客户端/AP，再安装新的二进制和启动脚本；保留Wi-Fi配置、server.conf和电脑端稳定身份。新版本不再管理旧AP进程，不能覆盖后才指望新脚本替旧版清理。

功能/接口取舍详见[minimal-non-audio.md](minimal-non-audio.md)；对应数字见[机器可读记录](non-audio-results.json)。
