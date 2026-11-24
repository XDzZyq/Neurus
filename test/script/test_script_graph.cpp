#include <gtest/gtest.h>

#include "asset/ResourceManager.hpp"
#include "editor/Editor.hpp"
#include "editor/RenderManager.hpp"
#include "editor/events/GraphEvents.hpp"
#include "editor/events/OperationEvents.hpp"
#include "editor/events/ScriptEvents.hpp"
#include "script/ScriptContext.hpp"
#include "script/ScriptEngine.hpp"
#include "script/proxies/ProxyBase.hpp"

namespace neurus {
namespace {

template <typename Event>
concept CanEmitCheckedEvent = requires(const script::ProxyBase& proxy, Event event)
{
	script::EmitCheckedEvent(proxy, event);
};

static_assert(CanEmitCheckedEvent<RenderGraphCreateRequested>);
static_assert(!CanEmitCheckedEvent<UndoRequested>);      // No error slot.
static_assert(!CanEmitCheckedEvent<OperationRejected>); // Owns its error string.

class ScriptGraphTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		editor.Initialize();
		editor.NewScene();
		ASSERT_TRUE(engine.Initialize());
		engine.SetEventQueue(&editor.GetEventQueue());
		editor.UpdateScriptContext(context);
		ASSERT_TRUE(Run("import neurus\ng = neurus.render.graph"));
	}

	bool Run(const std::string& code) { return engine.RunStringChecked(code, &context); }

	Editor editor{nullptr};
	ScriptEngine engine;
	ScriptContext context;
};

TEST_F(ScriptGraphTest, ConsoleQueueCommitsAndReportsRejectedGraphEdits)
{
	engine.Submit("g.create()\ng.connect(g.nodes['compose'].outputs['outputComposed'], g.outputs['color'])");
	engine.ExecutePending(context);
	ASSERT_EQ(engine.LastRunStatus(), ScriptRunStatus::Ok) << engine.LastRunMessage();
	const auto history = editor.GetHistory().undo.size();
	engine.Submit("g.disconnect(g.outputs['color'])");
	engine.ExecutePending(context);
	EXPECT_NE(engine.LastRunStatus(), ScriptRunStatus::Ok);
	EXPECT_NE(engine.LastRunMessage().find("color"), std::string::npos);
	EXPECT_EQ(editor.GetHistory().undo.size(), history);
	EXPECT_TRUE(Run("assert g.outputs['color'].source.node == 'compose'")) << engine.LastRunMessage();
}

TEST_F(ScriptGraphTest, BypassAndRestoreUsesSocketViewsAndOneUndoEntry)
{
	ASSERT_TRUE(Run(R"PY(
assert g.uid is None and len(g.nodes) == 7 and len(g.edges) == 14
assert g.outputs["color"].source.node == "fxaa" and not g.validate()
g.create()
compose = g.nodes["compose"]
assert compose.kind == "ComposePass" and compose.shader is None
assert compose.inputs["gHDRColor"].source.node == "lighting"
g.connect(compose.outputs["outputComposed"], g.outputs["color"])
assert g.outputs["color"].source.name == "outputComposed"
assert g.nodes["fxaa"].inputs["gInput"].source.node == "compose"
assert compose.output_tags["outputComposed"].format == compose.outputs["outputComposed"].tag.format
)PY")) << engine.LastRunMessage();
	ASSERT_EQ(editor.GetHistory().undo.size(), 1u);
	ASSERT_TRUE(Run(R"PY(
g.connect(g.nodes["fxaa"].outputs["outputFXAA"], g.outputs["color"])
)PY")) << engine.LastRunMessage();
	ASSERT_EQ(editor.GetHistory().undo.size(), 2u);
	editor.GetOperations().Undo();
	EXPECT_TRUE(Run("assert g.outputs['color'].source.node == 'compose'")) << engine.LastRunMessage();
}

TEST_F(ScriptGraphTest, InvalidFinalWiringAndScriptExceptionsDiscardGraphWrites)
{
	ASSERT_TRUE(Run("g.create()"));
	const auto uid = editor.GetRenderManager().GraphData()->RequireObjectID();
	const auto history = editor.GetHistory().undo.size();
	EXPECT_FALSE(engine.EvalStringChecked("g.disconnect(g.outputs['color'])", &context, nullptr));
	EXPECT_NE(engine.LastRunMessage().find("color"), std::string::npos) << engine.LastRunMessage();
	EXPECT_FALSE(Run(R"PY(
g.connect(g.nodes["compose"].outputs["outputComposed"], g.outputs["color"])
raise RuntimeError("discard this graph edit")
)PY"));
	EXPECT_TRUE(Run("assert g.outputs['color'].source.node == 'fxaa'")) << engine.LastRunMessage();
	EXPECT_EQ(editor.GetRenderManager().GraphData()->RequireObjectID(), uid);
	EXPECT_EQ(editor.GetHistory().undo.size(), history);
}

TEST_F(ScriptGraphTest, RejectedRunStartSkipsPythonAndClosesOpenedScopes)
{
	bool reject = true;
	editor.GetEventQueue().Subscribe<ScriptRunStarted>([&](const auto&) {
		if (reject)
			throw std::runtime_error("could not start graph edit");
	});
	EXPECT_FALSE(Run("g.create()"));
	EXPECT_EQ(engine.LastRunMessage(), "could not start graph edit");
	EXPECT_EQ(engine.LastRunStatus(), ScriptRunStatus::ScriptError);
	reject = false;
	EXPECT_TRUE(Run("assert g.uid is None and g.create().uid is not None")) << engine.LastRunMessage();
	EXPECT_EQ(editor.GetHistory().undo.size(), 1u);
}

TEST_F(ScriptGraphTest, FailedCreationRestoresImplicitGraphAndDropsCreatedShader)
{
	const auto baseline = editor.GetResourceManager().Size();
	EXPECT_FALSE(Run(R"PY(
g.create().nodes["compose"].create_shader()
raise RuntimeError("discard creation")
)PY"));
	EXPECT_TRUE(Run("assert g.uid is None and g.nodes['compose'].shader is None")) << engine.LastRunMessage();
	EXPECT_EQ(editor.GetResourceManager().Size(), baseline);
	EXPECT_TRUE(editor.GetHistory().undo.empty());
}

TEST_F(ScriptGraphTest, RejectedGraphMutationsAndStaleShaderWritesReportAtTheCall)
{
	ASSERT_TRUE(Run("g.create()"));
	EXPECT_FALSE(Run(R"PY(
stage = g.nodes["compose"].create_shader().compute
try:
    g.add("ComposePass", id="compose")
except RuntimeError as error:
    duplicate_error = str(error)
)PY"));
	EXPECT_TRUE(Run(R"PY(
assert "Duplicate" in duplicate_error and "compose" in duplicate_error
for operation in (lambda: setattr(stage, "code", "stale"),
                  lambda: setattr(stage, "parsed", neurus.shader.ShaderStruct()),
                  lambda: stage.compile()):
    try:
        operation()
    except RuntimeError as error:
        assert "shader stage not found" in str(error)
    else:
        raise AssertionError("a stale shader stage accepted an edit")
)PY")) << engine.LastRunMessage();
}

TEST_F(ScriptGraphTest, RenamePreservesEdgesAndShaderAssignment)
{
	ASSERT_TRUE(Run(R"PY(
g.create()
s = g.nodes["compose"].create_shader()
other = g.add("ComposePass", id="other")
g.connect(g.nodes["lighting"].outputs["outputImage"], other.inputs["gHDRColor"])
other.shader = s
assert other.shader.uid == s.uid
renamed = g.rename("other", "postprocess")
g.connect(renamed.outputs["outputComposed"], g.outputs["color"])
assert g.outputs["color"].source.node == "postprocess"
assert any(edge.to_node == "postprocess" for edge in g.edges)
renamed.shader = None
assert renamed.shader is None and g.nodes["compose"].shader.uid == s.uid
)PY")) << engine.LastRunMessage();
}

TEST_F(ScriptGraphTest, RemoveThenAddWithTheSameNameInvalidatesSavedNodeAndSocket)
{
	ASSERT_TRUE(Run(R"PY(
g.create()
old = g.nodes["fxaa"]
old_output = old.outputs["outputFXAA"]
g.remove("fxaa")
new_node = g.add("FXAAPass", id="fxaa")
g.connect(g.nodes["compose"].outputs["outputComposed"], new_node.inputs["gInput"])
g.connect(new_node.outputs["outputFXAA"], g.outputs["color"])
for read in (lambda: old.kind, lambda: old_output.name):
    try:
        read()
    except (KeyError, ValueError, RuntimeError):
        pass
    else:
        raise AssertionError("a removed graph handle resolved to the replacement")
)PY")) << engine.LastRunMessage();
}

TEST_F(ScriptGraphTest, PooledShaderEditsAutoCompileAndRejectBadSource)
{
	ASSERT_TRUE(Run(R"PY(
g.create()
compose = g.nodes["compose"]
s = compose.create_shader()
assert s.uid == compose.create_shader().uid
original = s.compute.code
target = "vec4(composed, 1.0)"
assert target in original
red = original.replace(target, "vec4(composed * vec3(1.0, 0.05, 0.05), 1.0)")
neurus.data.shaders[s.uid].compute.code = red
assert s.compute.code == red
)PY")) << engine.LastRunMessage();
	const auto history = editor.GetHistory().undo.size();
	EXPECT_FALSE(Run(R"PY(
g.nodes['compose'].shader.compute.code = 'this is invalid GLSL'
shader_errors = [diagnostic.message for diagnostic in g.validate()]
try:
    s.compute.compile()
except RuntimeError as error:
    compile_error = str(error)
)PY"));
	EXPECT_EQ(editor.GetHistory().undo.size(), history);
	EXPECT_TRUE(Run("assert any(shader_errors) and \"compose\" in compile_error and s.compute.code == red")) << engine.LastRunMessage();
}

} // namespace
} // namespace neurus
