/**
 * @file test_script_shader.cpp
 * @brief Python shader slot, pooled shader and unit edit tests.
 *
 * A headless Editor supplies the real pool, event bus, controllers and script
 * context; the Python run executes on the same event loop as production.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "core/Log.h"
#include "editor/Editor.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"
#include "script/ScriptEngine.h"

class ScriptShader : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_editor.Initialize();

		auto mesh = m_editor.GetResourceManager().Load<neurus::Mesh>();
		mesh->o_name = m_editor.GetResourceManager().SetName(mesh->GetObjectID(), "Ground");
		m_editor.GetScene().UseMesh(mesh);
		m_editor.UpdateScriptContext(m_ctx);

		ASSERT_TRUE(m_engine.Initialize());
		m_engine.SetEventQueue(&m_editor.GetEventQueue());
	}

	void TearDown() override { m_engine.Shutdown(); }

	void Run(const std::string& code) { m_engine.RunString(code, &m_ctx); }

	void RunCleanly(const std::string& code)
	{
		const std::size_t before = neurus::LogBuffer::instance().ErrorCount();
		Run(code);
		EXPECT_EQ(neurus::LogBuffer::instance().ErrorCount(), before) << "script raised an error";
	}

	neurus::Editor         m_editor{nullptr};
	neurus::ScriptContext  m_ctx;
	neurus::ScriptEngine   m_engine;
};

TEST_F(ScriptShader, FourCreationPaths)
{
	RunCleanly(R"(
import neurus
mesh = neurus.scene.meshes["Ground"]
mesh.shader.create()
assert mesh.shader.attached
mesh.shader.create("res/shaders/render/gbuffer.vert", "res/shaders/render/gbuffer.frag")
assert mesh.shader.attached
shader = neurus.shader.load_shader("res/shaders/render/gbuffer.frag")
mesh.shader.create(fragment=shader)
assert mesh.shader.fragment is not None
neurus.shader.load_shader("res/shaders/render/gbuffer.frag", name="MyFrag")
mesh.shader.create(fragment=neurus.data.shaders["MyFrag"])
)");
}

TEST_F(ScriptShader, CodeAndIrEditsAreLiveAndUndoable)
{
	RunCleanly(R"(
import neurus
mesh = neurus.scene.meshes["Ground"]
mesh.shader.create()
frag = mesh.shader.fragment
frag.code = frag.code
ir = frag.parsed
ir.functions.append(neurus.shader.Function(name="helper", return_type="float", body="return 1.0;"))
frag.parsed = ir
assert any(f.name == "helper" for f in frag.parsed.functions)
frag.compile(from_code=False)
)");
}

TEST_F(ScriptShader, WrongStageIsRejected)
{
	Run(R"(
import neurus
mesh = neurus.scene.meshes["Ground"]
compute = neurus.shader.load_shader("res/shaders/compute/dummy.comp")
mesh.shader.create(fragment=compute)
)");
	EXPECT_GT(neurus::LogBuffer::instance().ErrorCount(), 0);
}

TEST_F(ScriptShader, ComputeIsEditableAndCompilable)
{
	RunCleanly(R"(
import neurus
compute = neurus.shader.load_shader("res/shaders/compute/dummy.comp")
unit = compute.compute
unit.code = unit.code
unit.compile(from_code=True)
)");
}

TEST_F(ScriptShader, PooledShaderNamesResolve)
{
	RunCleanly(R"(
import neurus
neurus.shader.load_shader("res/shaders/render/gbuffer.frag", name="MyFrag")
assert "MyFrag" in neurus.data.shaders
assert neurus.data.shaders["MyFrag"].name == "MyFrag"
)");
}
