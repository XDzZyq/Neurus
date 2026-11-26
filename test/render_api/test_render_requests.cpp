#include "asset/ResourceManager.hpp"
#include "render_api/RenderRequests.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace neurus {

TEST(RenderRequestValues, LightSnapshotSurvivesSourceEditsAndDeletion)
{
	ResourceManager resources;
	auto light = resources.Load<Light>(LightType::SPOTLIGHT, 7.0f, glm::vec3(0.2f, 0.4f, 0.6f));
	light->SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	light->spot_cutoff = 0.75f;
	light->spot_outer_cutoff = 0.5f;
	light->use_shadow = false;
	const int uid = light->GetObjectID();
	const UpdateLightRequest request{uid, MakeLightSnapshot(*light)};

	light->SetPosition(glm::vec3(9.0f));
	light->light_power = 99.0f;
	resources.Clear();
	light.reset();

	EXPECT_EQ(request.light.objectId, uid);
	EXPECT_EQ(request.light.type, LightType::SPOTLIGHT);
	EXPECT_EQ(request.light.position, glm::vec3(1.0f, 2.0f, 3.0f));
	EXPECT_EQ(request.light.color, glm::vec3(0.2f, 0.4f, 0.6f));
	EXPECT_FLOAT_EQ(request.light.power, 7.0f);
	EXPECT_FLOAT_EQ(request.light.spotCutoff, 0.75f);
	EXPECT_FLOAT_EQ(request.light.spotOuterCutoff, 0.5f);
	EXPECT_FALSE(request.light.useShadow);
}

TEST(RenderRequestValues, LightingBatchOnlyIncludesVisibleRenderedLights)
{
	ResourceManager resources;
	auto visible = resources.Load<Light>(LightType::SUNLIGHT, 2.0f, glm::vec3(1.0f));
	auto hidden = resources.Load<Light>();
	hidden->is_viewport = false;
	auto excluded = resources.Load<Light>();
	excluded->is_rendered = false;
	const std::unordered_map<int, ResourceRef<Light>> lights = {
		{visible->GetObjectID(), visible},
		{hidden->GetObjectID(), hidden},
		{excluded->GetObjectID(), excluded},
		{-1, nullptr},
	};
	const UpdateLightingRequest request = MakeUpdateLightingRequest(lights);
	ASSERT_EQ(request.lights.size(), 1u);
	EXPECT_EQ(request.lights.at(visible->GetObjectID()).type, LightType::SUNLIGHT);
}

namespace {

class UploadTestShader final : public ShaderData
{
public:
	UploadTestShader() : ShaderData("upload test") {}
	ShaderType GetType() const override { return ShaderType::COMPUTE; }
};

} // namespace

TEST(RenderRequestValues, ShaderUploadOwnsBytesAndMetadataAfterSourceEditsAndDeletion)
{
	ResourceManager resources;
	auto shader = resources.Load<UploadTestShader>();
	auto& stage = shader->GetStage(ShaderType::COMPUTE);
	stage.spv = {0x07230203, 11, 22};
	stage.parsed.uniform_list.push_back({.name = "target", .type = ParaType::NONE,
	                                     .count = 1, .binding = 3, .actualType = "image2D"});
	stage.path = "source.comp";
	stage.code = "editable source";
	stage.parsed.Main = "editable function body";
	shader->BumpVersion();
	const int uid = shader->GetObjectID();
	const int allocations = resources.NextId();
	const UploadShaderRequest upload = MakeUploadShaderRequest(*shader);
	EXPECT_EQ(resources.NextId(), allocations);

	stage.spv[1] = 99;
	stage.parsed.uniform_list[0].name = "edited";
	shader->BumpVersion();
	const auto edited = MakeUploadShaderRequest(*shader);
	EXPECT_EQ(edited.shaderId, upload.shaderId);
	EXPECT_GT(edited.version, upload.version);
	EXPECT_EQ(edited.stages.at(ShaderType::COMPUTE).spv, (std::vector<uint32_t>{0x07230203, 99, 22}));
	resources.Clear();
	shader.reset();

	EXPECT_EQ(upload.shaderId, uid);
	EXPECT_EQ(upload.version, 1);
	EXPECT_EQ(upload.primaryType, ShaderType::COMPUTE);
	ASSERT_EQ(upload.stages.size(), 1u);
	const auto& uploaded = upload.stages.at(ShaderType::COMPUTE);
	EXPECT_EQ(uploaded.spv, (std::vector<uint32_t>{0x07230203, 11, 22}));
	ASSERT_EQ(uploaded.parsed.uniform_list.size(), 1u);
	EXPECT_EQ(uploaded.parsed.uniform_list[0].name, "target");
	EXPECT_TRUE(uploaded.parsed.Main.empty());
}

TEST(RenderRequestValues, CompiledGraphRequestStripsShaderReferencesAndKeepsIdentity)
{
	ResourceManager resources;
	auto shader = resources.Load<UploadTestShader>();
	shader->BumpVersion();

	RenderGraphData document;
	auto* node = document.AddNode("compose", CPUNode{.pass = "ComposePass"});
	ASSERT_NE(node, nullptr);
	node->data.shader = shader;

	const std::uint64_t originalHash = document.Hash();
	const int uid = shader->GetObjectID();
	const int version = shader->GetVersion();

	const CompileRenderGraphRequest request = MakeCompileRenderGraphRequest(document);

	ASSERT_TRUE(request.graph);
	ASSERT_EQ(request.graph->nodes.size(), 1u);
	EXPECT_FALSE(request.graph->nodes.front()->data.shader);
	EXPECT_NE(request.graph->nodes.front().get(), document.nodes.front().get());
	ASSERT_EQ(request.shaders.size(), 1u);
	ASSERT_TRUE(request.shaders.contains("compose"));
	EXPECT_EQ(request.shaders.at("compose").uid, uid);
	EXPECT_EQ(request.shaders.at("compose").version, version);
	EXPECT_EQ(request.hash, originalHash);
	// The original document keeps its reference; only the clone was stripped.
	EXPECT_TRUE(document.nodes.front()->data.shader);

	// Later source edits cannot reach the request clone.
	shader->BumpVersion();
	EXPECT_EQ(request.shaders.at("compose").version, version);
}

} // namespace neurus
