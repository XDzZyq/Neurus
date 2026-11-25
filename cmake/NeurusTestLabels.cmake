# CTest includes this after Google Test discovers both executables.
# The layer identifies the subsystem under test. See docs/build/test.md for
# the four severity levels and examples of selecting tests.

function(neurus_test_labels layer severity)
	foreach(key IN LISTS ARGN)
		set("labels_${key}" "Layer::${layer};Severity::${severity}" PARENT_SCOPE)
	endforeach()
endfunction()

# Suite defaults, shared by the full and CPU-only executables.
neurus_test_labels(Application Medium AppArgs PreferencesTest)
neurus_test_labels(Application High SessionCommand VulkanContextTest)
neurus_test_labels(Asset Medium ImageDataTest MeshDataTest CpuPorts ShaderDataSourceTest
	UIComponentRoundtrip)
neurus_test_labels(Asset High CpuGraphValidation DefaultProject GraphComponent
	RenderGraphData RenderGraphDocument SerializeOptional ShaderDataPersistence
	ShaderInterface)
neurus_test_labels(Asset Critical ProjectRoundtrip)
neurus_test_labels(CLI Medium CliParse ProcessLaunchTest TerminalBinding)
neurus_test_labels(Command Medium CommandDispatcher CommandJson CommandTypes)
neurus_test_labels(Core Low LogBufferTest)
neurus_test_labels(Core Medium PresetTest ResourceNameRegistry ResourceTypeIndex
	SelectionsTest)
neurus_test_labels(Core High GraphClone GraphMorphism GraphTest ResourceManagerTest UIDTest)
neurus_test_labels(Editor Medium CameraControllerTest DebugDrawBuilder DebugDrawList
	DebugPropertyTest EditorOverlayRedraw EditorViewport EditorViewportTest
	GizmoDrawBuilderTest GizmoMath InputTest ObjectControllerTest RenderConfigControllerTest
	OptionalRenderConfigControllerTest SceneControllerTest TransformGizmoControllerTest
	TransformGizmoTest ViewportCameraTest)
neurus_test_labels(Editor High EditorCommandAdapterTest EditorRenderGraph
	EditorSceneLifecycleTest EventBusTyped OperationManagerTest OperationSerializationTest
	OptionalConfigPersistence PassShaderEditing ScriptControllerTest ShaderControllerTest
	ShaderCreateTest ShaderCreateUndoTest TypedEventQueueTest)
neurus_test_labels(IPC High Frame IpcRoundtripTest SessionRegistryTest)
neurus_test_labels(IPC Critical SessionNameValidation)
neurus_test_labels(Platform Medium PlatformPathsTest PlatformSystemTest ProcessMac)
neurus_test_labels(Renderer Medium BufferLayoutTest DebugPassTest
	DescriptorSetLayoutBuilderTest GizmoPassTest HaltonSequenceTest MaterialTest
	MeshRenderTest OverlayBlendTest PassCountersTest PassTypeTest RenderContextSettings
	ResourceTag ShadowAlphaTest TextureTest)
neurus_test_labels(Renderer High ArrayBufferGpuTest BufferGpuTest BufferTest
	DescriptorManagerTest FrameMailboxTest FXAATest GeometryPassTest
	GeometryPipelineWarmTest GraphUploaderTest IBLConversionTest IBLRenderTest ImageTest
	LightingPassTest ModelRenderTest MultiLightShadowTest NullScenePassTest
	OffscreenRenderTest PassBindings PassFactoryTest PickPixelTest RenderCacheResourcesTest
	RenderCacheTest RenderGraphPipelineTest RenderGraphTest RenderRequestExecutorTest
	RenderRequestProcessorTest RenderRequestQueueTest RenderThreadTest SceneWiringTest
	ScreenshotTest ShaderReflection ShadowCubemapTest ShadowIntensityTest SharedQueueTest
	SSAOTest SunShadowDepthTest SunShadowIntensityTest SyncObjectsDeviceTest
	TimelineSemaphoreTest)
neurus_test_labels(Renderer Critical DeferredShadingTest)
neurus_test_labels(RenderAPI High RenderFrameSnapshotTest RenderRequestValues)
neurus_test_labels(Scene Medium Camera DebugLine DebugPoints Light Mesh ObjectIDTest
	SceneTest SpriteTest Transform3D)
neurus_test_labels(Scene High SceneCameraActivation SceneIntegrationTest)
neurus_test_labels(Scene Critical SceneSerialize)
neurus_test_labels(Script Low ScriptConsoleBuffer)
neurus_test_labels(Script Medium ScriptBindings ScriptPreload)
neurus_test_labels(Script High ScriptEditorWrites ScriptEngine ScriptGraphTest
	ScriptHotReload ScriptShader ScriptWrites)
neurus_test_labels(Script Critical ScriptFileRoundTripTest ScriptIsolation ScriptSandbox)
neurus_test_labels(Shader Medium ShaderParserTest ShaderPorts ShaderUnitImport)
neurus_test_labels(Shader High BuiltInRenderDataTest)
neurus_test_labels(UI Low CodeHighlighterPython I18nTest LogFilterProxyTest LogModelTest
	POCatalogTest ScalarSliderModel)
neurus_test_labels(UI Medium DebugPropertiesTest DockIdentityTest FloatingWindowTest
	PreferencesDialogTest RenderConfigPanelModel RunScriptMenu ScriptConsolePanelTest
	UIContextSettings UIContextShape UIEventsScript UIEventsTest ViewportTest)
neurus_test_labels(UI High LayoutRestoreTest ShaderEditorPanelTest)

# Individual contracts whose owning layer or regression impact differs.
neurus_test_labels(Application Critical SessionCommand.DirtySessionStaysOpenUntilSaved
	SessionCommand.OpeningAProjectRequiresACleanStateOrForce
	SessionCommand.ReplacingADirtyProjectRequiresForce)
neurus_test_labels(Application Medium
	SerializeOptional.PreferencesCorruptFileKeepsLiveValues
	SerializeOptional.PreferencesKeepsPresentKeyWhenOtherAbsent
	SerializeOptional.PreferencesWithNoKnownKeyIsOkWithDefaults)
neurus_test_labels(Asset Critical
	GraphComponent.CorruptFirstGraphDoesNotDiscardSiblingResources
	SerializeOptional.ShippedProjectResavesWithEveryOldKey
	ShaderDataPersistence.MalformedShaderEntryDoesNotDiscardOtherProjectResources
	ShaderDataPersistence.SavingLiveCompileFailurePreservesEditableSource)
neurus_test_labels(Core Critical ResourceManagerTest.PoolSaveLoadRoundtrip)
neurus_test_labels(Editor Critical
	OperationSerializationTest.HistoryComponent_SaveLoad_BothStacks)
neurus_test_labels(IPC Critical IpcRoundtripTest.NoResponseBeforeTheMainThreadDrains
	IpcRoundtripTest.WrongTokenIsUnauthorizedAndExecutorNeverRuns
	SessionRegistryTest.RegistryEntryIsOwnerOnly)
neurus_test_labels(Platform Medium ProcessLaunchTest.ExitedChildIsNotAlive
	ProcessLaunchTest.SpawningANonExecutableFails)
neurus_test_labels(Renderer Critical
	FrameMailboxTest.RequestStop_WakesAnAcquireThatIsAlreadyBlocked
	OffscreenRenderTest.DrawsAndCapturesWithoutASurface
	RenderGraphPipelineTest.DefaultDocumentCompilesAndDraws
	RenderRequestExecutorTest.HeldEnvironmentTable_KeepsTheReplacedEnvironmentAlive
	RenderThreadTest.AnExceptionInDrawFrameEndsTheLoopAndBreaksPendingPromises
	RenderThreadTest.ControlsNeverRunWhileAFrameIsBeingDrawn)
neurus_test_labels(Shader High ShaderParserTest.GenerateShader_Idempotent_AllFiles)

foreach(test_name IN LISTS neurus_test_TESTS neurus_cpu_test_TESTS)
	string(REGEX REPLACE "^Neurus\\.(CPU\\.)?" "" case_name "${test_name}")
	string(REGEX MATCH "^[^.]+" suite "${case_name}")
	string(REGEX REPLACE "/[0-9]+$" "" suite "${suite}")
	string(REGEX REPLACE "^.*/" "" suite "${suite}")
	if(DEFINED "labels_${case_name}")
		set(labels "${labels_${case_name}}")
	elseif(DEFINED "labels_${suite}")
		set(labels "${labels_${suite}}")
	else()
		message(FATAL_ERROR "Assign a layer and severity to GoogleTest suite '${suite}' (test '${test_name}') in ${CMAKE_CURRENT_LIST_FILE}.")
	endif()
	set_tests_properties("${test_name}" PROPERTIES LABELS "${labels}")
endforeach()
