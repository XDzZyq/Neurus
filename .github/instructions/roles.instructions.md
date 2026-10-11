# Roles and Parallel Work

How to split work across the master agent and subagents without racing or
reverting each other's changes. Repository-wide rules are summarized here; the
mechanical commands live in [tips.instructions.md](tips.instructions.md).

## Master vs. subagents

- **The master agent owns the build, the tests and the commit.** It runs the
  final `cmake --build` and the one full `ctest`/`Neurus.exe` pass, and it is the
  only one that commits or merges.
- **Subagents edit only.** They may compile-check individual files (for example
  via diagnostics) but must **not** invoke the build system, run `ctest`, or
  launch `Neurus.exe`.
- **Never `git stash` while others are editing** - it reverts another agent's
  in-progress work.

## Subagent racing prevention

A single `Neurus.exe`/`neurus_test` binary and a single build directory are
shared resources. Concurrent access fails with "Access denied" or file-in-use,
or produces false test failures.

1. Only the master runs the build for final verification.
2. Only one agent runs `ctest` or `Neurus.exe` at a time.
3. Subagents compile-check only; they never call the build system.
4. Subagents never run the test binary or the app.

## Lane patterns

Split parallel work into **disjoint write scopes** so two agents never edit the
same file. Two shapes have worked well:

- **By directory** - one lane per top-level subtree (`src/render/**`,
  `src/ui/**`, `src/editor/**`, and the rest), with each lane reporting a
  "salvage list" of anything genuinely non-obvious it removed.
- **By artifact** - one lane per independent deliverable (a component, its
  tests, its docs) where the deliverables share no files.

Only the master integrates the lanes and resolves overlaps. If two lanes must
touch one file, serialize them.

## When to use which agent

| Agent | Use it for |
|-------|-----------|
| **orchestrator** (master) | Coordinating lanes, integrating, running the final build/test, committing. Owns anything with a shared resource. |
| **explorer** | Read-only reconnaissance: locating code, tracing call chains, answering "how does this work" from the source. Fast, no edits. |
| **fixer** | A targeted change: fix a build error, a failing test, or a specific bug in a bounded scope. |
| **designer** | Producing a written design or plan before implementation - options, trade-offs, and the chosen approach. |
| **oracle** | Architectural review and simplification of an existing design or change: "is this the right shape?", "can this be simpler?". High-reasoning, advisory. |
| **librarian** | External knowledge: library/framework docs, API references and real-world examples (via web search and code search). Not for this repository's own code. |

Prefer the lightest agent that fits: reconnaissance to `explorer`, a bounded
change to `fixer`, and keep the master free to build, verify and commit.
