# 最终验收执行清单

这是执行方案，尚未启动验收，也没有读取或生成保留种子。

1. 负责人先选定最终统一源码，复制到专用冻结路径，记录完整 SHA256；单挑和四人模式如需分别报告，应使用同一份源码。开发版本的成绩不合并到最终验收。
2. 起跑前确定地图数、五名对手、`seed-base`、输出目录和判定口径；运行中不根据胜率换候选、改样本量或挑地图。以下默认 50 张地图，每名对手换座 100 局，共 500 局；若预先选 100 张图，则将两处 `50` 同时改为 `100`。
3. 全队确认只有这一轮 benchmark 在运行，使用 `--jobs 1`，不并发其他评测、编译或持续 CPU 探针。裁判、规则、对手源码和时限使用仓库原版。
4. benchmark 会在起跑前冻结所有源码并记录源码、可执行文件、引擎哈希。检查候选快照哈希与封版记录相同；运行结束核对预期局数、各地图两个座位、逐局记录和全部回放完整。
5. 每名对手分别报告严格胜 / 平 / 负、胜率、区间及双方异常；平局不计胜，全部异常局保留。目标始终是每名强敌胜率 **严格超过 80%**，不是五个对手合计平均超过 80%。四人独占第一率另测另报。

以下是待负责人设置冻结源码和全新输出目录后执行的 CLI，本文未执行此命令：

```bash
arena_frozen_source=land/analysis/domination/frozen/final.cpp
arena_acceptance_out=land/analysis/domination/runs/final_acceptance_duel
sha256sum "$arena_frozen_source"
python land/analysis/domination/benchmark.py \
  --bot "$arena_frozen_source" \
  --opponents sonnet community sparring opus astra \
  --maps 50 --split acceptance --seed-base 0 --jobs 1 \
  --replay-maps 50 --out "$arena_acceptance_out"
```

统计字段不作修改：`target_met` 表示全部对阵观察胜率是否达标；
`acceptance_empirical_pass` 仍保留现有的零异常要求；
`acceptance_conservative_evidence` 仍保留现有地图全胜置信下界要求。
区间、对手异常胜局全作非胜的敏感性下限以及旧验收字段分列，不能混称为同一个结论。
本清单不增加新的成绩门槛，也不把异常局剔除后重新计算验收胜率。

若验收暴露问题，保留本轮完整结果和原因。修改后的候选先回开发集验证，
再预先确定新的独立保留套件；已看过的验收结果不能再次宣称为未见数据。
