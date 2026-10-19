# 穹界喵

单文件 C++20 圈地 bot，同一份程序支持单挑（N2）和四人混战（N4）。作者与 AI 实现信息见 [meta.json](meta.json)。本文对应 `bot.cpp` 的 SHA256：

```text
e733d847add9fa443a6c8a492c6162f3df41b033b5474a0c1732f15a2ab25224
```

## 策略

两种模式共用闭环搜索、精确圈地收益计算、回家距离和轨迹拦截。用行位图与边界泛洪计算闭合面积，遵守出生保护区规则；比较方案时也考虑领地被对方短闭环重新夺回的风险。每回合根据真实状态修订计划，计划失效时重新选择动作。

单挑增加 **prefix 返航搜索**：先走有限组折线路径，再寻找能够闭合的回程，补充固定矩形难以覆盖的不规则形状。返航搜索会跳过预测中将在到达前被敌方圈走的归家终点，继续寻找其他终点；不会把这些当前己方格当成可穿越的中间节点。敌方未来闭环仍是预测，因此这项检查不能保证任意后续局面都安全。

**复活拦截（TP）** 利用规则允许的自撞轨迹：死亡清除轨迹、保留已有领地，并按真实出生点和初始朝向复活，再尝试切断敌线。自杀路径模拟包含途中归家和轨迹重置；双人复活追杀中，如果自己无线、第一步仍在己方领地、对手至少两步才能回家且下一步能切旧线，则允许第一步存在撞头风险：撞头会同时消灭对手，未撞则继续两步切线。其余拦截收益、敌方返航时机及执行路线仍可能判断失准，不宣称全程保底。

**一步归家救场（oneplyhome）** 针对可能撞头而被普通避碰规则拒绝的归家动作，枚举敌方当步合法回应，比较同时结算后的最坏面积差，仅在满足条件且严格改善时替换动作。单挑最后八步以内另尝试纯策略 maximin：对双方合法行动作完整深度求解，使用 alpha-beta 剪枝；到达时间预算则保留进入搜索前的动作，不让未完成的搜索结果覆盖原策略。其保证值依赖后续继续执行相应策略，未来超时回退可能打破这一条件。

四人模式采用 **firstplace** 目标：按敌方面积的 softmax 分配竞争权重，让扩张与拦截共同关注最可能夺冠的对手，而非只改善相邻名次。混战成绩须单独评估。

## 构建与复现

需要 Python 3.10+、支持 C++20 的 `g++` 及本仓库原版引擎；没有第三方 Python 包依赖。bot 本身只用标准库、单线程，通过标准输入输出通信。以下命令均从仓库根目录运行。

```bash
g++ -O2 -std=c++20 land/workspace/submission/bot.cpp -o land/workspace/submission/bot
```

评测也可直接传入 `.cpp`，由官方构建器编译；Windows 构建器另加官方 `-static` 参数。开发复测示例：

```bash
python3 land/analysis/domination/benchmark.py \
  --bot land/workspace/submission/bot.cpp \
  --opponents sonnet community sparring opus astra \
  --maps 6 --split dev --jobs 1 --replay-maps 6 \
  --out land/analysis/domination/runs/submission-dev-001

python3 land/analysis/domination/benchmark.py \
  --bot land/workspace/submission/bot.cpp \
  --mode melee --opponents sonnet community astra \
  --maps 4 --split dev --jobs 1 --replay-maps 4 \
  --out land/analysis/domination/runs/submission-melee-dev-001
```

每张单挑地图交换两座，混战轮换四座；修改源码后须使用新输出目录。逐局记录、源码指纹、异常及统计口径见 [BENCHMARK.md](../../analysis/domination/BENCHMARK.md)，封版与独立验收流程见 [ACCEPTANCE.md](../../analysis/domination/ACCEPTANCE.md)。验收时仅运行一个 `--jobs 1` 评测，并避免其他重负载。

## 规则验证与强度状态

```bash
python3 land/analysis/domination/verification/verify_mechanics.py
python3 land/analysis/domination/verification/verify_endgame.py
python3 land/analysis/domination/verification/verify_tp_approach.py
```

三条命令默认检查本目录的 `bot.cpp`，用原版 `Game.step` 对照冻结状态，覆盖同时结算、自杀复活、一步救场，终盘小深度穷举和超时回退，以及两步拦截的真实状态与禁用条件；报告记录实际源码指纹。依赖、覆盖范围和自定义路径见 [verification/README.md](../../analysis/domination/verification/README.md)。规则验证不等于整场强度或时限验证。

**源码已冻结，独立验收尚未执行；正式成绩见后续 REPORT.md。** 达标要求对每名强敌分别取得超过 80% 的严格胜率，平局不算胜，超时与判负保留；旧候选的开发胜率不能作为本版本成绩。墙上时间预算会受机器负载影响，相同种子也不保证逐步动作一致。

## 来源与许可

本实现基于 mohen2006 的 [墨痕喵](../../../community/land/mohen2006__land-gpt-61-sol/NOTES.md)，并参考仓库中的 Sonnet、Astra、Opus bot；来源已列入 `meta.json` 的 `based_on`。沿用仓库 [MIT License](../../../LICENSE)；再分发本程序或实质性代码时须保留上游版权与许可声明，并保留上述来源归属。
