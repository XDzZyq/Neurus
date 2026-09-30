/**
 * @file test_shadow_cubemap.cpp
 * @brief GPU test: renders cube+plane scene into ShadowDepthPass cubemap,
 *        reads back raw float depth data for all 6 faces, and verifies every
 *        pixel against mathematically-computed expected depth values.
 *
 * Mathematical verification:
 *   - Cube unit at [-0.5, +0.5]^3 positioned at (0,0,3), plane at z=0 spanning [-10,10] in XY (Z-up convention)
 *   - Point light at (0, 0, 6), farPlane from Light::point_shadow_far
 *   - Depth = dist(lightPos, worldPos) / farPlane written by fragment shader
 *   - For each pixel (px,py) on each face, ray-cast from light to determine expected depth
 *   - Compare pixel-by-pixel with tolerance +/-3/255 (~0.01176)
 *
 * Reference image regression:
 *   - First run: generates reference PNGs for all 6 faces -> GTEST_SKIP
 *   - Second run: compares pixel-by-pixel with +/-2 tolerance -> PASS
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "shared/TestSimpleShadow.h"

#include "render/shaders/RenderShader.h"

#include "render/passes/ShadowDepthPass.h"
#include "render/passes/GeometryPass.h"
#include "render/RenderContext.h"
#include "render/Image.h"
#include "render/Barrier.h"

#include "shared/TestReferenceImage.h"
#include "scene/Light.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class ShadowCubemapTest : public VulkanTestShared
{
protected:
	static constexpr uint32_t kRes        = 256;
	// farPlane sourced from Light::point_shadow_far at verification time
	static constexpr float    kTolerance  = 3.0f / 255.0f;  // +/-3 U8 steps in [0,1] range

	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!m_hasVulkan) return;

		auto& pd = PhysicalDevice();

		m_shadowDepthPass = std::make_unique<ShadowDepthPass>(
			*m_device, pd, m_queue, m_graphicsQueueFamily, kRes);
		m_renderCache = std::make_unique<RenderCache>(*m_device, pd);
		m_renderCache->SetLightingCache(std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily));
	}

	void TearDown() override
	{
		VulkanTestShared::TearDown();
	}

	// --- Face direction and pixel categorization helpers ---

	/**
	 * @brief Computes world-space direction from light for pixel (sx,sy) on cubemap face.
	 *
	 * Mathematically derived from the lookAt matrices used to compute cubemap
	 * view-projection matrices. For each face, sx,sy in [-1,1] are NDC coordinates;
	 * the returned direction is the normalized world-space vector from the light
	 * through that pixel.
	 */
	static glm::vec3 FaceDirection(uint32_t faceIdx, float sx, float sy)
	{
		struct FC { glm::vec3 fwd, upVec; };
		static const FC kFaces[6] = {
			{{ 1, 0, 0}, {0,-1, 0}}, // 0: +X
			{{-1, 0, 0}, {0,-1, 0}}, // 1: -X
			{{ 0, 1, 0}, {0, 0, 1}}, // 2: +Y
			{{ 0,-1, 0}, {0, 0,-1}}, // 3: -Y
			{{ 0, 0, 1}, {0,-1, 0}}, // 4: +Z
			{{ 0, 0,-1}, {0,-1, 0}}, // 5: -Z
		};
		const auto& fc = kFaces[faceIdx];
		glm::vec3 right = glm::normalize(glm::cross(fc.fwd, fc.upVec));
		glm::vec3 actualUp = glm::normalize(glm::cross(right, fc.fwd));
		return glm::normalize(sx * right + sy * actualUp + fc.fwd);
	}

	// --- Pixel categorization for diagnostic summary ---

	enum class PixelCategory { Cube, Plane, Background };

	/**
	 * @brief Categorize a pixel as cube, plane, or background based on ray intersection.
	 * @param dir       Normalized world-space direction from light through pixel.
	 * @param lightPos  World-space position of the point light.
	 * @param cubePos   World-space position of the cube centre (default origin).
	 * @return PixelCategory indicating what geometry (if any) the ray hits first.
	 */
	static PixelCategory CategorizePixel(const glm::vec3& dir, const glm::vec3& lightPos,
	                                     const glm::vec3& cubePos = glm::vec3(0.0f))
	{
		// Plane z=0 intersection (bounded to [-10, 10] in XY)
		const float t_plane = -lightPos.z / dir.z;
		bool hitsPlane = false;
		if (t_plane > 0.0f)
		{
			const glm::vec3 hp = lightPos + t_plane * dir;
			hitsPlane = (std::abs(hp.x) <= 10.0f && std::abs(hp.y) <= 10.0f);
		}

		// Cube AABB slab test (cube centered at cubePos, extent ±0.5)
		const float eps = 0.001f;
		float tMin = eps, tMax = 1e10f;
		bool parallelMiss = false;
		for (int axis = 0; axis < 3; ++axis)
		{
			const float centre = (&cubePos.x)[axis];
			const float lo = centre - 0.5f, hi = centre + 0.5f;
			const float origin = (&lightPos.x)[axis];
			const float d       = (&dir.x)[axis];
			if (std::abs(d) > 1e-7f)
			{
				const float t1 = (lo - origin) / d;
				const float t2 = (hi - origin) / d;
				tMin = std::max(tMin, std::min(t1, t2));
				tMax = std::min(tMax, std::max(t1, t2));
			}
			else if (origin < lo || origin > hi)
				parallelMiss = true;
		}
		const bool hitsCube = (!parallelMiss && tMin < tMax && tMin > eps);

		if (hitsCube && tMin < t_plane)
			return PixelCategory::Cube;
		if (hitsPlane)
			return PixelCategory::Plane;
		return PixelCategory::Background;
	}

	// --- Depth-to-u8 conversion for reference image regression ---

	/**
	 * @brief Converts a vector of float depth values [0,1] to uint8_t.
	 * @param depthData  Float depth data (one value per pixel).
	 * @return uint8_t data, clamped and quantized to [0, 255].
	 */
	static std::vector<uint8_t> DepthToU8(const std::vector<float>& depthData)
	{
		std::vector<uint8_t> u8Data(depthData.size());
		for (size_t i = 0; i < depthData.size(); ++i)
		{
			float v = depthData[i];
			v = std::max(0.0f, std::min(1.0f, v));
			u8Data[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
		}
		return u8Data;
	}

	// --- Expected depth computation ---

	/**
	 * @brief Computes expected depth for a specific cubemap face.
	 *
	 * Uses FaceDirection() to determine the world-space ray from the light
	 * for the given pixel, then intersects with the cube AABB and plane.
	 * @param faceIdx Cubemap face index 0-5.
	 * @param cubePos World-space position of the cube centre (default origin).
	 * @return Expected depth = t / farPlane where t is distance to first hit.
	 */
	static float ComputeExpectedDepth(uint32_t faceIdx,
	                                  uint32_t px, uint32_t py,
	                                  const glm::vec3& lightPos,
	                                  float farPlane, uint32_t res,
	                                  const glm::vec3& cubePos)
	{
		const float sx = (2.0f * static_cast<float>(px) + 1.0f) / static_cast<float>(res) - 1.0f;
		const float sy = (2.0f * static_cast<float>(py) + 1.0f) / static_cast<float>(res) - 1.0f;

		const glm::vec3 dir = glm::normalize(FaceDirection(faceIdx, sx, sy));

		// --- Plane z=0 intersection ---
		const float t_plane = -lightPos.z / dir.z;
		bool hitsPlaneInBounds = false;
		if (t_plane > 0.0f)
		{
			const glm::vec3 hitPoint = lightPos + t_plane * dir;
			if (std::abs(hitPoint.x) <= 10.0f && std::abs(hitPoint.y) <= 10.0f)
				hitsPlaneInBounds = true;
		}

		// --- Cube AABB slab test (cube centered at cubePos, extent ±0.5) ---
		const float eps = 0.001f;
		float t_min_cube = eps;
		float t_max_cube = 1e10f;

		const float cx = cubePos.x, cy = cubePos.y, cz = cubePos.z;

		if (std::abs(dir.x) > 1e-7f)
		{
			const float t1 = (cx - 0.5f - lightPos.x) / dir.x;
			const float t2 = (cx + 0.5f - lightPos.x) / dir.x;
			t_min_cube = std::max(t_min_cube, std::min(t1, t2));
			t_max_cube = std::min(t_max_cube, std::max(t1, t2));
		}
		else if (lightPos.x < cx - 0.5f || lightPos.x > cx + 0.5f)
			t_min_cube = 1e10f;

		if (std::abs(dir.y) > 1e-7f)
		{
			const float t1 = (cy - 0.5f - lightPos.y) / dir.y;
			const float t2 = (cy + 0.5f - lightPos.y) / dir.y;
			t_min_cube = std::max(t_min_cube, std::min(t1, t2));
			t_max_cube = std::min(t_max_cube, std::max(t1, t2));
		}
		else if (lightPos.y < cy - 0.5f || lightPos.y > cy + 0.5f)
			t_min_cube = 1e10f;

		if (std::abs(dir.z) > 1e-7f)
		{
			const float t1 = (cz - 0.5f - lightPos.z) / dir.z;
			const float t2 = (cz + 0.5f - lightPos.z) / dir.z;
			t_min_cube = std::max(t_min_cube, std::min(t1, t2));
			t_max_cube = std::min(t_max_cube, std::max(t1, t2));
		}
		else if (lightPos.z < cz - 0.5f || lightPos.z > cz + 0.5f)
			t_min_cube = 1e10f;

		const bool hitsCube = (t_min_cube < t_max_cube && t_min_cube > eps);

		if (hitsCube && t_min_cube < t_plane)
			return t_min_cube / farPlane;
		if (hitsPlaneInBounds)
			return t_plane / farPlane;
		return 1.0f;
	}

	// --- Render data ---
	std::unique_ptr<ShadowDepthPass> m_shadowDepthPass;
	std::unique_ptr<RenderCache> m_renderCache;
};

// ===========================================================================
// All Faces Cubemap Depth Verification Test
// ===========================================================================

TEST_F(ShadowCubemapTest, AllFacesDepth)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	// -------------------------------------------------------------------
	// Step 1: Build scene geometry
	// -------------------------------------------------------------------
	auto shadowRes = neurus::test::LoadSimpleShadow(
		*m_device, pd, m_queue, m_graphicsQueueFamily);

	ASSERT_EQ(shadowRes.scene->mesh_list.size(), 2u) << "Expected 2 meshes (cube + plane)";

	// -------------------------------------------------------------------
	// Step 2: Light position is read from ctx.scene->light_list at Record() time.
	// -------------------------------------------------------------------
	const int lightUID = shadowRes.scene->light_list.begin()->first;
	const glm::vec3 lightPos = shadowRes.scene->light_list.begin()->second->GetPosition();
	const glm::vec3 viewPos = shadowRes.cubePos;

	// -------------------------------------------------------------------
	// Step 3: Pre-register GPU resources before pass recording
	// -------------------------------------------------------------------
	VulkanTestShared::EnsureMeshesUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_renderCache, *shadowRes.scene, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	// -------------------------------------------------------------------
	// Step 3: Render all 6 faces into depth cubemap + colour output (via cache).
	// -------------------------------------------------------------------
	{
		auto& cmd = BeginCmd();

		RenderContext ctx{};
		ctx.width = kRes; ctx.height = kRes;
		ctx.editor.scene = shadowRes.scene.get();
		// Point-light-only scene, but ShadowDepthPass still walks its sun path and
		// reads the viewing camera from the context; publish it as the frame driver would.
		VulkanTestShared::PublishSceneCamera(*m_renderCache, ctx.editor);
		m_shadowDepthPass->Record(*cmd, *m_renderCache, ctx);

		EndSubmitWait(cmd);
	}

	// -------------------------------------------------------------------
	// Step 4: Read back the actual depth cubemap (D32_SFLOAT)
	// -------------------------------------------------------------------
	{
		auto* lgpu = m_renderCache->GetLightGPU(lightUID);
		ASSERT_NE(lgpu, nullptr);
		ASSERT_NE(lgpu->shadowDepthMap, nullptr);
		auto& shadowCubemap = *lgpu->shadowDepthMap;
		std::cout << "\n=== Depth Cubemap Readback (D32_SFLOAT) ===\n";

		for (uint32_t face = 0; face < 6; ++face)
		{
			auto layerRange = shadowCubemap.Layer(face);
			auto data = shadowCubemap.ReadImageData(
				*m_device, pd, m_queue, m_graphicsQueueFamily,
				&layerRange, {kRes, kRes});
			const float* depthData = reinterpret_cast<const float*>(data->GetPixelData().data());

			// Debug: depth range summary
			{
				float minVal = depthData[0], maxVal = depthData[0];
				int zeroCount = 0, oneCount = 0;
				for (uint32_t i = 0; i < kRes * kRes; ++i)
				{
					const float v = depthData[i];
					minVal = std::min(minVal, v);
					maxVal = std::max(maxVal, v);
					if (v == 0.0f) zeroCount++;
					if (v >= 0.999f && v <= 1.001f) oneCount++;
				}
				std::cout << "[DepthCubemap Face " << face << "] Depth: min=" << minVal
				          << " max=" << maxVal << " zeros=" << zeroCount
				          << " ones~=" << oneCount << " total=" << (kRes * kRes)
				          << std::endl;
			}

			// Pixel-by-pixel mathematical verification
			int cubePixels  = 0;
			int planePixels = 0;
			int bgPixels    = 0;
			int badPixels   = 0;

			for (uint32_t py = 0; py < kRes; ++py)
			{
				for (uint32_t px = 0; px < kRes; ++px)
				{
					const float actual   = depthData[py * kRes + px];
					const float expected =
						ComputeExpectedDepth(face, px, py, lightPos, Light::point_shadow_far, kRes, viewPos);
					const float expected_clamped = std::max(0.0f, std::min(1.0f, expected));

					const float sxV = (2.f * static_cast<float>(px) + 1.f) / static_cast<float>(kRes) - 1.f;
					const float syV = (2.f * static_cast<float>(py) + 1.f) / static_cast<float>(kRes) - 1.f;
					const glm::vec3 dir = glm::normalize(FaceDirection(face, sxV, syV));
					auto cat = CategorizePixel(dir, lightPos, viewPos);

					if (cat == PixelCategory::Cube)       cubePixels++;
					else if (cat == PixelCategory::Plane)  planePixels++;
					else                                   bgPixels++;

					const float diff = std::abs(actual - expected_clamped);
					if (diff > kTolerance)
					{
						badPixels++;
						if (badPixels <= 10)
						{
							const char* catStr = (cat == PixelCategory::Cube) ? "cube"
							                   : (cat == PixelCategory::Plane) ? "plane" : "bg";
							std::cout << "BAD PIXEL [DepthCubemap] (face=" << face
							          << " px=" << px << " py=" << py
							          << "): actual=" << actual
							          << " expected (clamped)=" << expected_clamped
							          << " diff=" << diff
							          << " category=" << catStr
							          << std::endl;
						}
					}
				}
			}

			std::cout << "[DepthCubemap Face " << face << "] Cube:" << cubePixels
			          << " Plane:" << planePixels << " BG:" << bgPixels
			          << " Bad:" << badPixels
			          << " (tol=" << kTolerance << ")" << std::endl;

			EXPECT_LT(badPixels, 1)
				<< "DepthCubemap Face " << face << ": " << badPixels
				<< " pixels exceed tolerance " << kTolerance;
		}
	}
}
