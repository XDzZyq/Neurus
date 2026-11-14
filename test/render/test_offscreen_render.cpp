/**
 * @file test_offscreen_render.cpp
 * @brief The headless DeferredRenderer path: no surface, no swapchain.
 *
 * Everything else in the GPU suite drives passes directly. This file is the only
 * place the production renderer runs without a window, so it is what proves the
 * Linux product binary's frame path: construct the offscreen target, draw, and
 * capture the OverlayOutput attachment.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/DeferredRenderer.h"
#include "render/SharedQueue.h"
#include "render/UploadManager.h"
#include "render/resources/LightingCache.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace
{

constexpr uint32_t kWidth = 96;
constexpr uint32_t kHeight = 64;

std::size_t CountPngs(const std::filesystem::path& directory)
{
	std::size_t count = 0;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(directory, error))
	{
		if (error)
			break;
		if (entry.path().extension() == ".png")
			++count;
	}
	return count;
}

} // namespace

class OffscreenRenderTest : public VulkanTestShared
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
			m_graphicsQueueFamily, vk::Extent2D{kWidth, kHeight});

		m_uploader = std::make_unique<neurus::UploadManager>(
			*m_device, m_physicalDevices[m_selectedPdIndex], *m_queue, m_graphicsQueueFamily);
		m_renderer->GetRenderCache().SetLightingCache(
			m_uploader->CreateLightingCache(*m_device, m_physicalDevices[m_selectedPdIndex]));
		m_renderer->SetUploadManager(m_uploader.get());
	}

	/// Draws one frame of a camera-only scene, so the overlay output is written.
	void DrawOneFrame()
	{
		neurus::Scene scene;
		auto camera = CreateTestCamera(kWidth, kHeight);
		scene.UseCamera(camera);
		scene.ActivateCamera(camera->GetObjectID());

		// The snapshot and the context must outlive the call: the renderer reads
		// through ctx.snapshot while it records.
		neurus::RenderFrameSnapshot snapshot{};
		neurus::RenderContext context{};
		PublishFrame(m_renderer->GetRenderCache(), context, snapshot, &scene);
		ASSERT_NO_THROW(m_renderer->DrawFrame(context));
		m_renderer->WaitIdle();
	}

	std::unique_ptr<neurus::UploadManager> m_uploader;
	std::unique_ptr<neurus::DeferredRenderer> m_renderer;
};

TEST_F(OffscreenRenderTest, CaptureBeforeAnyFrameWritesNothing)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}
	ASSERT_NO_FATAL_FAILURE(CreateRenderer());

	const std::filesystem::path directory =
		std::filesystem::temp_directory_path() / "neurus-offscreen-empty";
	std::filesystem::remove_all(directory);

	// No frame has written OverlayOutput: this must report the problem and leave
	// no file behind, rather than capture an empty attachment as a black PNG.
	m_renderer->CaptureScreenshot(false, directory);

	EXPECT_FALSE(std::filesystem::exists(directory))
		<< "a capture with no drawn frame must not create output";
}

TEST_F(OffscreenRenderTest, DrawsAndCapturesWithoutASurface)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}
	ASSERT_NO_FATAL_FAILURE(CreateRenderer());

	EXPECT_EQ(m_renderer->GetExtent().width, kWidth);
	EXPECT_EQ(m_renderer->GetExtent().height, kHeight);

	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	const std::filesystem::path directory =
		std::filesystem::temp_directory_path() / "neurus-offscreen-frame";
	std::filesystem::remove_all(directory);
	m_renderer->CaptureScreenshot(false, directory);

	EXPECT_EQ(CountPngs(directory), 1u) << "one frame must produce exactly one capture";

	std::uintmax_t bytes = 0;
	for (const auto& entry : std::filesystem::directory_iterator(directory))
	{
		bytes += entry.file_size();
	}
	EXPECT_GT(bytes, 0u) << "the capture must not be an empty file";

	// Resize follows the window path: attachments are dropped, the extent changes
	// and the next frame still renders and captures.
	m_renderer->HandleResize(64, 48);
	EXPECT_EQ(m_renderer->GetExtent().width, 64);
	EXPECT_EQ(m_renderer->GetExtent().height, 48);
	ASSERT_NO_FATAL_FAILURE(DrawOneFrame());

	std::filesystem::remove_all(directory);
}
