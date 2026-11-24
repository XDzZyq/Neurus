# Deferred deserialization of project resources

[Neurus issue #80](https://github.com/XDzZyq/Neurus/issues/80) · Implementation plan · Base `fe8ec583`

Opening a project should read its JSON once to discover what exists. Constructing a CPU resource, reading its OBJ or image, and compiling its shader should happen when a caller first requests that UID. Project open itself requests the scene, the active render graph and their dependencies so the first frame has everything it needs.

For example, if two scene meshes refer to the same MeshData UID, the first lookup reads the OBJ and caches the complete MeshData. The second mesh receives the same pointer. A third OBJ used only by an unrelated pooled mesh stays unread, including through an immediate save.

## 1. Capture the existing behavior

- Extend the existing project round-trip and resource-manager tests. Use a fixture with two meshes sharing geometry, a selected object, an active camera and graph, an inactive graph with its own shader, and unused geometry and image data.
- Count actual OBJ reads, image decodes and shader compilations by resource in the test fixture. Measure from project open through first publication, then separately during save and explicit lookup. Record counts and elapsed open time from the current branch point for comparison.
- Pin down existing recovery behavior for missing dependencies, optional shader defaults, malformed records, legacy scene fields and inline graphs. These expectations guide the reader migration.

## 2. Index records and teach the pool to load by UID

- In `ResourceComponent::LoadJson`, reuse the existing per-entry normalization of cereal type IDs. Validate identity and concrete type, apply the existing duplicate policy, and retain an owned normalized payload for each accepted UID. Index names in UID order so collisions resolve deterministically. Reserve the allocator past all accepted IDs before restoring roots or creating defaults.
- Keep JSON, type-specific readers and asset path handling in project persistence. Give the main-thread `ResourceManager` metadata and a callable for each deferred UID. The callable must remain usable after the input archive and project components go out of scope. Preserve the base for resolving external files across Save As.
- Implement the cache in the existing virtual `Get(int) const`. A hit returns the shared object. A miss invokes its loader, recursively resolves dependencies, checks the restored UID and concrete type, then publishes the complete object. Use a scoped in-progress guard to diagnose recursive cycles and unwind cleanly on failure. An unknown UID returns null.
- Make membership, size and name registration work from metadata. Route name lookup through `Get`. Snapshot IDs before object enumeration and fetch each through the same cache, preserving exact-type enumeration and its existing fallback. Cover registration collisions, rename, remove, clear and project replacement across both live and deferred entries.
- **Checkpoint.** Counting loaders in pool tests show zero calls for metadata queries, one successful call across repeated UID and name lookups, and no partially initialized object after failure. Creating a resource before any deferred lookup produces a fresh UID.

## 3. Restore roots and dependencies in project readers

- Move the UID lists from `Scene` into a local persistence record read by `SceneComponent`. Resolve typed scene membership first, then apply selection, active object and active camera. Preserve membership order and the legacy distinction between an absent camera field and an explicitly cleared camera.
- Move mesh, debug-mesh and environment wiring from `ResourceComponent::RestoreReferences` into their individual load paths. Resolve geometry, optional shaders and images as each object is built. Preserve IDs that carry document meaning, including a stale environment image reference.
- Read graph topology into a local `RenderGraphRecord`. Use the existing pass catalog and shader fallback rules when constructing a graph. Support both the active pooled graph UID and legacy inline topology. An inactive graph is resolved when requested.
- Route normal project open through indexing followed by root restoration. Remove the whole-pool fixup scans, `Scene`'s pending members and `RenderGraphData::m_pending`. Check other reference fields by lifetime and remove those used only between archive read and fixup. Editor upload and graph publication continue to consume fully built CPU objects.
- **Checkpoint.** Through normal project open, shared geometry loads once and unused assets record zero content loads through first publication. Exercise wrong-type references, a broken dependency and a cycle, then successfully retrieve an independent resource.

## 4. Round-trip a partially loaded pool

- Have project save enumerate registered IDs without calling object-valued enumeration. Serialize live objects from their current state and write untouched entries from retained payloads. Reflect registry name changes in the saved record. Allocate consistent archive-wide cereal polymorphic type IDs and shared-pointer IDs across both sources.
- Exercise open → edit one live object → Save As → reopen → request an untouched resource. Include repeated concrete types, shared references, relative external paths, and both live and deferred deletions. Check that project replacement releases the previous records.
- **Checkpoint.** Save adds zero content loads. After reopening, edits, names and UIDs survive, a deleted UID stays absent, and requesting an untouched resource twice loads it once and returns the same pointer.

## 5. Verify the complete project-open path

- Repeat the baseline on the same fixture and build configuration. Report elapsed open time alongside per-resource counts. The acceptance criteria are first-frame readiness, pointer identity, one load for shared content, and zero loads for unused content through open and save.
- Run the project round-trip, history/undo, inactive-graph shader retention and reference-image coverage. Run `make check` for the cross-layer change. Exercise open, edit, undo, save/reopen and snapshot capture through a CLI-driven session. Check captured output for validation errors and inspect affected reference images with PIL.
- Update the resource ownership and project-format documentation with the lookup and save contracts. Follow the repository's phase review and commit/push gates.

## Phase 1 baseline

Measured on Windows x64, MSVC Debug, using `ProjectRoundtrip.MixedPoolContentLoads` in three separate processes after the build finished. The fixture creates and saves its source assets before timing, so filesystem caches can be warm. Open time covers `ApplicationCore::LoadProject` through the first graph publication to a recording render sink. Samples were 275.7, 288.5 and 286.7 ms (median 286.7 ms).

| Observed operation | Open | Immediate Save As | Explicit lookups after save |
|---|---:|---:|---:|
| Shared scene OBJ read | 1 | 0 | 0 |
| Unused OBJ read | 1 | 0 | 0 |
| Unused image decode | 1 | 0 | 0 |
| Inactive graph shader compilation | 1 | 0 | 0 |

All three runs produced these counts. After lazy loading lands, move the last three counts from the open column to the explicit lookup column. Keep the shared geometry pointer, restored selection, active camera and published graph assertions in place.

Added a separate round-trip regression for a missing environment image UID. Both Debug test executables built against the worktree's Python environment. The focused CTest run passed all 98 tests, including the layer-boundary check. Phase 1 was committed as `5a3d220`.

## Phase 2 checkpoint

Project open indexes concrete types and reserves every accepted UID before it requests objects. Each loader owns its normalized cereal record. Reading an indexed record reuses its reserved UID, so reopening the document does not consume a temporary identity for each pooled resource.

The pool tests cover metadata queries, rename by UID, shared lookup results, typed enumeration, duplicate registrations, removal, project replacement, cycle recovery, and rejection of loaders returning the wrong object. The current project reader still requests every indexed record for the reference-wiring pass. It assigns unique names in UID order after validation so legacy records with omitted names participate using their constructor defaults. Phase 3 must preserve that ordering when it moves name assignment ahead of root restoration.

The full Windows Debug build succeeded. `make check` reported 1,004 passes, zero failures and one platform skip for POSIX permission reporting. The translation check passed. The reference-image files are unchanged, including the constant-depth sun shadow reference.

## Phase 3 checkpoint

Project open now resolves the scene, viewport camera and active graph through the indexed pool. The mixed fixture reads the shared OBJ once and does zero unused OBJ reads, image decodes or inactive shader compilations through first publication. Scene membership and graph topology are restored from local records. Scene and RenderGraphData no longer carry pending state. The full Debug build and 99 targeted CTest cases passed.

## Phase 4 checkpoint

Saving a partially loaded pool preserves unrequested records and writes current live objects. The application fixture reports zero additional content loads through Save As and one on first explicit lookup of each unused asset. The mixed-save regression covers a legacy name field, shared geometry, UID removal, an absent original project file, and reopening through both supported project readers. The focused persistence suite passed all 57 tests.

## Phase 5 verification

Three independent Windows x64 MSVC Debug runs of the phase 1 fixture measured 40.837, 45.927 and 40.365 ms from project open to first graph publication. Median open time fell from 286.7 ms to 40.8 ms on this fixture. Filesystem caches may be warm. All three runs produced the same content-load counts.

| Observed operation | Open | Immediate Save As | Explicit lookups after save |
|---|---:|---:|---:|
| Shared scene OBJ read | 1 | 0 | 0 |
| Unused OBJ read | 0 | 0 | 1 |
| Unused image decode | 0 | 0 | 1 |
| Inactive graph shader compilation | 0 | 0 | 1 |

The Debug build succeeded. `make check` passed 1,008 tests with zero failures. Windows skipped the POSIX-only registry permission test. PIL read all 25 reference PNGs, including the intentionally uniform sun shadow depth reference. The translation check found all 207 keys translated.

A CLI-driven Vulkan session opened a fixture with unused mesh and image records, added and moved a camera, replayed undo and redo, changed gamma, saved and reopened the project. The camera retained its position of (1, 2, 3). Both unused UIDs and source paths survived Save As without a corresponding asset load in the log. Snapshot capture produced a nonuniform 1278 × 797 image after the first frame was presented. The application logs contained zero `VUID-` messages. The host also reported RenderDoc manifest, OBS layer version and Qt window geometry warnings.
