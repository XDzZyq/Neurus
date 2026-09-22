# AGENTS.md
# Guidance for agentic coding in this repo

This repository is a C++20 Vulkan-HPP 1.4 real-time renderer with strict layer
isolation. Use this file as the high-level reference; detailed rules live in
`.github/instructions/*.md`.

---

## Quick Reference

| Topic | Document |
|-------|----------|
| Architecture & Layers | [architecture.instructions.md](.github/instructions/architecture.instructions.md) |
| Build & Test | [build.instructions.md](.github/instructions/build.instructions.md) |
| Code Style & Error Handling | [style.instructions.md](.github/instructions/style.instructions.md) |
| Git Workflow | [git-workflow.instructions.md](.github/instructions/git-workflow.instructions.md) |
| Testing Standards | [test.instructions.md](.github/instructions/test.instructions.md) |
| Renderer Layer | [renderer.instructions.md](.github/instructions/renderer.instructions.md) |
| Editor Layer | [editor.instructions.md](.github/instructions/editor.instructions.md) |
| Operation System (Undo/Redo) | [operation-system.instructions.md](.github/instructions/operation-system.instructions.md) |
| UI System | [ui-system.instructions.md](.github/instructions/ui-system.instructions.md) |
| Event System | [events.instructions.md](.github/instructions/events.instructions.md) |
| Asset Layer | [data-resource.instructions.md](.github/instructions/data-resource.instructions.md) |

---

## Architecture (Hard Requirements)

```
┌──────────────────────────────────────────────────────────────┐
│ UI Layer (Qt6 Widgets + ADS)                                 │
│  owns: VkSurfaceKHR, QWindow, UIEvents (QObject singleton)   │
│  QML provides window + input ONLY. No rendering logic.      │
└─────────────────────┬────────────────────────────────────────┘
                      │ Qt Signals/Slots
                      ▼
┌──────────────────────────────────────────────────────────────┐
│ Editor Layer                                                │
│  owns: Scene, RenderConfig, UploadManager, Controllers        │
│  Application logic, scene mutation, event orchestration     │
└─────────────────────┬────────────────────────────────────────┘
                      │
            ┌─────────┴──────────┐
            ▼                    ▼
┌──────────────────┐  ┌────────────────────────────────────────┐
│ Asset Layer       │  │ Renderer Layer (Vulkan-HPP vk::raii)   │
│  Vulkan-free       │  │  owns: VkInstance, VkDevice, VkQueue,  │
│  MeshData,         │  │   VkSwapchainKHR, VkPipeline,          │
│  ImageData,        │  │   VkCommandBuffer, all GPU resources   │
│  PixelFormat       │  │   MeshGPU, EnvironmentGPU (via Cache)  │
│                    │  │  consumes: VkSurfaceKHR (read-only)    │
├──────────────────┤  └────────────────────────────────────────┘
│ Scene Layer       │
│  Vulkan-free       │
│  Camera, Light,    │
│  Mesh, Transform   │
└──────────────────┘
```

### Isolation Chain (Hard Requirement)

Data and control flow follow this strict unidirectional chain:

```
UI Layer (Qt6) → UIEvents (Qt Signals) → Editor → EventQueue (typed events) → Controllers → Context → Renderer
```

- UI emits signals via UIEvents. NEVER calls the Editor or Renderer directly.
- Editor receives raw InputState, translates to typed events via EventQueue, dispatches to Controllers.
- Controllers handle discrete events (CameraController subscribes to CameraEvents) -- no per-frame polling.
- Renderer receives read-only scene data through Context objects. NEVER calls back into Editor.
- Each arrow represents a **decoupled interface** (signals, events, contexts). No layer knows the internals of the next.

### Layer Boundaries

- **Renderer**: pure rendering service; owns GPU resources; owns `MeshGPU` and
  `EnvironmentGPU` via `RenderCache`; consumes read-only scene data; must not
  mutate application-level state.
- **Editor**: application logic and scene mutation; owns Scene, RenderConfig,
  Controllers, and the ResourceManager (app-scoped UID object pool);
  communicates via Context, UIEvents (Qt signals), and EventQueue (typed
  events). Exposes `SetRenderConfig()` for UI→editor sync.
- **UI**: Qt6 Widgets presentation only; owns surface; emits signals.
- **Asset** (`src/asset/`): Vulkan-free CPU-side asset loading (MeshData,
  ImageData, PixelFormat enum). No GPU resources.
- **Scene** (`src/scene/`): Vulkan-free logical scene objects (Camera, Light,
  Mesh, Transform). GPU resources separated into `MeshGPU` owned by Renderer.

### Communication

Cross-layer communication MUST go through:
- **UIEvents** (Qt Signals/Slots) for UI↔Editor signals
- **EventQueue** for Editor↔Renderer events
- **Context objects** for data queries

Direct coupling across layers is forbidden. Renderer must not include Editor or
UI headers. UI must not include Renderer headers (only surface handle).

### Vulkan Ownership

- UI Layer: owns `VkSurfaceKHR` (created from QWindow + QVulkanInstance)
- Renderer Layer: owns everything else (instance via QVulkanInstance sharing,
  device, swapchain, pipeline, command pool, all GPU resources)
- No shared ownership of GPU resources across layers.
- Non-copyable GPU resource classes (`= delete` copy/assign).

### Threading

- Main thread only for MVP.
- Future: Renderer may run on dedicated thread with proper synchronization.

---

## Feature Development

For implementing any new rendering feature (light type, pass, effect), follow
the proven pattern documented in [development.instructions.md](.github/instructions/development.instructions.md).
This workflow was established through deferred PBR, SSAO, multi-light shadows,
and sun light implementations, and encodes the common pitfalls and verification
steps.

Key rules:
- Plan by tracing the closest existing feature
- Break into 4 waves: Foundation → Core → Integration → Tests/Docs
- Verify EVERY wave: build, ctest, VUID check, Python PIL for references
- Never commit until user approves, unless user notified you in advance.

## Development Guidelines

Follow Karpathy Guidelines. For each task:

1. Think and plan. System design should be highly decoupled and elegant. Also, do not assume user is always correct, ask question if you have any concerns, confusions, or better design.
2. Implement. Do not hide errors, expose them directly. Use debug printing and
   logging. Wire every feature **IMMEDIATELY** into the renderer and program.
3. Test and Verify. 
   - The design of test should be comprehensive (mathematical verification, reference image test, etc.), forming a logic chain that proves the correctness of each component.
      - Reference image must be verified externally through python: The generated reference is not empty (transparent), purely black or write, or any unreasonable result. Do not cheat yourself.
   - Do not just run unit tests -- **always launch `Neurus.exe`** to check:
      - Terminal output for validation errors (`VUID-...`), crashes, or unexpected log messages
      - Visual correctness in the rendered viewport (use screenshots for analysis)
      - Runtime behavior: resize the window, interact with the viewport, verify no deadlocks or freezes.
   - Do not move to the next stage if the test failed
   
4. Before committing, keep ALL relevant documents and guidances (`.github/instructions/*.md`) updated. No validation error, no unreasonable reference image, all tests passed.
5. Commit. Follow the current commit format. Moreover, in the description, add "No validation error, no unreasonable reference image, all tests passed."

After each development phase, stop and wait for user verification.

---

## File Layout

```
Neurus/
├── .github/
│   ├── instructions/       # Architecture/component docs for AI agents
│   └── workflows/          # GitHub Actions CI
├── cmake/                  # CMake helper modules
├── dep/                    # Git submodule dependencies
│   └── qtadvanceddocking/  # Qt-Advanced-Docking-System (ADS)
├── res/i18n/              # gettext .po translation catalogs (zh_CN.po, ...)
├── src/
│   ├── render/             # Renderer layer (Vulkan-HPP)
│   │   ├── Barrier.h/cpp            # Centralized image barrier management
│   │   ├── DeferredRenderer.h/cpp   # Deferred PBR pipeline (active renderer)
│   │   ├── Image.h/cpp              # GPU image with state tracking (ImageState)
│   │   ├── RenderCache.h/cpp        # Cross-frame mutable resource pool
│   │   ├── RenderConfig.h           # User-settable pipeline options
│   │   ├── RenderContext.h          # Per-frame immutable scene snapshot
│   │   ├── shaders/          # Shader pipeline (parse/generate/compile)
│   │   │   ├── Shader.h/cpp, ShaderGPU.h   # Per-mesh shader + GPU module
│   │   │   ├── ShaderUnit.h     # Per-stage state: code, parsed IR, SPIR-V, version
│   │   │   ├── ShaderLibrary.h/cpp  # Load/parse/generate/compile service
│   │   │   ├── ShaderParser.h/cpp   # GLSL -> ShaderStruct IR
│   │   │   ├── ShaderGenerator.h/cpp # ShaderStruct IR -> GLSL
│   │   │   └── RenderShader.h/cpp, ComputeShader.h/cpp, ShaderCompiler.h/cpp
│   │   ├── Swapchain.h/cpp
│   │   ├── UploadManager.h/cpp      # CPU-to-GPU upload service
│   │   ├── GPUProfiler.h/cpp        # Timestamp-query GPU/CPU per-pass profiling
│   │   ├── VulkanContext.h/cpp
│   │   ├── resources/        # GPU resource structs (owned by RenderCache)
│   │   │   ├── CameraGPU.h/cpp      # Shared camera UBO (viewProj + view)
│   │   │   ├── DebugCache.h/cpp     # Per-frame DebugDrawList SSBOs (CPUBuffer pairs)
│   │   │   ├── EnvironmentGPU.h     # IBL cubemap Textures
│   │   │   ├── LightGPU.h           # Per-light shadow resources
│   │   │   ├── LightingCache.h/cpp  # Light SSBO storage (point + sun)
│   │   │   └── MeshGPU.h            # GPU-side mesh + MeshPushConstants
│   │   ├── render_graph/     # Pass DAG orchestration
│   │   │   ├── RenderGraph.h/cpp    # Compile-once DAG, topological Execute()
│   │   │   └── DescriptorBinder.h   # Per-pass descriptor write helper
│   │   ├── passes/          # Render passes
│   │   │   ├── Pass.h/cpp, ComputePass.h  # Pass base + compute specialization
│   │   │   ├── GeometryPass.h/cpp
│   │   │   ├── SSAOPass.h/cpp
│   │   │   ├── LightingPass.h/cpp
│   │   │   ├── IBLPass.h/cpp
│   │   │   ├── ShadowDepthPass.h/cpp
│   │   │   ├── ShadowIntensityPass.h/cpp
│   │   │   ├── GizmoPass.h/cpp      # IDBuffer edge detection -> selection highlight
│   │   │   ├── ComposePass.h/cpp    # Highlight blend + gamma -> ComposedOutput
│   │   │   ├── DebugPass.h/cpp      # Debug/gizmo overlay raster pass (issue #22)
│   │   │   └── FXAAPass.h/cpp       # Luma-based post AA (conditional)
│   │   └── buffers/          # Buffer class hierarchy
│   │       ├── Buffer.h/cpp         # Virtual base class (Buffer)
│   │       ├── StagingBuffer.h/cpp  # Host-visible staging (upload/download ONLY)
│   │       ├── CPUBuffer.h/cpp      # Permanent host-visible mapped buffer
│   │       ├── GPUBuffer.h/cpp      # Device-local with staging
│   │       ├── ArrayBuffer.h        # Growable typed device buffer (ArrayBuffer<T>)
│   │       ├── UniformBuffer.h      # Template uniform (UniformBuffer<T>)
│   │       ├── VertexBuffer.h/cpp   # Vertex buffer (inherits GPUBuffer)
│   │       ├── IndexBuffer.h/cpp    # Index buffer (inherits GPUBuffer)
│   │       └── BufferLayout.h/cpp   # Vertex input layout description
│   ├── core/              # Core layer (identity + resource pool)
│   │   ├── UID.h/cpp              # Generic unique identifier primitive (resource base)
│   │   ├── ResourceManager.h/cpp # Single factory + UID pool (Load<T>, polymorphic serialize)
│   │   ├── IResourceLookup.h      # Read-only pool lookup interface
│   │   ├── Log.h                  # NEURUS_LOG / NEURUS_ERR macros
│   │   ├── Serialize.h            # NEURUS_OPTIONAL_NVP / OptionalBlock (back-compat fields)
│   │   ├── Graph.h                # Generic DAG template
│   │   ├── Selections.h           # Selection state container
│   │   └── Timer.h                # Scoped timer
│   ├── editor/             # Editor layer (logic, controllers)
│   │   ├── DebugDrawBuilder.h/cpp  # Scene debug objects -> DebugDrawList flattening
│   │   ├── events/          # Event system (UIEvents + typed EventQueue)
│   │   │   ├── UIEvents.h/cpp    # Qt signal bus for UI↔Editor
│   │   │   ├── EventBus.h        # Typed EventQueue dispatcher (no Qt)
│   │   │   ├── CameraEvents.h    # Camera event structs
│   │   │   └── ShaderEvents.h    # Shader editor event structs
│   │   ├── controllers/     # Controller implementations
│   │   │   ├── Controllers.h     # Base class for all controllers
│   │   │   ├── CameraController.h/cpp  # Event-driven camera controls
│   │   │   └── ShaderController.h/cpp  # Event-driven shader lifecycle
│   │   ├── operations/      # Undo/redo (see operation-system.instructions.md)
│   │   │   ├── Operation.h              # Base op + TransitionOp CRTP
│   │   │   ├── SceneOperations.h        # Per-object + selection ops
│   │   │   ├── ConfigOperations.h       # RenderConfig op
│   │   │   ├── OperationManager.h/cpp   # Undo/redo stacks + replay guard
│   │   │   └── registrations/           # cereal polymorphic registration
│   │   │       └── OperationRegistration.h/cpp  # op type registration + force-link
│   │   └── CMakeLists.txt
│   ├── ui/                 # UI layer (Qt6 Widgets + ADS)
│   │   ├── UIManager.h/cpp      # Main window with ADS dock manager + menus
│   │   ├── UIContext.h          # Per-frame UI data snapshot
│   │   ├── items/               # Reusable composite QWidgets
│   │   │   ├── ScalarSlider.h/cpp  # Slider+spinbox pair with auto-derived step
│   │   │   ├── Vec3Spin.h/cpp      # XYZ triple-spinbox composite widget
│   │   │   ├── ColorButton.h/cpp   # Color swatch button (QColorDialog, optional alpha)
│   │   │   ├── OutlinerRow.h/cpp   # Pool-recyclable outliner row
│   │   │   ├── ShaderFieldRow.h/cpp # Type/name editor row for struct fields
│   │   │   └── CodeEditor.h/cpp    # GLSL code editor (line numbers, monospace)
│   │   ├── presets/              # GOType-specific PropertyPanel subpanels
│   │   │   ├── CameraProperties.h/cpp       # Target + FOV
│   │   │   ├── MeshProperties.h/cpp         # Path + shadow/material toggles
│   │   │   ├── LightProperties.h/cpp        # Power/radius/cone + shadow toggle
│   │   │   ├── EnvironmentProperties.h/cpp  # Path + intensity/rotation
│   │   │   └── DebugProperties.h/cpp        # DebugLine/Points/Mesh + position table (issue #22)
│   │   ├── models/               # Qt item models (non-widget)
│   │   │   ├── ShaderStructModel.h/cpp  # Tree model for ShaderStruct IR
│   │   │   ├── LogModel.h/cpp        # QAbstractListModel over core LogBuffer
│   │   │   └── LogFilterProxy.h/cpp  # Level filter + search proxy
│   │   ├── delegates/            # Qt item delegates (non-widget)
│   │   │   ├── ShaderFieldDelegate.h/cpp  # Type/name editors for struct fields
│   │   │   └── LogDelegate.h/cpp     # Severity-colored row delegate
│   │   ├── utils/                # Non-widget UI helpers
│   │   │   ├── ShaderHighlighter.h/cpp  # GLSL syntax highlighter
│   │   │   ├── POCatalog.h/cpp       # gettext .po parser (neurus::po), unit-testable on its own
│   │   │   └── I18n.h/cpp            # Runtime i18n manager (gettext .po catalogs, live switch)
│   │   ├── panels/               # Dock panel widgets
│   │   │   ├── UIPanel.h/cpp     # Base class for all panels (I18n.h kept out of the header)
│   │   │   ├── Viewport.h/cpp    # Native HWND Vulkan surface widget
│   │   │   ├── Outliner.h/cpp    # Scene object hierarchy tree
│   │   │   ├── PropertyPanel.h/cpp  # Object property inspector (hosts the presets/)
│   │   │   ├── RenderConfigPanel.h/cpp  # Live render setting controls
│   │   │   ├── ShaderEditorPanel.h/cpp  # Code + Structure shader editor
│   │   │   ├── LogPanel.h/cpp        # Realtime log viewer dock (issue #39)
│   │   │   └── PreferencesDialog.h/cpp  # Preferences dialog (language, target FPS)
│   │   └── qml/            # QML source files (legacy)
│   ├── asset/              # Asset layer (Vulkan-free)
│   │   ├── Project.h/cpp            # Pure registration-based serializer
│   │   ├── Serializable.h           # Abstract base class: Key/Save/Load
│   │   ├── PixelFormat.h            # Vulkan-free format enum + helpers
│   │   ├── components/              # project::Serializable adapters
│   │   │   ├── ConfigComponent.h/cpp    # RenderConfig serialization adapter
│   │   │   ├── SceneComponent.h/cpp     # Scene serialization adapter (holds ResourceManager*)
│   │   │   ├── ResourceComponent.h/cpp  # ResourceManager pool persistence (registered first)
│   │   │   ├── UIComponent.h/cpp        # UI-state serialization adapter
│   │   │   └── HistoryComponent.h/cpp   # Undo/redo-stack adapter (wraps OperationManager)
│   │   ├── registrations/           # cereal polymorphic registration (data resources)
│   │   │   └── DataRegistration.h/cpp   # MeshData, ImageData (force-init)
│   │   ├── data/                     # CPU-side data containers (UID-derived pooled resources)
│   │   │   ├── MeshData.h/cpp        # CPU-side mesh geometry (UID, self-serializing)
│   │   │   └── ImageData.h/cpp       # CPU-side image pixels (UID, self-serializing)
│   ├── scene/              # Scene layer (Vulkan-free)
│   │   ├── Camera.h        # Camera object
│   │   ├── Light.h         # Light objects (PointLight, SunLight)
│   │   ├── Mesh.h          # Mesh + Transform (no GPU buffers; pooled MeshData/Shader refs)
│   │   ├── Sprite.h        # Screen-space sprite object
│   │   ├── Environment.h   # IBL environment object
│   │   ├── Material.h      # Material parameters
│   │   ├── DebugLine.h/cpp    # Debug segment-list object (retained, stateful)
│   │   ├── DebugPoints.h/cpp  # Debug point-list object (square/rhombus/circle/cube)
│   │   ├── DebugMesh.h/cpp    # Debug wireframe-mesh object
│   │   ├── DebugDrawList.h    # Flattened overlay payload consumed by DebugPass
│   │   ├── EditorContext.h    # Editor → renderer per-frame scene snapshot
│   │   ├── DefaultScene.h/cpp # Built-in starter scene
│   │   ├── ObjectID.h      # Scene identity + metadata (ObjectID : UID)
│   │   ├── Transform.h     # Spatial transform
│   │   └── registrations/           # cereal polymorphic registration
│   │       ├── TypeRegistration.h/cpp  # scene types + UID-level relations (force-init)
│   │       └── TypeRegistration.cpp    # scene object types
│   ├── app/                # Application layer (lifecycle orchestration)
│   │   ├── Application.h/cpp  # QApplication lifecycle, signal wiring, project/preferences persistence
│   │   ├── Preferences.h/cpp  # App-level preferences (language, target FPS) persisted to ~/.neurus/preferences.json — owned exclusively by the Application
│   │   └── VulkanContext.h/cpp
│   └── main.cpp            # Application entry point
├── test/
│   ├── render/             # Renderer GPU tests
│   │   └── reference/      # Reference images for regression tests
│   │       ├── deferred/   # Deferred-pass reference PNGs
│   │       └── debug/      # DebugPass overlay reference PNGs
│   ├── editor/             # Editor unit tests (run in CI, no GPU)
│   ├── ui/                 # Qt widget tests (QApplication from shared/test_main.cpp)
│   └── shared/             # Test infrastructure
│       └── TestVulkanShared.h/cpp  # GPU test fixture base class
├── AGENTS.md               # This file
├── CMakeLists.txt           # Root CMake build
├── CMakePresets.json        # CMake presets (default, release, vs2022)
├── Makefile                 # Convenience build wrapper
└── README.md               # Project introduction
```

---

## Practical Guidance

- Respect layer isolation; do not introduce cross-layer header includes where
  forbidden.
- Prefer UIEvents/EventQueue/Context-driven flows over direct calls across layers.
- Every cross-layer interaction MUST go through the isolation chain
  (UIEvents/EventQueue/Context) -- no shortcuts. If a function reaches
  across layers by including a forbidden header or calling a method on an
  object it doesn't own, that's a design violation.
- Use the event-driven pattern (Controller subscribes, Editor dispatches)
  instead of polling or Update() loops. Controllers should never tick
  per-frame; they react to discrete typed events from the EventQueue.
- Keep functions small and focused (single responsibility). If a function
  exceeds ~50 lines, split it. Long functions are a sign of hidden
  concerns that belong in separate helpers or classes.
- Keep classes lean. Prefer composition over inheritance. Avoid sprawling
  god classes that mix rendering, logic, and I/O. If a class has more
  than 8-10 public methods, question whether it's doing too much.
- If a file or function feels "clumsy", refactor it early. Technical debt
  compounds fast in rendering code, where debugging GPU state is already
  complex enough.
- Do not add global state unless a file already uses it and there is no
  alternative.
- When extending renderer behavior, keep GPU ownership inside Renderer or
  Data & Resource layer; avoid leaking Vulkan handles outward.
- Avoid formatting churn; keep edits minimal and localized.
- TDD: write tests first (RED), implement (GREEN), do NOT refactor beyond
  the current task's scope.
- Vulkan-HPP vk::raii: never call raw vkDestroy. Let RAII handle cleanup.
- Validation layers: test in Debug mode. Never suppress validation warnings
  without explicit justification in code comments.
- See `.github/instructions/test.instructions.md` for GPU test patterns
  (VulkanTestShared, reference-image regression, attachment conversion rules,
  scene/light scaling, Python verification).

### Temporal Accumulation Convention
- **Iteration** (`DrawFrame()` counter): DeferredRenderer::m_iteration, reset by Editor
  on scene changes via `RenderResetEvent` (see events.instructions.md).
- **Jitter**: 3D jitter (`RenderContext::jitter`). Point: `pos + radius*jitter`.
  Sun: UV-space `shadowUV + (jitter.x, jitter.y) * uvRadius`.
- **Accumulation** (`ComposePass`): In-place read-modify-write on ShadowIntensity
  with EMA blend: `mix(prev, sample, alpha)`.
- **Alpha**: 0 = FixedAlpha (1/8), 1 = MovingAvg (1/(n+1)).

### Debug Overlay Convention (issue #22)

Viewport gizmos and debug geometry are **retained and stateful**, not immediate-mode:
Qt is stateful, so debug objects are too.

- `DebugLine`, `DebugPoints` and `DebugMesh` are **first-class Scene objects** —
  pooled, serialized and selectable like any Mesh or Light, not per-frame commands.
- `DebugDrawBuilder` (editor) is the single flattener: it walks the Scene's three
  debug pools into a `DebugDrawList` on a **dirty flag** (`MarkDirty()` from
  `RenderResetEvent`, `Rebuild()` at the end of `Editor::Edit()`), never per frame.
  See editor.instructions.md for its partition / revision / world-space contracts.
- `DebugPass` (renderer) is the single consumer and runs **last, after post-AA**: it draws
  over the final shaded image with `LOAD_OP_LOAD` — `FXAAOutput` when FXAA is on,
  `ComposedOutput` when it is off, chosen by `SetTarget()` from `PipelineSignature` —
  reads but never writes the G-Buffer depth, and flips
  `eDepthTestEnable` as dynamic state to serve x-ray — at most 6 draws per frame
  regardless of primitive count. Every debug primitive is already antialiased
  analytically, so handing FXAA the scene alone keeps both correct. See renderer.instructions.md.
- Upload is **revision-gated**: an unchanged list re-uses the buffer a frame slot
  already holds and skips the memcpy entirely.
- **Hiding a debug object means not flattening it.** `DebugDrawList` has no
  per-primitive enable bit, so `DebugDrawBuilder::Rebuild()` drops objects whose
  `is_viewport && is_rendered` is false, before the x-ray partition is computed. The
  Outliner toggle already reaches it: `SceneController` → `RenderResetEvent` →
  `MarkDirty()`.
- **Smoothing is derived, never authored — and on by default.** Nothing stores a
  smooth bit: no scene property, no event, no op, no checkbox. `DebugDrawBuilder` is
  the one place that decides it, per primitive kind: a solid line and every cube edge
  get `DebugFlag::Smooth`, a **stippled** line does not (antialiasing would soften the
  hard ends a dash is made of), point sprites always do (their shape mask is an
  analytic distance, so a rhombus or circle is visibly stepped without it), and a
  `DebugMesh` wireframe never does — `PolygonMode::eLine` gives the fragment stage no
  distance to fade, so `debug_wire.frag` has no branch on the flag.
- A `DebugMesh` wireframe still needs a **MeshGPU** in the RenderCache, uploaded by the
  Editor through `UploadManager::UploadMeshData()` — `DebugMesh` is not a `Mesh`, so it
  cannot reuse `UploadMesh()`. Without it `DebugPass` skips the mesh silently.
- `CPUBuffer` is the permanent host-visible buffer type (named against `GPUBuffer`).
  **`StagingBuffer` is for uploads and downloads only** — never retain one as a
  permanent buffer object.
- The **frame driver publishes, passes read**: `DeferredRenderer::recordFrame()` writes
  `RenderCache`'s `CameraGPU` and `DebugCache` once per frame before any pass records;
  `GeometryPass` and `DebugPass` only bind them. A test driving a pass directly stands in
  for that driver — call `VulkanTestShared::PublishSceneCamera()` or the pass renders
  nothing.

---

## Coordinate System Convention (Z-up, +Y forward)

The entire codebase uses a **Z-up, +Y forward, right-hand** coordinate system
(Blender convention):

- Up direction: `+Z` (positive Z is world-space up)
- Forward direction: `+Y` (positive Y is forward/north)
- Right direction: `+X`
- Euler rotation storage: `m_rotation = (pitch=X, roll=Y, yaw=Z)`
- Rotation multiplication order: `T * Rz(yaw) * Rx(pitch) * Ry(roll) * S`
- `Transform::GetDirection()`: `d = Rz(yaw) · Rx(pitch) · Ry(roll) · (0,1,0)`
  (rotates forward vector)
- View space is **left-handed** (`GLM_FORCE_DEPTH_ZERO_TO_ONE`): `+Z` is forward
  from camera, NDC Z range is `[0, 1]`.
- Camera projection requires `proj[1][1] *= -1` to flip Y for Vulkan NDC
  (NDC Y=-1 at top, Y=+1 at bottom).
- `CameraController` spherical coordinates: elevation=`asin(dir.z)`,
  azimuth=`atan2(dir.x, dir.y)` (angle from +Y forward axis).
- Test helpers: `MakeTestCamera(w, h)` places camera at `(0,0,2)` with
  up=`(0,0,1)`.

**IMPORTANT**: OBJ files exported from Blender use Blender's native convention
(Z-up, +Y forward) — do NOT edit .obj files.

---

## Subagent Racing Prevention

Multiple parallel subagents can race on `cmake --build` or launching
`Neurus.exe` or the tests, causing file lock contention and false test failures. Moreover, subagent may use `git stash` to revert the changes, this will interrupt the editing of other subagents.

**Rules:**
1. **Only the master agent runs `cmake --build build/debug`** for the final
   verification. Subagents must NOT run builds or tests.
2. **Only one agent runs `ctest` or `Neurus.exe` at a time.** The EXE/DLL
   is locked by the first process; a second concurrent launch fails with
   "Access denied" or "file in use".
3. **Subagents may compile-check** individual files via `lsp_diagnostics`
   but must NOT invoke the build system.
4. **Never run `Neurus.exe` or the test binary from a subagent** — always
   let the master agent handle it.
5. **Never use `git stash` while editing files**

**Test working directory**: CTest runs every test with `WorkingDirectory = build/`
— `test/CMakeLists.txt` pins `gtest_discover_tests(WORKING_DIRECTORY
"${CMAKE_BINARY_DIR}")` so `res/` (copied there by a POST_BUILD step) always
resolves relative to the CWD. The binary itself lives one level down, in the
per-config directory the multi-config generator picks (`build/Debug/neurus_test`
on CI, `build/debug/neurus_test` locally). So run `ctest` from `build/`, or if
you launch the binary directly, `cd build` first and invoke it by path —
launching it from its own directory makes `res/` unresolvable and can write
stale reference images to the wrong path (e.g.
`D:\Projects\test\render\reference\`).


---

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, invoke the `skill` tool with `skill: "graphify"` before doing anything else.

Rules:
- Do NOT run graphify (update/rebuild/query) proactively. The graph is kept
  current automatically by a git post-commit/post-checkout hook (installed via
  `graphify hook install`, extended to run `graphify-out/post_rebuild_cleanup.py`
  after `_rebuild_code()` to de-noise the raw output: AST-noise removal,
  file/duplicate node merging, community labelling, GRAPH_REPORT.md + labels +
  HTML regeneration). Re-install the hooks in a fresh clone:
  `graphify hook install`, then re-append the cleanup call after
  `_rebuild_code(...)` in `.git/hooks/post-commit` and `.git/hooks/post-checkout`
  (local, untracked).
- Only run graphify when the user invokes the `/graphify` skill or explicitly
  asks for it. When they do, follow the skill's workflow (query/path/explain for
  codebase questions; update/rebuild only when the skill calls for it).
- Dirty graphify-out/ files are expected after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
