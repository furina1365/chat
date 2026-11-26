---
name: implement-and-document
description: 在 chat 项目内按 TDD 与因素矩阵把功能、修复或重构做到验收，交付一份供用户审核和复盘的大白话文档，以及一份供 AI 执行和复查的文档。
---

# 从方案到交付

1. **调查代码与列因素矩阵**。按 [trace-change-code](../trace-change-code/SKILL.md) 沿真实入口定位代码和基线，主动挖出代码里藏着、需求里没写的影响因素（如并发、重排、背压、半关闭、双后端差异等），列出因素测试矩阵并写入 `<主题短名>-execution.md`。这一步每次必做，不能因为改动看着简单就省掉。
2. **动代码前写用户审核文档**。按 [plan-change](../plan-change/SKILL.md) 在 `<主题短名>.md` 用大白话写清这块业务是干嘛的、谁在用、会影响谁，以及跟本次有关的那段链路原来怎么走、打算改成怎么走。把这些要点同步给用户再动手。篇幅按改动大小来，无关的不写。
3. **按因素矩阵做 TDD 实施与验收**。按 [execute-change](../execute-change/SKILL.md) 严格按矩阵逐项先红后绿实施并跑验收，把实际命令、观察结果和动手时的返工/改道补进同一份 `<主题短名>-execution.md`。
4. **改完后更新用户审核文档**。回到 `<主题短名>.md`，对齐“原来怎么走 → 实际改成怎么走”；如果动手时跟原来说的不一样（返工、换做法），必须写清改了什么、为什么改。补上简洁的实测变化和未测边界，两份文档用相对链接关联，交用户审核合并。

用户单独要求某一阶段时，调用对应技能。只做方案时，实测状态按已知事实填写，不提前宣称通过。

## 项目内的技能副本

在 AI 执行文档末尾列出本次实际应用的技能及仓库内链接。项目内技能直接引用各自的 `SKILL.md`。来自项目外的技能，将实际使用的 `SKILL.md` 及其所需的引用文件、脚本复制到本技能的 `references/skills/<技能名>/`。已有相同副本时直接复用。执行前优先使用项目内副本。

- 每次实施都用 [tdd](references/skills/tdd/SKILL.md) 驱动因素测试矩阵（先红后绿，不因改动小省略）。
- 缺陷调查用 [systematic-debugging](references/skills/systematic-debugging/SKILL.md)。
- 完成前核对实测证据用 [verification-before-completion](references/skills/verification-before-completion/SKILL.md)。
- 维护本项目技能时用 [skill-creator](references/skills/skill-creator/SKILL.md)。
