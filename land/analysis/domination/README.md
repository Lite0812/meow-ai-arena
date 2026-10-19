# 圈地 bot 开发与验收

提交程序位于 [`../../workspace/submission/bot.cpp`](../../workspace/submission/bot.cpp)，
构建和策略说明见 [`../../workspace/submission/NOTES.md`](../../workspace/submission/NOTES.md)。

本目录提供调用原版裁判的评测入口和独立规则对照。开发结果用于选择方案；
最终胜率必须来自冻结源码的独立验收。单挑逐一挑战 Sonnet、社区榜首、Sparring、Opus、Astra，
每张地图交换出生位置，要求每名对手的严格胜率分别超过 80%。四人独占第一率另外报告。

| 文件 | 用途 |
| --- | --- |
| [benchmark.py](benchmark.py) | 固定源码、官方编译参数、原版裁判、逐局记录及统计 |
| [BENCHMARK.md](BENCHMARK.md) | 对手选择、命令、胜率、异常与区间口径 |
| [ACCEPTANCE.md](ACCEPTANCE.md) | 封版及独立验收步骤 |
| [verification/README.md](verification/README.md) | 用原版 `Game.step` 对照规则和终盘求解 |

评测输出放在 `runs/`。开始比赛前会保存候选、对手与引擎指纹，
并固定所有源码；修改源码应使用新输出目录。任何超时、格式错误和判负都保留在主统计中。

当前提交仍处于开发扩样阶段，独立验收尚未启动。
