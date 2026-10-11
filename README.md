# xent-dictation · 单词默写

基于 Project-Xent / Fluxent 的 Windows 原生应用，使用 C23 和 xmake 构建。

## 功能

- 两种题型：释义 → 单词（首字母提示）、单词 → 释义。
- 普通默写：错词进入下一轮，全部答对后完成。
- 无尽模式：每 20 个词一组；答错后显示答案，重开时将本组见过的词打乱排在前面，未见过的词保持原顺序。整组一次答对后进入下一组。
- 导航栏切换默写、无尽模式、词库和说明；两种练习分别保存本次运行中的进度。
- 词库支持输入窗口和剪贴板批量导入，自动跳过重复词，清空需确认。
- 虚拟化词表列表，按行显示序号、单词、词性与释义，修复长列表挤压重叠。
- 可选本地 JEV 判分，后台线程处理请求。

## 源码结构

| 文件 | 职责 |
|---|---|
| dictation.c | 消息处理、窗口和入口 |
| dict_app.h | 前端模型和消息 |
| dict_view.c | 导航和各页面视图 |
| dict_session.c / .h | 普通轮次与无尽模式状态机 |
| dict_words.c / .h | 词库读写、批量导入 |
| dict_text.c / .h | 字面判分、提示和文本工具 |
| dict_judge.c / .h | 本地模型判分 |
| tests/test_dict_core.c | 后端测试 |

## 构建

需要 Windows、Visual Studio 2022 C++ 桌面开发工具和 xmake 3.x。
将 [fluxent](https://github.com/Project-Xent/fluxent)、[xent-kit](https://github.com/Project-Xent/xent-kit)、[xent-core](https://github.com/Project-Xent/xent-core)、[cwinrt](https://github.com/Project-Xent/cwinrt) 放在同级目录。

1. 将本仓库的 examples/dictation 整个目录复制到 fluxent/examples/dictation。
2. 将 xmake-targets.lua 中的三个目标追加到 fluxent/xmake.lua；已有同名目标时替换。
3. 在 fluxent 目录运行：

```bat
xmake build dictation
```

产物：fluxent/build/windows/x64/release/dictation.exe。
应用使用 Fluxent 内部运行时接口，构建目标需要 include、src、src/bridge 头文件路径；框架升级可能需要调整这些接口。

## 数据

使用 dictation_extra.json 保存词表，格式为 [["word","pos","meaning"], ...]。
dictation_judge.json 配置可选的本地判分器。仓库提供 *.sample.json 示例，复制并去掉 .sample 后使用。
本地词库与判分配置不纳入版本控制。

run-dictation.bat 是启动脚本示例，修改其中 exe 路径后使用。

## 检查

在 fluxent 目录运行：

```bat
xmake build test_dict_core
xmake run test_dict_core
xmake build test_dict_layout
xmake run test_dict_layout
```

布局检查只测量页面内容；导航外壳需要真实窗口后端。

| 环境变量 | 页面 |
|---|---|
| STAGE=5 | 普通默写 |
| STAGE=8 | 词库 |
| STAGE=12 | 错词预览 |
| STAGE=13 | 无尽模式答错后 |
| STAGE=14 | 说明 |
| STAGE=15 | 完成 |
| DICT_START_PAGE=1 | 启动真实窗口时直接进入无尽模式 |
| DICT_START_PAGE=3 | 直接进入词库 |
| DICT_START_PAGE=4 | 直接进入说明 |

## 许可

MIT，见 LICENSE。框架版权归 Project-Xent 各仓库所有。
