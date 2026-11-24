/**
 * @file test_render_graph_pipeline.cpp
 * @brief The production renderer compiles its scene graph from the CPU document.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.hpp"

#include "core/RenderConfig.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ImageData.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "shader/ComputeShader.hpp"
#include "shader/ShaderLibrary.hpp"
#include "shader/ShaderParser.hpp"
#include "render/DeferredRenderer.hpp"
#include "render/SharedQueue.hpp"
#include "render/UploadManager.hpp"
#include "render_api/RenderRequests.hpp"
#include "render/resources/LightingCache.hpp"
#include "scene/Camera.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace
{

constexpr uint32_t kWidth  = 96;
constexpr uint32_t kHeight = 64;

neurus::RenderGraphData::NodeType* FindNode(neurus::RenderGraphData& graph,
                                           const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

neurus::RenderGraphData::SocketOutT* FindOutput(neurus::RenderGraphData::NodeType& node,
                                               const std::string& name)
{
	for (const auto& output : node.outputs)
	{
		if (output->name == name)
			return output.get();
	}
	return nullptr;
}

neurus::RenderGraphData::SocketInT* FindInput(neurus::RenderGraphData::NodeType& node,
                                             const std::string& name)
{
	for (const auto& input : node.inputs)
	{
		if (input->name == name)
			return input.get();
	}
	return nullptr;
}

} // anonymous namespace

class RenderGraphPipelineTest : public VulkanTestShared
{
protected:
	void TearDown() override
	{
		m_renderer.reset();
		m_uploader.reset();
		VulkanTestShared::TearDown();
	}

	void CreateRenderer()
	{
		m_renderer = std::make_unique<neurus::DeferredRenderer>(
			*m_device, m_physicalDevices[m_selectedPdIndex], *m_queue,
			m_graphicsQueueFamily, vk::Extent2D{kWidth, kHeight}, m_defaults);

		m_uploader = std::make_unique<neurus::UploadManager>(
			*m_device, m_physicalDevices[m_selectedPdIndex], *m_queue, m_graphicsQueueFamily);
		m_renderer->GetRenderCache().SetLightingCache(
			m_uploader->CreateLightingCache(*m_device, m_physicalDevices[m_selectedPdIndex]));
		m_renderer->SetUploadManager(m_uploader.get());
	}

	std::unique_ptr<neurus::RenderGraphData> DefaultDocument() const
	{
		return m_defaults->Graph()->Clone();
	}

	void PostDocument(std::unique_ptr<neurus::RenderGraphData> graph)
	{
		neurus::CompileRenderGraphRequest request;
		request.graph = std::move(graph);
		m_renderer->Post(neurus::RenderRequest{std::move(request)});
	}

	void DrawOneFrame(const neurus::RenderConfig* config = nullptr)
	{
		neurus::Scene scene;
		auto camera = CreateTestCamera(kWidth, kHeight);
		scene.UseCamera(camera);
		scene.ActivateCamera(camera->RequireObjectID());

		neurus::RenderFrameSnapshot snapshot{};
		neurus::RenderContext context{};
		PublishFrame(m_renderer->GetRenderCache(), context, snapshot, &scene, config);
		ASSERT_NO_THROW(m_profile = m_renderer->DrawFrame(context));
		m_renderer->WaitIdle();
	}

	neurus::FrameProfile m_profile;
	std::unique_ptr<neurus::UploadManager> m_uploader;
	std::unique_ptr<neurus::DeferredRenderer> m_renderer;
	std::shared_ptr<const neurus::BuiltInRenderData> m_defaults =
		std::make_shared<const neurus::BuiltInRenderData>();
};

TEST_F(RenderGraphPipelineTest, DefaultDocumentCompilesAndDraws)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());

	// No project graph has been published. The first frame resolves the cached
	// built-in document and builds the normal seven-pass scene pipeline.
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
	EXPECT_TRUE(m_renderer->LastGraphReport().compiled);
	EXPECT_EQ(m_renderer->ScenePassCount(), 7u);
	EXPECT_TRUE(m_renderer->LastGraphReport().diagnostics.empty());

	// Only the final topology in a batch matters. A redundant compile would
	// clear the existing resource plans, including this probe.
	auto& cache = m_renderer->GetRenderCache();
	cache.PlanResource("test/probe", {neurus::GraphFormat::Rgba16f, neurus::GraphUsage::ColorSampled});
	PostDocument(std::make_unique<neurus::RenderGraphData>());
	PostDocument(DefaultDocument());
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	EXPECT_TRUE(cache.HasResource("test/probe"));
	EXPECT_TRUE(m_renderer->LastGraphReport().compiled);
	EXPECT_EQ(m_renderer->ScenePassCount(), 7u);  // the document always has FXAA
	EXPECT_TRUE(m_renderer->LastGraphReport().diagnostics.empty());
}

TEST_F(RenderGraphPipelineTest, FxaaStaysInTheDocumentWhateverTheConfig)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());
	ASSERT_NO_FATAL_FAILURE(PostDocument(DefaultDocument()));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
	ASSERT_EQ(m_renderer->ScenePassCount(), 7u);

	neurus::RenderConfig config{};
	config.r_aa = neurus::AAAlg::FXAA;
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame(&config));
	EXPECT_EQ(m_renderer->ScenePassCount(), 7u);
}

TEST_F(RenderGraphPipelineTest, BypassingAndRestoringFxaaChangesActualDispatches)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());
	neurus::RenderConfig config;
	config.r_aa = neurus::AAAlg::FXAA;
	auto graph = DefaultDocument();
	auto draw = [&]
	{
		PostDocument(graph->Clone());
		DrawOneFrame(&config);
	};
	auto fxaaDispatches = [&]
	{
		uint32_t count = 0;
		for (const auto& pass : m_profile.passes)
			if (pass.name == "FXAAPass")
				count += pass.dispatches;
		return count;
	};
	ASSERT_NO_FATAL_FAILURE(draw());
	EXPECT_EQ(m_renderer->ScenePassCount(), 7u);
	EXPECT_GT(fxaaDispatches(), 0u);

	graph->sceneOutputs["color"] = {"compose", "outputComposed"};
	ASSERT_NO_FATAL_FAILURE(draw());
	EXPECT_EQ(graph->nodes.size(), 7u);
	EXPECT_EQ(m_renderer->ScenePassCount(), 6u);
	EXPECT_EQ(fxaaDispatches(), 0u);
	EXPECT_FALSE(m_renderer->GetRenderCache().HasResource("fxaa/outputFXAA"));

	graph->sceneOutputs["color"] = {"fxaa", "outputFXAA"};
	ASSERT_NO_FATAL_FAILURE(draw());
	EXPECT_EQ(m_renderer->ScenePassCount(), 7u);
	EXPECT_GT(fxaaDispatches(), 0u);
}

TEST_F(RenderGraphPipelineTest, EditedDocumentChangesTopology)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());

	auto document = DefaultDocument();
	neurus::RenderGraphData::NodeType* ssao = FindNode(*document, "ssao");
	neurus::RenderGraphData::NodeType* geometry = FindNode(*document, "geometry");
	neurus::RenderGraphData::NodeType* lighting = FindNode(*document, "lighting");
	ASSERT_NE(ssao, nullptr);
	ASSERT_NE(geometry, nullptr);
	ASSERT_NE(lighting, nullptr);

	// U_AO still has to be fed by something; the topology test only cares that
	// removing a node changes the executed pass list.
	ASSERT_TRUE(document->RemoveNode(ssao));
	ASSERT_TRUE(document->Connect(FindOutput(*geometry, "gAlbedo"),
	                             FindInput(*lighting, "U_AO")));

	ASSERT_NO_FATAL_FAILURE(PostDocument(std::move(document)));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	EXPECT_TRUE(m_renderer->LastGraphReport().compiled);
	EXPECT_EQ(m_renderer->ScenePassCount(), 6u);
}

TEST_F(RenderGraphPipelineTest, BadDocumentKeepsPreviousGraph)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	ASSERT_NO_FATAL_FAILURE(CreateRenderer());
	ASSERT_NO_FATAL_FAILURE(PostDocument(DefaultDocument()));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());
	const size_t previousPasses = m_renderer->ScenePassCount();
	ASSERT_EQ(previousPasses, 7u);

	auto bad = DefaultDocument();
	bad->nodes[0]->outputs[0]->data.tag.reset();

	ASSERT_NO_FATAL_FAILURE(PostDocument(std::move(bad)));
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	EXPECT_FALSE(m_renderer->LastGraphReport().compiled);
	EXPECT_FALSE(m_renderer->LastGraphReport().diagnostics.empty());
	EXPECT_EQ(m_renderer->ScenePassCount(), previousPasses);
}

TEST_F(RenderGraphPipelineTest, UploadedCustomComposeShaderChangesPixelsAndRetainsOwnedPayload)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	ASSERT_NO_FATAL_FAILURE(CreateRenderer());
	std::shared_ptr<neurus::ComputeShader> shader = neurus::ShaderLibrary::LoadComputeShader(
		"custom compose", "res/shaders/compute/compose.comp.glsl");
	ASSERT_NE(shader, nullptr);
	m_resources.Register(shader);
	auto& unit = shader->GetStage(neurus::ShaderType::COMPUTE);
	const std::string declarations = unit.code.substr(0, unit.code.find("void main("));
	const auto compileColor = [&](const char* color)
	{
		unit.code = declarations + "void main() {\n"
			"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
			"if (any(greaterThanEqual(p, imageSize(outputComposed)))) return;\n"
			"imageStore(outputComposed, p, vec4(" + color + ", 1.0));\n}\n";
		unit.parsed = neurus::ShaderParser::ParseShaderCode(unit.code, neurus::ShaderType::COMPUTE);
		unit.spv = neurus::ShaderLibrary::Compile(unit, neurus::ShaderType::COMPUTE, "custom compose");
		unit.BumpVersion();
		shader->BumpVersion();
	};
	compileColor("1.0, 0.0, 0.0");
	ASSERT_FALSE(unit.spv.empty());
	const auto redUpload = neurus::MakeUploadShaderRequest(*shader);
	compileColor("0.0, 1.0, 0.0");
	ASSERT_FALSE(unit.spv.empty());
	const auto greenUpload = neurus::MakeUploadShaderRequest(*shader);
	ASSERT_NE(redUpload.stages.at(neurus::ShaderType::COMPUTE).spv,
	          greenUpload.stages.at(neurus::ShaderType::COMPUTE).spv);

	auto document = DefaultDocument();
	FindNode(*document, "compose")->data.shader = shader;
	m_renderer->Post(neurus::RenderRequest{redUpload});
	PostDocument(std::move(document));

	const auto drawColor = [&](const uint16_t red, const uint16_t green)
	{
		DrawOneFrame();
		ASSERT_TRUE(m_renderer->LastGraphReport().compiled);
		auto& output = m_renderer->GetRenderCache().GetAttachment(neurus::AttachmentName::ComposedOutput,
		                                                        vk::Extent2D{kWidth, kHeight});
		const auto image = output.ReadImageData(*m_device, PhysicalDevice(), *m_queue, m_graphicsQueueFamily);
		ASSERT_NE(image, nullptr);
		const auto* pixel = reinterpret_cast<const uint16_t*>(image->GetPixelData().data());
		EXPECT_EQ(pixel[0], red);
		EXPECT_EQ(pixel[1], green);
		EXPECT_EQ(pixel[2], 0u);
	};
	drawColor(0x3c00, 0);
	// Shader uploads alone invalidate the compiled graph.
	m_renderer->Post(neurus::RenderRequest{greenUpload});
	drawColor(0, 0x3c00);
	m_renderer->Post(neurus::RenderRequest{redUpload});
	drawColor(0x3c00, 0);
}
