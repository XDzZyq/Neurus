# Feature Development Workflow

Every rendering feature (a new light type, a new pass, a new effect) follows this
proven pattern, learned while implementing deferred PBR, SSAO, multi-light
shadows and the sun light. It is a checklist, not a design document - design
detail lives in [`docs/`](../../docs/README.md).

## Phase 1 - Plan

1. **Trace the closest existing feature** end to end to find every touchpoint.
   Example: study the entire point-light pipeline before adding a sun light.
2. **Map every layer** the feature crosses: scene -> cache -> passes -> shaders ->
   renderer -> UI -> tests -> docs. No layer should be surprised by it.
3. **Confirm the approach with the user** before writing code: new class vs.
   extending an existing one, storage strategy, binding layout, shader approach,
   test strategy.

The layer responsibilities and the pipeline are documented in
[architecture/layers.md](../../docs/architecture/layers.md) and
[render/passes.md](../../docs/render/passes.md).

## Phase 2 - Implement

Break the work into four waves. Fan out aggressively where files are disjoint.

| Wave | What | Parallelism |
|------|------|-------------|
| Foundation | Data structs, shaders, cache support | Full parallel (independent) |
| Core | Extend passes, modify GLSL | Parallel unless the same file |
| Integration | Wire into the renderer, UI, scene | Sequential (depends on Core) |
| Tests + Docs | GPU tests, reference images, docs | Full parallel |

Rules that hold across waves:

- Keep GPU-visible structs byte-for-byte identical to their GLSL counterparts
  (`static_assert` size + `offsetof` per field).
- A new pass declares its image I/O through `Pass::GetIO()`; the RenderGraph wires
  and orders it. See [render/render-graph.md](../../docs/render/render-graph.md).
- Wire each part into the renderer **immediately**; never land a feature that is
  inert.
- Keep classes lean and functions small (single responsibility); refactor
  "clumsy" code early.

## Phase 3 - Verify (per wave, non-negotiable)

Do not move to the next wave if any check fails.

```
cmake --build build --config Debug                # 0 errors
python scripts/extract_i18n.py --check            # 100% translated, not stale
make check                                        # all pass, including regression
& "build/Debug/Neurus.exe" 2>&1 | Select-String "VUID-"   # zero matches
```

- **Every new user-visible string is translated in the same commit.**
  `scripts/extract_i18n.py` only extracts; a new key lands with an empty
  `msgstr`, which counts as *missing*, not pending. Fill the translation for every
  shipped catalog until `--check` prints `OK`.
- **Always launch `Neurus.exe`** and check the terminal for `VUID-` validation
  errors, crashes and unexpected `NEURUS_ERR` lines; then check the viewport
  visually and exercise resize/interaction for deadlocks.
- **Verify every reference image with Python PIL** before committing: not empty,
  not uniform, not all-black or all-white. The exact checks are in
  [docs/build/test.md](../../docs/build/test.md).

## Phase 4 - Fix common bugs

Pitfalls encountered during past features are consolidated as straightforward
`symptom -> rule` entries in [lessons.instructions.md](lessons.instructions.md).
Check there first when a wave misbehaves.

## Phase 5 - Commit

- **Do not commit until the user explicitly approves** unless they told you in
  advance that you may.
- Follow the current commit format (`feat(scope): what was built`).
- In the description, add: "No validation error, no unreasonable reference image,
  all tests passed."
- See [git-workflow.instructions.md](git-workflow.instructions.md).

## Checklist

- [ ] All four waves complete
- [ ] Build: 0 errors, 0 warnings
- [ ] All tests pass (`make check`)
- [ ] Zero `VUID-` validation errors from `Neurus.exe` and the test binary
- [ ] No untranslated or stale catalogs (`python scripts/extract_i18n.py --check`)
- [ ] Every reference image verified with Python PIL
- [ ] No stubs, TODOs or placeholders left behind
- [ ] Relevant `docs/` updated (docs are the design authority)
- [ ] User reviewed and approved
