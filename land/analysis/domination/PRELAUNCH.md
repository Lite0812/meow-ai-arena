# 最终评测启动前参数记录

参数与源码于 **2026-10-19T07:36:31.057224+00:00** 冻结。本记录在生成或查看验收种子之前写入。
完整机器可读参数见 [FINAL_LAUNCH.json](FINAL_LAUNCH.json)。

## 冻结源码

- 源码提交：`619dfa45ac298952527f2e933f797f188fa5712e`
- 提交路径：`land/workspace/submission/bot.cpp`
- 实际评测输入：`land/analysis/domination/frozen/final.cpp`，与提交文件逐字节相同。
- SHA256：`e733d847add9fa443a6c8a492c6162f3df41b033b5474a0c1732f15a2ab25224`
- benchmark SHA256：`d9b9c558481ba095d34765ca646c1413aa0ca56ae2307015aa77516af0e91cd7`
- 对应验证：`verification/tp_patched_report.json`、`tp_patched_mechanics_report.json`、`tp_patched_endgame_report.json`，均通过且记录同一源码指纹。

源码在开发提交 `7051de34` 的基础上仅增加已复现的两步拦截修复；该版自身尚无整场成绩。
389 版的 200 场开发成绩不得移作本源码的独立验收结果。

## 固定比赛计划

| 项目 | 单挑 | 四人混战 |
| --- | --- | --- |
| 顺序 | 先运行 | 单挑完整结束后运行 |
| 对手（固定顺序） | sonnet community sparring opus astra | sonnet community astra |
| 地图数 | 每敌 50 | 20 |
| 座位 | 每图交换两座 | 每图轮换四座 |
| 总场次 | 每敌 100，共 500 | 80 |
| split / seed-base | acceptance / 0 | acceptance / 0 |
| jobs | 1 | 1 |
| replay-maps | 50，保存全部 500 场 | 20，保存全部 80 场 |
| 输出目录后缀 | final_acceptance_duel_e733d847 | final_acceptance_melee_e733d847 |

输出均在 `land/analysis/domination/runs/` 下。起跑前两目录均不存在。
四人固定上述桌面顺序的四个循环座位，并非全部对手排列；两种模式的成绩不合并。
同盐值两种模式的地图也不作为额外的独立单挑样本。

## 执行与判定

全队已确认其他整场评测、编译和持续 CPU 探针停止；仅本轮一个 benchmark，`--jobs 1`。
使用仓库原版规则、裁判、对手与构建器：`-O2 -std=c++20`，600 回合，普通步 50 ms，
初始化 2000 ms，官方宽限 15 ms，10 次错误判负，512 MiB。
运行中不改源码、对手、地图数、盐值或终止条件；异常保留，不删除、不静默重赛。

每名强敌分别要求 **超过 80%** 的严格胜率，平局不算胜，总平均不能代替逐敌结果。
原有 `criterion` 字段保持含义：`target_met` 为完整全场次观察胜率达标；
`acceptance_empirical_pass` 另含零异常门槛；`acceptance_conservative_evidence` 还要求地图全胜区间下界。
对手异常胜局全作非胜的敏感性下限、置信区间与这三个字段分开报告，不改门槛。

## 固定命令

从仓库根目录依次运行：

```bash
python3 land/analysis/domination/benchmark.py \
  --bot land/analysis/domination/frozen/final.cpp --name candidate \
  --mode duel --opponents sonnet community sparring opus astra \
  --maps 50 --split acceptance --seed-base 0 --jobs 1 --replay-maps 50 \
  --out land/analysis/domination/runs/final_acceptance_duel_e733d847

python3 land/analysis/domination/benchmark.py \
  --bot land/analysis/domination/frozen/final.cpp --name candidate \
  --mode melee --opponents sonnet community astra \
  --maps 20 --split acceptance --seed-base 0 --jobs 1 --replay-maps 20 \
  --out land/analysis/domination/runs/final_acceptance_melee_e733d847
```

## 结束后清点

`verify_run.py` 应分别以 `--expected-games 500` 与 `80`、上面的 `--source-sha256`、
以及 `--require-all-replays` 核对。将其 `parameters` 与本记录逐项对照，核验源码、二进制、
引擎、场数、座位覆盖、全部异常与回放，以及汇总重算一致性。
`inventory_ok` 只表示材料完整，不表示胜率达标；提前判负完整回放单列。
开始、结束时间与正式成绩随后写入 [REPORT.md](../../workspace/submission/REPORT.md)，本启动前参数记录保留。
