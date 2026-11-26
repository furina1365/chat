---
name: code-review
description: 审查 chat 项目的分支、MR 或指定范围内的改动。并行核对代码质量与仓库规范、需求符合度，并在用户询问能否提 MR 或合并时给出有验证依据的结论。
---

Two-axis review of the diff between a base ref and the candidate revision:

- **Standards**: does the code behave correctly and conform to this repo's documented coding standards?
- **Spec**: does the code faithfully implement the originating issue / spec?

Both axes run as **parallel sub-agents** so they don't pollute each other's context, then this skill aggregates their findings.

## Process

### 1. Pin the fixed point

Use the base the user supplied or the target branch of an existing MR. For a prospective MR with no specified target, use the repository's default branch if it can be identified and state that assumption. Ask when the target is still ambiguous and would change the review. Use the named candidate branch or MR head when supplied, and `HEAD` for the current branch. For an explicit worktree review, include the requested staged, unstaged, and untracked changes.

Resolve both refs to commits, then use `git diff <base>...<candidate>` to review the changes since their merge base and `git log <base>..<candidate> --oneline` to check the candidate's commits. For a worktree-only review, capture its status and diffs directly. If the requested scope is empty, report that there are no changes to review.

### 2. Identify the spec source

Use any requirement or spec the user provides. Otherwise, look for the originating spec in this order:

1. The existing MR description and linked issues, or issue references in the candidate commits, when accessible.
2. Change plans or execution documents in [`docs/changes/`](../../../docs/changes/) that match the branch or feature. Use the plan for intended behavior and the execution document for prior evidence.

If no spec is available, continue the Standards review and record the gap under `Spec`. Request the spec only when a missing requirement prevents the requested conclusion.

### 3. Identify the standards sources

Use the applicable `AGENTS.md` files and any other repository coding standards.

On top of whatever the repo documents, the Standards axis always carries the **smell baseline** below: a fixed set of Fowler code smells (_Refactoring_, ch.3) that applies even when a repo documents nothing. Two rules bind it:

- **The repo overrides.** A documented repo standard always wins; where it endorses something the baseline would flag, suppress the smell.
- **Always a judgement call.** Each smell is a labelled heuristic ("possible Feature Envy"), never a hard violation. Like any standard here, skip anything tooling already enforces.

Each smell reads *what it is* → *how to fix*; match it against the diff:

- **Mysterious Name**: a function, variable, or type whose name doesn't reveal what it does or holds. → rename it; if no honest name comes, the design's murky.
- **Duplicated Code**: the same logic shape appears in more than one hunk or file in the change. → extract the shared shape, call it from both.
- **Feature Envy**: a method that reaches into another object's data more than its own. → move the method onto the data it envies.
- **Data Clumps**: the same few fields or params keep travelling together (a type wanting to be born). → bundle them into one type, pass that.
- **Primitive Obsession**: a primitive or string standing in for a domain concept that deserves its own type. → give the concept its own small type.
- **Repeated Switches**: the same `switch`/`if`-cascade on the same type recurs across the change. → replace with polymorphism, or one map both sites share.
- **Shotgun Surgery**: one logical change forces scattered edits across many files in the diff. → gather what changes together into one module.
- **Divergent Change**: one file or module is edited for several unrelated reasons. → split so each module changes for one reason.
- **Speculative Generality**: abstraction, parameters, or hooks added for needs the spec doesn't have. → delete it; inline back until a real need shows.
- **Message Chains**: long `a.b().c().d()` navigation the caller shouldn't depend on. → hide the walk behind one method on the first object.
- **Middle Man**: a class or function that mostly just delegates onward. → cut it, call the real target direct.
- **Refused Bequest**: a subclass or implementer that ignores or overrides most of what it inherits. → drop the inheritance, use composition.

### 4. Spawn both sub-agents in parallel

**Standards sub-agent prompt** should include:

- The full diff command and commit list.
- The list of standards-source files you found in step 3, **plus the smell baseline from step 3** pasted in full (the sub-agent has no other access to it).
- Review defects, regressions, compatibility and relevant edge cases first. For each finding, give a file and line, a trigger, and the user-visible or operational impact. Cite the rule for documented-standard violations. Mark code smells as judgement calls and skip style issues covered by tooling.

**Spec sub-agent prompt** should include:

- The diff command and commit list.
- The path or fetched contents of the spec.
- The brief: "Report: (a) requirements the spec asked for that are missing or partial; (b) behaviour in the diff that wasn't asked for (scope creep); (c) requirements that look implemented but where the implementation looks wrong. Quote the spec line for each finding. Under 400 words."

If the spec is missing, skip the Spec sub-agent and note this in the final report.

### 5. Aggregate

Present the two reports under `## Standards` and `## Spec` headings, verbatim or lightly cleaned. Do **not** merge or rerank findings, because the two axes are deliberately separate (see _Why two axes_).

Summarize the count and most important finding within each axis. Mark findings that block readiness and explain what needs to change.

### 6. Assess MR readiness when requested

Use the [project test guide](../../../docs/testing.md) to select checks for the changed paths and apply [verification-before-completion](../implement-and-document/references/skills/verification-before-completion/SKILL.md) before making pass claims. If local changes or a different checkout would affect the results, use an isolated checkout of the candidate. Tie any existing CI result to the exact candidate commit. Report the checks that passed, failed, or could not run, with commands or CI links and any material coverage gaps.

Conclude whether the branch is ready to open an MR, suitable for a draft while a material check is outstanding, or needs fixes first. For an existing MR, give a separate merge-readiness conclusion based on the available evidence and required checks.

## Why two axes

A change can pass one axis and fail the other:

- Code that follows every standard but implements the wrong thing → **Standards pass, Spec fail.**
- Code that does exactly what the issue asked but breaks the project's conventions → **Spec pass, Standards fail.**

Reporting them separately stops one axis from masking the other.
