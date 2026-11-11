# Neurus Developer Instructions

Daily-development material for working in this repository: conventions,
lessons learned, git workflow, tips and agent roles. System design and
architecture are product documentation under [`docs/`](../../docs/README.md) -
this folder does not restate them.

| Instruction | Purpose |
|-------------|---------|
| [conventions.instructions.md](conventions.instructions.md) | C++/GLSL language, formatting, naming, RAII, error handling, logging and comment conventions |
| [development.instructions.md](development.instructions.md) | The four-wave feature-development workflow and its per-wave verification |
| [git-workflow.instructions.md](git-workflow.instructions.md) | Branch, worktree, issue and commit conventions, plus the lessons learned around them |
| [lessons.instructions.md](lessons.instructions.md) | Encountered issues and pitfalls, each as a short symptom -> rule |
| [tips.instructions.md](tips.instructions.md) | Build/test commands, the two build trees, worktree tools, graphify and presets |
| [roles.instructions.md](roles.instructions.md) | Master vs. subagent rules, racing prevention, lane patterns and agent roles |

Product documentation is the design authority: [`docs/README.md`](../../docs/README.md).

Link direction is one-way: instructions may link to `docs/`; `docs/` never links
back here.
