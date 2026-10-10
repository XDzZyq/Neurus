# Graph Report - D:\Projects\Neurus  (2026-10-10)

## Corpus Check
- 137 files · ~5,270 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1054 nodes · 2020 edges · 56 communities (55 shown, 1 thin omitted)
- Extraction: 98% EXTRACTED · 2% INFERRED · 0% AMBIGUOUS · INFERRED: 31 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Preferences|Preferences]]
- [[_COMMUNITY_Swapchain|Swapchain]]
- [[_COMMUNITY_GPU Image Core|GPU Image Core]]
- [[_COMMUNITY_Descriptor & Shadow Pipeline|Descriptor & Shadow Pipeline]]
- [[_COMMUNITY_LogDelegate|LogDelegate]]
- [[_COMMUNITY_Shader Compilation|Shader Compilation]]
- [[_COMMUNITY_Image & Barrier System|Image & Barrier System]]
- [[_COMMUNITY_Main Window & UI|Main Window & UI]]
- [[_COMMUNITY_Debug Primitives & Transform|Debug Primitives & Transform]]
- [[_COMMUNITY_Shader Struct Parsing|Shader Struct Parsing]]
- [[_COMMUNITY_Editor & Context|Editor & Context]]
- [[_COMMUNITY_Editor & Property UI|Editor & Property UI]]
- [[_COMMUNITY_Texture & Material|Texture & Material]]
- [[_COMMUNITY_Debug Primitives & Transform (alt)|Debug Primitives & Transform (alt)]]
- [[_COMMUNITY_POCatalog|POCatalog]]
- [[_COMMUNITY_Transform Math|Transform Math]]
- [[_COMMUNITY_Debug Primitives & Transform (alt)|Debug Primitives & Transform (alt)]]
- [[_COMMUNITY_Pass System|Pass System]]
- [[_COMMUNITY_Project Management|Project Management]]
- [[_COMMUNITY_Editor & Property UI (alt)|Editor & Property UI (alt)]]
- [[_COMMUNITY_Scene & Camera Controllers|Scene & Camera Controllers]]
- [[_COMMUNITY_SceneController|SceneController]]
- [[_COMMUNITY_Editor & Property UI (alt)|Editor & Property UI (alt)]]
- [[_COMMUNITY_RenderConfigPanel|RenderConfigPanel]]
- [[_COMMUNITY_IBL Push Constants|IBL Push Constants]]
- [[_COMMUNITY_Deterministic RNG|Deterministic RNG]]
- [[_COMMUNITY_Render Pipeline & SSAO|Render Pipeline & SSAO]]
- [[_COMMUNITY_Shadow Eval Constants|Shadow Eval Constants]]
- [[_COMMUNITY_Vulkan Context|Vulkan Context]]
- [[_COMMUNITY_SceneController (alt)|SceneController (alt)]]
- [[_COMMUNITY_Descriptor & Shadow Pipeline (alt)|Descriptor & Shadow Pipeline (alt)]]
- [[_COMMUNITY_LogModel|LogModel]]
- [[_COMMUNITY_Editor & Property UI (alt)|Editor & Property UI (alt)]]
- [[_COMMUNITY_OutlinerRow|OutlinerRow]]
- [[_COMMUNITY_Editor & Property UI (alt)|Editor & Property UI (alt)]]
- [[_COMMUNITY_Outliner|Outliner]]
- [[_COMMUNITY_Debug Primitives & Light|Debug Primitives & Light]]
- [[_COMMUNITY_RenderGraph|RenderGraph]]
- [[_COMMUNITY_OutlinerRow (alt)|OutlinerRow (alt)]]
- [[_COMMUNITY_LogModel (alt)|LogModel (alt)]]
- [[_COMMUNITY_Descriptor & Buffer Layout|Descriptor & Buffer Layout]]
- [[_COMMUNITY_GPU Image Core (alt)|GPU Image Core (alt)]]
- [[_COMMUNITY_SceneController (alt)|SceneController (alt)]]
- [[_COMMUNITY_ColorButton|ColorButton]]
- [[_COMMUNITY_MacOSSurface|MacOSSurface]]
- [[_COMMUNITY_Editor & Property UI (alt)|Editor & Property UI (alt)]]
- [[_COMMUNITY_Input System|Input System]]
- [[_COMMUNITY_Input|Input]]
- [[_COMMUNITY_LogFilterProxy|LogFilterProxy]]
- [[_COMMUNITY_PreferencesDialog|PreferencesDialog]]
- [[_COMMUNITY_Shader Compilation (alt)|Shader Compilation (alt)]]
- [[_COMMUNITY_Buffer Layout|Buffer Layout]]
- [[_COMMUNITY_Buffer Hierarchy|Buffer Hierarchy]]
- [[_COMMUNITY_ShaderOperations|ShaderOperations]]
- [[_COMMUNITY_SyncObjects|SyncObjects]]
- [[_COMMUNITY_Debug Primitives & Light (alt)|Debug Primitives & Light (alt)]]

## God Nodes (most connected - your core abstractions)
1. `Init()` - 58 edges
2. `RenderCache` - 51 edges
3. `ControllerContext` - 50 edges
4. `Editor` - 49 edges
5. `SceneController` - 49 edges
6. `ShaderStruct` - 36 edges
7. `Image` - 34 edges
8. `Scene` - 32 edges
9. `MakeEvent()` - 29 edges
10. `Retranslate()` - 29 edges

## Surprising Connections (you probably didn't know these)
- `InitVulkan()` --calls--> `CreatePlatformSurface()`  [INFERRED]
  src/app/Application.cpp → src/platform/PlatformSurface.cpp
- `OnMeshImport()` --calls--> `MakePortableAssetPath()`  [INFERRED]
  src/editor/Editor.cpp → src/asset/data/AssetPath.cpp
- `GetShadowIntensityArray()` --references--> `Image`  [EXTRACTED]
  src/render/RenderCache.cpp → src/render/Barrier.cpp
- `ResourceKey()` --calls--> `AttachmentNameToString()`  [INFERRED]
  src/render/render_graph/RenderGraph.cpp → src/render/RenderCache.cpp
- `CaptureAllAttachments()` --calls--> `AttachmentNameToString()`  [INFERRED]
  src/render/Screenshot.cpp → src/render/RenderCache.cpp

## Import Cycles
- None detected.

## Communities (56 total, 1 thin omitted)

### Community 0 - "Preferences"
Cohesion: 0.05
Nodes (50): DefaultPath(), project(), Serializable, ConfigComponent(), Load(), HistoryComponent(), ResourceComponent(), SceneComponent() (+42 more)

### Community 1 - "Swapchain"
Cohesion: 0.08
Nodes (43): Controllers, DeferredRenderer, Editor, AddDefaultDebugObjects(), ApplyViewportToActiveCamera(), BeginLoad(), CreateDefaultScene(), DropSceneGpuResources() (+35 more)

### Community 2 - "GPU Image Core"
Cohesion: 0.08
Nodes (49): IsAbsolutePath(), MakePortableAssetPath(), ResolveAssetPath(), ConvertHalfToU8(), EnsureDirectory(), HalfToFloat(), ImageData(), LoadFromPath() (+41 more)

### Community 3 - "Descriptor & Shadow Pipeline"
Cohesion: 0.06
Nodes (32): AttachmentConfig, AttachmentName, Remove(), DebugDrawList, EnvironmentGPU, LightGPU, LightingCache, MeshGPU (+24 more)

### Community 4 - "LogDelegate"
Cohesion: 0.08
Nodes (28): sizeHint(), AddButtonRect(), createEditor(), editorEvent(), paint(), populateTypeCombo(), updateEditorGeometry(), CodeEditor() (+20 more)

### Community 5 - "Shader Compilation"
Cohesion: 0.07
Nodes (31): AddFace(), GetIndexCount(), GetVertexCount(), LoadObj(), LoadObjFromString(), ReloadContent(), RenderShader, S_Const (+23 more)

### Community 6 - "Image & Barrier System"
Cohesion: 0.07
Nodes (31): AccessFlags2, Buffer, createBuffer(), findMemoryType(), CPUBuffer(), GPUBuffer(), Map(), Unmap() (+23 more)

### Community 7 - "Main Window & UI"
Cohesion: 0.07
Nodes (28): CDockWidget, DockWidgetArea, eventFilter(), ScalarSlider(), setValue(), value(), PanelType, NativeWindowHandle (+20 more)

### Community 8 - "Debug Primitives & Transform"
Cohesion: 0.08
Nodes (23): DebugLine, DebugPoints, DebugSegment, AppendCubeEdges(), AppendDebugLine(), AppendDebugMesh(), AppendDebugPoints(), IsVisible() (+15 more)

### Community 9 - "Shader Struct Parsing"
Cohesion: 0.10
Nodes (20): Args, ShaderEvents, ParaType, ADD_TYPE(), DefFunc(), DefStruct(), ParseArgs(), ParseType() (+12 more)

### Community 10 - "Editor & Context"
Cohesion: 0.13
Nodes (22): Application, ApplyTargetFps(), BuildProject(), InitEditor(), InitRenderer(), InitVulkan(), NewFrameSignals(), OnProjectNew() (+14 more)

### Community 11 - "Editor & Property UI"
Cohesion: 0.16
Nodes (27): Init(), AppendDefault(), GetStageUnit(), OnCodeEdited(), OnCodeRestored(), OnCompileShader(), OnCreateShader(), OnFieldAddRestored() (+19 more)

### Community 12 - "Texture & Material"
Cohesion: 0.11
Nodes (22): MatDataType, MaterialRes, MatParaType, computeMipLevels(), createFromPixelData(), createSampler(), ForAttachment(), FromData() (+14 more)

### Community 13 - "Debug Primitives & Transform (alt)"
Cohesion: 0.16
Nodes (26): IsLightObject(), LightingRebuilt(), Mutated(), OnCameraFovChanged(), OnCameraPoseChanged(), OnDebugLineSmoothChanged(), OnDebugLineStippleChanged(), OnDebugLineWidthChanged() (+18 more)

### Community 14 - "POCatalog"
Cohesion: 0.13
Nodes (24): Catalog, LanguageInfo, QByteArray, QLatin1String, QList, QTextDocument, Language, I18n (+16 more)

### Community 15 - "Transform Math"
Cohesion: 0.13
Nodes (14): RecordCameraPose(), Camera(), ChangeCamPersp(), ChangeCamRatio(), RecomputeMatrices(), SetPosition(), SetTarPos(), GlmSerialization (+6 more)

### Community 16 - "Debug Primitives & Transform (alt)"
Cohesion: 0.09
Nodes (21): CameraPose, OnDebugProjectionModeChanged(), OnEnvironmentIntensityChanged(), OnLightShadowChanged(), OnMeshMaterialChanged(), SceneOperations, MakeEvent(), CameraFovChanged (+13 more)

### Community 17 - "Pass System"
Cohesion: 0.16
Nodes (21): Graph, Disconnect(), NodeT, Pass, ColorAttachmentCount(), ColorLoadOpFor(), ColorStoreOpFor(), DepthLoadOpFor() (+13 more)

### Community 18 - "Project Management"
Cohesion: 0.16
Nodes (12): AddPosition(), BuildLineRows(), BuildPointRows(), BuildPositionTable(), BuildSharedRows(), ConnectSignals(), DebugProperties(), EmitPositions() (+4 more)

### Community 19 - "Editor & Property UI (alt)"
Cohesion: 0.15
Nodes (15): ItemFlags, data(), addSection(), buildTree(), flags(), headerData(), index(), nodeString() (+7 more)

### Community 20 - "Scene & Camera Controllers"
Cohesion: 0.23
Nodes (17): CameraPushEvent, CameraResizeEvent, CameraRotateEvent, CameraSlideEvent, CameraZoomEvent, NotifyCameraChanged(), OnCameraPush(), OnCameraResize() (+9 more)

### Community 21 - "SceneController"
Cohesion: 0.15
Nodes (17): CameraTargetChanged, OnCameraTargetChanged(), OnEnvironmentRotationChanged(), OnMeshShadowChanged(), OnObjectDeleteRequested(), OnObjectDeselected(), OnObjectSelected(), RecordSelection() (+9 more)

### Community 22 - "Editor & Property UI (alt)"
Cohesion: 0.17
Nodes (13): ComputeShader, GetContext(), EditorContext, ComposePass(), CreateDescriptorSetLayout(), GetIO(), GizmoPass(), LightingPass() (+5 more)

### Community 23 - "RenderConfigPanel"
Cohesion: 0.17
Nodes (10): Retranslate(), SetFormRowLabel(), CameraProperties(), setObjectId(), setMeshPath(), EnvironmentProperties(), MeshProperties(), CameraProperties (+2 more)

### Community 24 - "IBL Push Constants"
Cohesion: 0.16
Nodes (14): WriteDescriptors(), ComputePass, CreateEquirectSampler(), dispatchCompute(), Generate(), IBLPass(), IBLPushConstants, maxStep (+6 more)

### Community 25 - "Deterministic RNG"
Cohesion: 0.19
Nodes (10): KernelSampleGpu, kMaxKernelSamples, kNoiseEntryCount, NoiseEntryGpu, DeterministicRNG, rng_state, GenerateKernel(), GenerateNoise() (+2 more)

### Community 26 - "Render Pipeline & SSAO"
Cohesion: 0.23
Nodes (13): ConfigureCommonState(), CreateLayout(), DebugPass(), BuildPipeline(), CreateBilinearSampler(), FXAAPass(), ConfigureGBufferPipeline(), CreatePerMeshPipeline() (+5 more)

### Community 27 - "Shadow Eval Constants"
Cohesion: 0.20
Nodes (15): ShadowEvalPushConstants, alpha, bias, farPlane, frameCount, jitterX, jitterY, jitterZ (+7 more)

### Community 28 - "Vulkan Context"
Cohesion: 0.21
Nodes (12): CreateInstance(), findGraphicsQueueFamily(), findGraphicsQueueFamilyWithPresent(), findTransferQueueFamily(), InitDevice(), InitQueue(), selectOptionalFeatures(), selectPhysicalDeviceIndex() (+4 more)

### Community 29 - "SceneController (alt)"
Cohesion: 0.14
Nodes (14): LightStructChanged(), OnLightColorChanged(), OnLightCutoffChanged(), OnLightOuterCutoffChanged(), OnLightPowerChanged(), OnLightRadiusChanged(), EventBus, IEventQueue (+6 more)

### Community 30 - "Descriptor & Shadow Pipeline (alt)"
Cohesion: 0.22
Nodes (13): DescriptorSetLayoutBuilder, CreateCameraLayout(), CreateSSBOLayout(), createSSBOResources(), MakeFaceVPs(), ShadowDepthPass(), CreateSunDescriptorSetLayout(), CreateSunShadowSampler() (+5 more)

### Community 31 - "LogModel"
Cohesion: 0.19
Nodes (11): FormatLogTimestamp(), BuildToolbar(), LoadLogPanelStyle(), LogPanel(), MaybeScrollToBottom(), OnAutoScrollToggled(), OnClearClicked(), OnExportClicked() (+3 more)

### Community 32 - "Editor & Property UI (alt)"
Cohesion: 0.35
Nodes (12): initializer_list, addComboRow(), addSpinRow(), BuildAmbientOcclusionSection(), BuildLightingSection(), BuildPipelineSection(), BuildPostProcessingSection(), BuildShadowsSection() (+4 more)

### Community 33 - "OutlinerRow"
Cohesion: 0.24
Nodes (11): LoadOutlinerStyle(), OutlinerRow(), setEyeBtnColor(), setObject(), setRenderBtnColor(), setSelectionMode(), setVisibilities(), OutlinerRow (+3 more)

### Community 34 - "Editor & Property UI (alt)"
Cohesion: 0.31
Nodes (10): setModelData(), collectExpandedPaths(), indexFromPath(), indexPathOf(), populateSections(), setShowEmptyState(), ShaderEditorPanel(), QTreeView (+2 more)

### Community 35 - "Outliner"
Cohesion: 0.24
Nodes (11): AddCategoryGroup(), EnsureRowPool(), keyPressEvent(), Outliner(), RetranslateRow(), Populate(), ProfilingPanel(), Execute() (+3 more)

### Community 36 - "Debug Primitives & Light"
Cohesion: 0.24
Nodes (7): CosineToDeg(), DegToCosine(), LightProperties(), setCutoff(), setOuterCutoff(), setShadowEnabled(), LightProperties

### Community 37 - "RenderGraph"
Cohesion: 0.24
Nodes (7): GPUProfiler, EndPass(), Record(), WriteSunDescriptors(), BeginPass(), PassStats, string_view

### Community 38 - "OutlinerRow (alt)"
Cohesion: 0.29
Nodes (7): retranslate(), ProfilingHead(), ProfilingRow(), setHidden(), QTreeWidget, QTreeWidgetItem, ProfilingRow

### Community 39 - "LogModel (alt)"
Cohesion: 0.28
Nodes (8): LogBuffer, LogEntry, rowCount(), SourceLength(), Refresh(), setShowCreateButton(), LogModel, UIContext

### Community 40 - "Descriptor & Buffer Layout"
Cohesion: 0.33
Nodes (6): Allocate(), Build(), CalculatePoolSizes(), DescriptorSet(), DescriptorSetLayout(), DescriptorManager

### Community 41 - "GPU Image Core (alt)"
Cohesion: 0.47
Nodes (9): AcquireNextImage(), chooseExtent(), choosePresentMode(), chooseSurfaceFormat(), createImageViews(), Present(), Recreate(), Swapchain() (+1 more)

### Community 42 - "SceneController (alt)"
Cohesion: 0.25
Nodes (8): ForDebugObject(), OnDebugColorChanged(), OnDebugOpacityChanged(), OnDebugXRayChanged(), DebugColorChanged, DebugOpacityChanged, DebugXRayChanged, Fn

### Community 43 - "ColorButton"
Cohesion: 0.39
Nodes (8): ColorButton(), PickColor(), setColor(), UpdateSwatch(), ToQColor(), ToVec4(), ColorButton, QColor

### Community 44 - "MacOSSurface"
Cohesion: 0.33
Nodes (6): CAMetalLayer, NeurusMetalView, -initWithFrame, -isOpaque, -layerClass, NSView

### Community 45 - "Editor & Property UI (alt)"
Cohesion: 0.52
Nodes (7): BuildHeader(), BuildTransformEditor(), BuildTypeSubpanels(), PropertyPanel(), SetEnabled(), ShowTypeSubpanel(), PropertyPanel

### Community 46 - "Input System"
Cohesion: 0.33
Nodes (6): ApplyFieldEdit(), S_Func, S_IO, S_PushConstant, S_StructDef, S_Uniform

### Community 47 - "Input"
Cohesion: 0.33
Nodes (5): GetModifiers(), GetMouseButton(), Modifiers, MouseButton, Input

### Community 48 - "LogFilterProxy"
Cohesion: 0.33
Nodes (3): LevelFilter, setLevelFilter(), LogFilterProxy

### Community 49 - "PreferencesDialog"
Cohesion: 0.40
Nodes (3): PreferencesDialog(), SyncFrom(), PreferencesDialog

### Community 50 - "Shader Compilation (alt)"
Cohesion: 0.40
Nodes (3): SetOptimizationLevel(), ShaderCompiler(), ShaderCompiler

### Community 51 - "Buffer Layout"
Cohesion: 0.60
Nodes (4): GetBindingDescription(), GetFormatSize(), GetStride(), BufferLayout

### Community 52 - "Buffer Hierarchy"
Cohesion: 0.40
Nodes (4): UniformBuffer, CameraUBOData, m_data(), CameraGPU

### Community 54 - "SyncObjects"
Cohesion: 0.50
Nodes (4): SyncObjects, sync_fence(), sync_semaphore(), device

### Community 55 - "Debug Primitives & Light (alt)"
Cohesion: 0.67
Nodes (4): LightType, Light(), ParseLightName(), SpriteType

## Knowledge Gaps
- **104 isolated node(s):** `ShaderCreateRequested`, `AttachmentConfig`, `DockWidgetArea`, `PanelType`, `CDockWidget` (+99 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **1 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Editor` connect `Swapchain` to `Preferences`, `Descriptor & Shadow Pipeline`, `Shader Compilation`, `LogModel (alt)`, `Debug Primitives & Transform`, `Editor & Context`, `Editor & Property UI`, `Transform Math`, `Editor & Property UI (alt)`?**
  _High betweenness centrality (0.177) - this node is a cross-community bridge._
- **Why does `Image` connect `GPU Image Core` to `IBL Push Constants`, `Descriptor & Shadow Pipeline`, `Texture & Material`, `Image & Barrier System`?**
  _High betweenness centrality (0.152) - this node is a cross-community bridge._
- **Why does `UID` connect `Editor & Property UI` to `Preferences`, `OutlinerRow`, `GPU Image Core`, `Swapchain`, `Outliner`, `Shader Compilation`, `Debug Primitives & Transform`, `Debug Primitives & Transform (alt)`, `Editor & Property UI (alt)`, `Transform Math`, `Scene & Camera Controllers`, `Editor & Property UI (alt)`, `SceneController (alt)`?**
  _High betweenness centrality (0.143) - this node is a cross-community bridge._
- **What connects `ShaderCreateRequested`, `AttachmentConfig`, `DockWidgetArea` to the rest of the system?**
  _104 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Preferences` be split into smaller, more focused modules?**
  _Cohesion score 0.05141242937853107 - nodes in this community are weakly interconnected._
- **Should `Swapchain` be split into smaller, more focused modules?**
  _Cohesion score 0.0784313725490196 - nodes in this community are weakly interconnected._
- **Should `GPU Image Core` be split into smaller, more focused modules?**
  _Cohesion score 0.07993966817496229 - nodes in this community are weakly interconnected._