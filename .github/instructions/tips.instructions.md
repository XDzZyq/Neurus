# Tips

Working notes for building, testing and running Neurus on a local machine. Full
build and test guides live in [docs/build/build.md](../../docs/build/build.md)
and [docs/build/test.md](../../docs/build/test.md).

## Build and test commands

```
make update     # init submodules, fetch prebuilt deps, configure CMake
make build      # full Debug build        (make build app / make build test)
make check      # run all tests           (make check FILTER="-R Pattern")
make release    # configure + build Release
make nobuild    # generate the VS 2022 solution at ../Neurus_VS2022
```

Direct CMake equivalents:

```
cmake --preset win
cmake --build build --config Debug
cd build && ctest -C Debug --output-on-failure
```

`make update` is mandatory after a fresh clone or a submodule bump; it needs
network access for the prebuilt binaries.

## Two build trees - and the one that is live

There are two trees, with separate test discovery. Aiming at the wrong one does
not fail - it answers about a different codebase.

| Tree | Generator (typical) | Run its tests with | Binaries |
|------|---------------------|--------------------|----------|
| `build/debug` | single-config Ninja | `cd build/debug && ctest` | `build/debug/Neurus`, `build/debug/neurus_test` |
| `build/` | multi-config (VS on Windows, Ninja Multi-Config on CI/macOS) | `cd build && ctest -C Debug` | `build/Debug/...` per config |

- **A bare `ctest` in a multi-config tree prints `Total Tests: 0` and exits 0.**
  That is a green that ran nothing. Always pass `-C Debug` there.
- **A stale `-C Debug` run is worse than an empty one**: it reports a plausible
  pass count from whatever its last configure saw.
- Filesystems are case-insensitive on Windows and macOS by default, so
  `build/Debug` **is** `build/debug` there. Pick one tree per machine and stay in
  it.

The test working directory is pinned to each tree's root (`CMAKE_BINARY_DIR`),
where the POST_BUILD step copies `res/`. Run tests through CTest, or `cd` into
the tree root before invoking the binary directly - otherwise `res/` is
unresolvable and reference images can be written outside the tree.

## CMake presets

`CMakePresets.json` defines:

- `win` - Windows x64 (Visual Studio 17 2022, MSVC), Debug + Release via
  `--config`; enables tests and validation.
- `vs2022` - a Visual Studio 2022 solution at `../Neurus_VS2022` (outside the
  source tree) for IDE work.
- `macos` - macOS arm64, Debug + Release in one tree.

## `/bigobj` is mandatory on MSVC

`CMakeLists.txt` passes `/bigobj` to every MSVC compile. Without it the cereal
registration TUs fail with C1128 once enough types are registered. The flag only
widens the COFF section limit and changes no codegen; if a TU hits C1128 again,
split that TU rather than adding the flag.

## Worktrees

- Prefer a git worktree for isolated feature work; keep it **outside** the main
  workspace, or under `.omo/worktrees/*` / `.slim/worktrees/*` so the main tree
  and the worktree stay visible side by side.
- Use the worktree tooling to create it, and work exclusively inside it. Do not
  switch branches in the main workspace.
- Clean up when done: `git worktree remove <path>` (add `--force` if a process
  such as the graphify hook holds a lock), then delete the branch.
- After merging, build and test from the main workspace - the worktree's build
  directory is separate and stale.

## graphify

The repository keeps a knowledge graph at `graphify-out/`, refreshed
automatically by a git post-commit/post-checkout hook. Do **not** run
`graphify update`/`rebuild` proactively; invoke it only through the `/graphify`
skill or when the user asks. Dirty graphify-out files after a hook are expected.

Only `graphify-out/post_rebuild_cleanup.py` is tracked - everything else there
is generated. That script is not optional and not cosmetic: it carries the noise
filters and the community-naming rules, and pins `PYTHONHASHSEED` so clustering
is reproducible. The raw extraction is roughly 5,300 nodes across 300 communities
named after arbitrary hub symbols; after cleanup it is ~3,000 nodes across 125
named communities. Skipping it also pushes the graph past graphify's 5,000-node
visualisation ceiling, which silently degrades the HTML to a community-only
aggregate. The full pipeline is therefore two steps, not one:

```bash
graphify extract . --out .
python graphify-out/post_rebuild_cleanup.py   # also writes graph.html
```

The web documentation publishes that graph as `docs/graphify.html`. `make doc`
runs the pipeline above and then builds the site; the docs workflow does the same
with both graphify steps marked `continue-on-error`, so a graphify regression
degrades to a stub page instead of blocking the deploy. The scan is confined to
`src/` by `.graphifyignore`, which is what keeps it local and free of API calls -
widening it to `docs/` would activate the semantic pass and require a key.
