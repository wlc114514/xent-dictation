# xent-dictation · 单词默写

基于 Project-Xent / Fluxent 的 Windows 原生应用，使用 C23 和 xmake 构建。

## 功能

- 两种题型：释义 → 单词（首字母提示）、单词 → 释义。
- 普通默写：错词进入下一轮，全部答对后完成。
- 无尽模式：每 20 个词一组；答错后显示答案，重开时将本组见过的词打乱排在前面，未见过的词保持原顺序。整组一次答对后进入下一组。
  一个词一次答对（没先提交答错）满 15 次就移出本组，从后面的词里按顺序补一个进来，本组保持 20 个；「不知道」不清零次数，从第 1 组重新开始才清零。
- 默写一轮结束的结果页可以「错词进无尽模式」：这一轮的错词打乱后替换无尽模式的进度（结果页不变，还能接着开下一轮），无尽页可「换回全部词库」。
- 导航栏切换默写、无尽模式、词库和说明；两种练习各自保留进度，存在 dictation_progress.bin，关掉再开接着做。
- 词库支持输入窗口和剪贴板批量导入，自动跳过重复词，清空需确认。
- 录入时自动去掉单词表上的标注（如「忘记 获利丰厚的」→「获利丰厚的」、「少词义 发展 …」），只删跟其他释义不沾边的，forget「忘记」这类不动。
- 一词多义里意思相同的释义自动合并（如「缓解、减轻」→「缓解（减轻）」，写哪个都算答对这一项）：录入新词时自动检查，已有词库在词库页点「合并同义释义」。
- 虚拟化词表列表，按行显示序号、单词、词性与释义，修复长列表挤压重叠。
- 可选本地 JEV 判分，后台线程处理请求。

## 源码结构

| 路径 | 职责 |
|---|---|
| src/dictation.c | 消息处理（update）、后台判分 / 合并线程、加词窗口、入口 |
| src/dict_app.h | 前端模型和消息 |
| src/dict_view.c | 导航和各页面视图 |
| src/dict_session.c / .h | 普通轮次与无尽模式状态机 |
| src/dict_words.c / .h | 词库读写、批量导入 |
| src/dict_text.c / .h | 字面判分、提示和文本工具 |
| src/dict_judge.c / .h | 本地模型判分 |
| tests/test_dict_core.c | 后端测试 |
| samples/ | 数据文件示例（*.sample.json） |
| tools/jev2b_server.py | 2B 判分模型的本地服务 |

## 下载

[Releases](../../releases) 里有打包好的 `xent-dictation-<版本>-x86_64.zip` / `-arm64.zip`：解压后把 samples 里的示例复制到 dictation.exe 旁边、去掉 `.sample` 即可运行。

发版：推 `v*` 标签，`.github/workflows/release.yml` 用 llvm-mingw 构建两种架构，跑后端测试和布局自检，打包上传到 Release。

## 构建

需要 Windows、[llvm-mingw](https://github.com/mstorsjo/llvm-mingw)（在 PATH 上）和 xmake 3.x。

```bat
xmake
```

第一次配置时 xmake 从 [fluxent v0.3.2 发布页](https://github.com/Project-Xent/fluxent/releases/tag/v0.3.2) 下载
`xent-0.3.2-<arch>-windows-gnu.zip`（公开头文件 + 合并的 libxent.a，校验 sha256），不需要再把框架源码放在旁边。
只用 Xent 的公开接口：`flux_run` 装配窗口和运行时，后台线程用 `flux_app_post` 把消息投回 UI 线程。

产物：build/mingw/x86_64/release/dictation.exe。ARM64：`xmake f -a arm64 && xmake`。
离线构建：把发布包 zip 放进某个目录，`xmake f --pkg_searchdirs=<目录>`。

## 数据

使用 dictation_extra.json 保存词表，格式为 [["word","pos","meaning"], ...]。
dictation_judge.json 配置可选的本地判分器。仓库提供 *.sample.json 示例，复制并去掉 .sample 后使用。

判分器有三种（`api` 字段），推荐 `systemone` + vllm-d3-nano：
- 不写：0.8B（Jev-Style-0.8B-Decision-v3），程序拉起 llama-server，过线 0.9。
- `"api": "judge"`：2B（Jev-Style-2B-Decision-v3），更准（尤其不会把「严肃的」判成「严酷的」），约 0.4 秒一次、占内存约 2.3GB。
  2B 要块注意力，llama-server 跑不了，要用作者的打分程序 jev-score-v2（对着 llama.cpp 441df11f 编译）和官方 Python 运行时；
  `tools/jev2b_server.py` 把它包成本地服务（`POST /judge`），放到模型目录里，按 `dictation_judge.2b.sample.json` 配置。
  服务把 2B 的原始概率在对数几率上平移，原始 0.55 映射成 0.9（`--pass-at`），所以过线和 0.8B 一样写 0.9；映射是单调的，等于原始分过线 0.55（在 170 组标注释义对上扫出来的）。
  示例配置把 2B 的过线降到 0.8（约等于原始分 0.35）：0.9 会把「精神/灵魂」「有害的/不利的」「身份/同一性」这类同义词判错（0.85–0.89），而「严肃的/严酷的」「合法的/合理的」在 0.78–0.80，0.8 仍能挡住。
  程序退出时连同服务和打分程序一起结束（作业对象）。
- `"api": "systemone"`：llama.cpp 新版（b11361 起）`/v1/systemone` 接口上的决策模型，程序拉起 llama-server（`-ngl 99`，有显卡就用）。
  推荐 [vllm-d3-nano](https://huggingface.co/ggml-org/vllm-d3-nano-GGUF)（Qwen3.5-2B，Q8_0 2.55GB），按 `dictation_judge.systemone.sample.json` 配置，过线 0.65（原始概率，0.6–0.7 都差不多）。
  87 组中文释义对（42 组应判对、45 组应判错，含形近词、反义词）上各取最佳阈值的平衡准确率：
  vllm-d3-nano 0.965、d1-3B 0.931、2B 0.920、lev 0.897、0.8B 0.839、Laya 0.667。
  vllm-d3-nano 能放过「错觉/幻觉」（两个方向 0.83/0.97），又挡住「严肃的/严酷的」（0.55/0.30）；0.8B 和 2B 只能顾一头。RTX 5060 上约 25ms 一次，纯 CPU 约 0.25 秒。

答案里写了几项（、，; 隔开）时逐项判，一项只顶一个义项。

合并同义释义（移植自 genuineedu 网页版）：同一个词的释义两两让判分模型双向打分，都大于 `merge_threshold` 才合并，宁可不合并也不错合并。
只有 `systemone` 后端默认开启（0.98）；其它后端没测过，默认不合并，可在 dictation_judge.json 里写 `merge_threshold` 打开。
在 genuineedu 的 268 对标注释义上（148 对同义、120 对不同义），vllm-d3-nano 双向 > 0.98：错合并 3/120（重要的/主要的、普通的/普遍的、到达/达到），同义合并 137/148；
网页版的「词义联想（CC-CEDICT）+ 模型 > 0.9」换成 nano 是错合并 3、同义合并 104，所以这里不用词义联想。

一词多义默认要把每个意思都写出来；练习页右上角取消勾选「多义词全答」后，答出任意一个意思就算对。开关保存在 dictation_settings.json。
本地词库与判分配置不纳入版本控制。

本地开发用 `xmake run dictation`，数据文件放在仓库根目录（已在 .gitignore 里）。

## 检查

```bat
xmake test
xmake build test_dict_layout
set STAGE=5 && xmake run test_dict_layout
```

布局检查只测量页面内容；导航外壳和答题框（AutoSuggestBox）需要真实窗口，自检里答题框换成同尺寸的 TextBox。

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
