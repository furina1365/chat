---
name: execute-change
description: 根据 chat 项目的 AI 执行文档按 TDD 因素矩阵逐项实施代码并跑验收，如实记录方案偏差、返工原因与实际证据。用于执行方案、开发修复及复查验收。
---

# 按因素矩阵 TDD 实施与记录

1. **核对基线与因素矩阵**。打开 `<主题短名>-execution.md` 读取调用链、因素测试矩阵和改动落点，结合用户要求或审核文档确认目标。若还没有因素矩阵或缺少隐藏因素排查，先用 [trace-change-code](../trace-change-code/SKILL.md) 补齐再动代码。
2. **强制按矩阵做 TDD（红 → 绿）**。按项目内的 [tdd](../implement-and-document/references/skills/tdd/SKILL.md) 逐项推进因素矩阵，不能因为改动看着简单就跳过：先在公开边界或真实入口为当前因素写出失败测试/复现检查并确认它真实失败（红），再写最小改动让它通过（绿），逐行收口整个矩阵。遇到意外失败用 [systematic-debugging](../implement-and-document/references/skills/systematic-debugging/SKILL.md) 追根因。
3. **实时记录返工与偏差**。动手过程中只要跟原方案不一样（返工、换做法、多改或少改了路径、新挖出隐藏因素），立即补进因素矩阵，并在 `<主题短名>-execution.md` 的「方案偏差与返工记录」写清**原方案、实际改了什么、为什么改**；同时同步回用户审核文档 `<主题短名>.md`，确保复盘信息不丢。
4. **验证与留证**。按 [verification-before-completion](../implement-and-document/references/skills/verification-before-completion/SKILL.md) 跑完完整构建与矩阵验收，按 [AI 执行模板](references/execution-template.md) 在 `<主题短名>-execution.md` 逐项填入版本、命令、配置、观察结果与红→绿状态。未测、阻塞或缺少自动化覆盖的项如实标明原因，不伪造通过。
5. **同步审核文档与技能清点**。按 [审核写法](../plan-change/SKILL.md) 回填 `<主题短名>.md` 的最终链路、返工说明与验证边界；在执行文档末尾清点实际使用的技能，按 [项目内副本规则](../implement-and-document/SKILL.md#项目内的技能副本) 留存副本并链接。

## chat 项目的验证入口

- 前端 TypeScript 变更从 `frontend` 运行 `npm run build`，核对生成资源。
- 后端用 CMake 构建并走实际入口。共享网络层、HTTP 或 SSE 变更涉及 Reactor 和 Proactor 时，分别构建和运行受影响的用例。
- 涉及登录、存储或模型交互时，使用隔离的 MySQL、Redis、SMTP 或模拟模型上游。记录实际依赖、配置和覆盖场景。证据只保留脱敏数据。[测试手册](../../../docs/testing.md)
