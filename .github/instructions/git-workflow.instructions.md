# Git Workflow

Branch, worktree and commit conventions. The mechanics of the build tree and
worktree tooling are in [tips.instructions.md](tips.instructions.md); who may
commit is in [roles.instructions.md](roles.instructions.md). Product docs live
under [`docs/`](../../docs/README.md).

## Issue & Label Conventions

- **Title**: clean, no prefix. Category expressed via label instead (e.g. title `Transform Editor panel` + label `UI & UX`, not `feat(ui): Transform Editor panel`).
- **Labels**: `Feature`, `Bug`, `Documentation`, `Core`, `UI & UX`, `Render`, `Good First Issue`, `Duplicated`.

## General Rules

- Prefer Git submodule.
- **Push Neurus-Lib before Neurus.** Check `git -C lib status` (and whether the
  `lib` pointer moved) before every push. When Neurus-Lib has changes: (1) commit
  and push them to Neurus-Lib, (2) wait until its build/release GitHub Action
  succeeds and publishes the release, (3) only then push Neurus. Neurus CI
  downloads the prebuilt libraries from that release, so pushing Neurus first -
  or against a Neurus-Lib commit whose release is not out yet - breaks Neurus CI.
  See [lessons.instructions.md](lessons.instructions.md).
- For renaming and moving, use `git mv` to track the history.
- Any files should not include absolute path.
- Complete and double check all aspects (tests, coding style, document) of the
  current task before commit.
- Only the master agent can commit and merge. If subagent tasks may overlap in
  files, use Branches and Git Worktree for parallelism and isolation - and see
  [roles.instructions.md](roles.instructions.md) for lane patterns. Do not
  forget to remove the branch and worktree once the task is complete.
- Use the worktree tooling to create a worktree. Keep the worktree directory
  outside the main workspace (see [tips.instructions.md](tips.instructions.md)).

## Lessons Learned

**CRITICAL: Work exclusively in the worktree.**
- The main workspace stays on `master` and must remain clean.
- All file edits, commits, and builds for feature work happen inside the worktree directory.
- Do NOT switch branches in the main workspace - use the worktree for isolation.
- Do NOT create worktree in `~/.config/opencode/*`, create the worktree inside `.omo/worktrees/*` or `.slim/worktrees/*`. So that user can view the changes in the main directory and the worktree directory at the same time.
- ALWAYS use `make update` to update the git submodules and pre-compiled libs.

**Merge conflicts are expected.**
- When `master` has progressed (new features added after your branch point), merge conflicts will occur.
- Example: `master` had IBL pass additions not present in the feature branch. Resolution: keep IBL code from master, but adapt it to use the refactored patterns from the feature branch.

**Subagents may not be able to target a worktree directory.**
- A subagent launched with the `task` tool operates in the main workspace root.
- For isolation, prefer the worktree tooling and move the session's working
  directory into the worktree where the harness supports it; otherwise serialize
  overlapping work (see [roles.instructions.md](roles.instructions.md)).

**Build and test from the main workspace after merge.**
- After merging, the main workspace contains the combined code.
- Run the Debug build and `make check` from the main workspace, in the live build
  tree named in [tips.instructions.md](tips.instructions.md).
- The worktree's build directory is separate - do not rely on stale worktree builds.

**Worktree cleanup can be tricky.**
- `git worktree remove` may fail with "Permission denied" if processes (e.g., graphify hook) hold file locks.
- Use `--force` if needed: `git worktree remove ../Neurus_Feature --force`
- If the worktree is already unregistered from git but the directory persists, remove manually.
- Always delete the branch: `git branch -d feature/my-feature`
