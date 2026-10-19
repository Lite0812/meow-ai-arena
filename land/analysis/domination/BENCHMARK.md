# 圈地 bot 评测口径

`benchmark.py` 直接调用仓库原有 `land/workspace/engine/referee.py` 的
`run_match`。它不替换裁判、不简化规则、不缩短回合数、不修改对手时限。

## 强对手集合

默认逐一挑战下面五个现有 bot。依据是仓库随附正式赛和社区结果，而不是临时挑选容易击败的对手。

| 别名 | 原始文件 | 选择依据 |
|---|---|---|
| `community` | `community/land/mohen2006__land-gpt-61-sol/bot.cpp` | 社区已提交 bot，随附快测排名第一 |
| `sonnet` | `land/bots/sonnet-5@claude-code#1/bot.cpp` | 正式赛总分、单挑和混战均第一 |
| `sparring` | `land/tournament/sparring.cpp` | 正式赛单挑排名第二的出题方陪练 |
| `opus` | `land/bots/opus-5.5@claude-code#1/bot.cpp` | 正式赛单挑第三，模型选手第二 |
| `astra` | `land/bots/gpt-6-astra@codex#1/bot.cpp` | 正式赛总分第二、混战第二 |

依据文件：`land/results/ratings.json`、`land/results/ratings.md`、
`community/results/land/mohen2006__land-gpt-61-sol.json`。
随附评分使用各自赛事口径，不能把社区分数与旧赛事 0–100 分数直接比较。

## 开发与验收

以下命令从仓库根目录运行。每张单挑地图两局，双方交换出生位置。

```bash
# 查看可用对手及其实际文件
python land/analysis/domination/benchmark.py --list-opponents

# 开发：两名强对手，每个对手 6 张图、12 局
python land/analysis/domination/benchmark.py \
  --bot land/analysis/domination/candidate.cpp \
  --opponents sonnet sparring --maps 6 --split dev --jobs 1 \
  --out land/analysis/domination/runs/dev-001

# 复现某些开发地图，并保存前两张图所有座位的回放
python land/analysis/domination/benchmark.py \
  --bot land/analysis/domination/candidate.cpp \
  --opponents community sonnet --seeds 72219,72220,72221 \
  --replay-maps 2 --jobs 1 \
  --out land/analysis/domination/runs/dev-replay-001

# 固定候选以后，再打开独立验收集：五个对手各 200 局，共 1000 局
python land/analysis/domination/benchmark.py \
  --bot land/analysis/domination/candidate.cpp \
  --maps 100 --split acceptance --jobs 1 \
  --replay-maps 100 \
  --out land/analysis/domination/runs/acceptance-001

# 四人混战：同一桌三名强对手，每张图轮换四个座位
python land/analysis/domination/benchmark.py \
  --bot land/analysis/domination/candidate.cpp \
  --mode melee --opponents community sonnet astra \
  --maps 20 --split dev --jobs 1 \
  --out land/analysis/domination/runs/melee-dev-001

# 添加任意其他候选作对手；只写 --opponent 时不自动加入五名默认对手
python land/analysis/domination/benchmark.py \
  --bot candidate.cpp --opponent previous=previous.cpp \
  --maps 6 --out land/analysis/domination/runs/regression-001

# 从已保留的逐局记录重新计算汇总，不编译、不增加或重打比赛
python land/analysis/domination/benchmark.py \
  --summarize land/analysis/domination/runs/dev-001
```

默认种子由固定的伪随机算法产生，`--seed-base` 是种子套件的盐值。
开发种子位于 `[1000000, 1000000000)`，验收种子位于
`[1000000000, 2000000000)`，两者不会重叠。`--seeds` 仅用于开发。
验收集不能用于挑参数或筛候选；程序会记录划分，但不能替代开发者遵守这一信息隔离。

同一命令、同一输出目录可以续跑。源代码、编译结果、对手、引擎、地图、并发数或回放设置发生变化时，
必须使用新的输出目录，旧结果不得覆盖或混入新候选结果。开始对局前会把所有 bot 复制为固定版本再编译，
因此开发者在另一处继续编辑候选，不会改变已经开始的一轮比赛。

地图生成和规则本身确定，但部分原版 bot 按墙上时间限制搜索次数。
同样的种子、座位与源代码，在没有超时或错误时也可能产生不同比分。
固定版本、种子和并发数保证的是可复核的实验条件，不承诺逐步动作完全一致。
这也是需要多个独立地图、保留所有结果而不能挑选某次复跑比分的原因。

## 胜率与验收含义

- **严格胜率 = 独占第一的局数 / 全部局数。** 平局不计胜；同时列出胜 / 平 / 负。
- 单挑“对强对手超过 80%”按**每名默认强对手分别超过 80%**判断，不用弱对手拉高总平均。
- 混战的胜利指四人中独占第一；并列第一算平局，其余算负。混战结论与单挑分开报告。
- 原裁判的超时、格式错误、崩溃和判负全部保留；主统计包含全部已完成比赛。不会静默删局或自动重赛。
- `target_met` 单独回答观察到的游戏胜率是否达标：本轮全部完成，且每一对阵严格胜率都超过 80%。它不附加零异常条件，也不改变开发集 / 验收集划分。
- `acceptance_empirical_pass` 要求验收集全部完成、每一对阵严格胜率超过 80%，并且所有参赛程序没有任何超时、错误或判负。
- `clean_*` 是诊断统计，不能代替包含全部比赛的主结果。

`opponent_fault_games` 记录至少一名对手出现过异常的局数，
`opponent_fault_wins` 记录其中候选获胜的局数。同一局出现多次异常只算一局。
`opponent_fault_win_rate_floor = (总胜局 - opponent_fault_wins) / 全部局数`
将这类胜局全部视作非胜，保留完整分母；候选自己的异常和原裁判结果也照常保留。
这是对对手异常影响的保守敏感性分析，不是统计置信下界，不推断异常由系统调度造成，
也不预测把这些比赛重打后的真实比分。
`target_met_if_opponent_fault_wins_count_as_nonwins` 要求这一敏感性下限在每一对阵仍然超过 80%。
新字段与原有验收门槛独立：即使 `target_met` 为真，
只要出现过异常，现有 `acceptance_empirical_pass` 仍为假。

报告的 `strict_win_wilson_ci95` 为按局计算的双侧 95% Wilson 区间。
相同地图的换座结果可能相关，因此该区间只作为描述，不能假定两局是两个独立地图样本。
另有按地图统计的“双座全胜”（混战为四座全胜）Wilson 区间：每张图只算一次，
全座获胜事件的概率是平均逐局胜率的下界。这是更保守的证据。
整体总表也只把一个 seed 算作一张独立地图：该 seed 的所有对手和座位全部获胜才算整体地图全胜，
不会将同一个 seed 的多个对手对阵伪装为独立地图。
`acceptance_conservative_evidence` 还要求每一对阵的地图全胜概率区间下界超过 80%。
这些区间逐对阵给出，未宣称对所有对阵同时成立的家族置信保证。

样本较少时，即使观察到 100% 胜率，区间仍然可能很宽。开发烟测不等于验收通过。
`--split acceptance` 也不自动证明某候选达到目标；应读取 `summary.json` 中的实际数据和验收字段。

## 裁判与资源

比赛参数来自原版裁判：单挑 40×40，混战 48×48，600 回合，普通回合 50 ms，
第零回合 2000 ms，裁判另加 15 ms 宽限；10 次错误会判负；内存地址空间上限 512 MiB。
C++ 使用官方 `-O2 -std=c++20` 参数（Windows 另有官方 `-static`）。

时限按墙上时间判断。并发太高会让正常对手受到系统调度影响，不能用这种结果宣传优势。
先用 `--jobs 1` 开发；最终验收全队只运行一个 benchmark，使用 `--jobs 1`，
其间暂停其他评测、编译和持续 CPU 探针。
如出现异常，保留原数据并查明原因；重新评测必须使用新目录，并报告重测原因。

封版和最终验收的简短执行清单见 `ACCEPTANCE.md`。

当前环境单并发实测一次完整社区 bot 对 Sonnet 的 600 回合比赛约 2.95 秒；
这是吞吐估计，不保证不同候选或系统负载下的速度。实际 CPU 配额未向该环境暴露，
不能用 `os.cpu_count()` 的逻辑核数代替资源保证。

## 输出文件

| 文件 / 目录 | 内容 |
|---|---|
| `manifest.json` | 原始源文件、源代码与可执行文件 SHA256、引擎 SHA256、全量种子、参数和运行环境 |
| `games.jsonl` | 每局原版裁判结果，加候选座位、严格胜负、耗时和版本指纹 |
| `summary.json` | 整体与逐对手胜 / 平 / 负、严格胜率、区间、异常、面积和验收判断 |
| `sources/` | 对局前冻结的所有参赛程序 |
| `build/` | 官方参数编译的程序 |
| `stderr/` | 每局每个座位的程序标准错误日志 |
| `replays/` | 使用 `--replay-maps` 选取的原版回放 |

回放由 `land/workspace/viewer/viewer.html` 打开。
