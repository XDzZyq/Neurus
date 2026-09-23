/**
 * @file test_sun_shadow_depth.cpp
 * @brief GPU test: renders a quad into sun-light orthographic shadow depth map,
 *        reads back D32 depth, verifies pixel-by-pixel against mathematical expectation.
 *
 * Z-up convention: +Y=forward, +Z=up. Sun dir=(0,1,0)=+Y, light at origin,
 * ortho field=2.5, near=-10, far=10.
 * Camera: eye = center - sunDir * farPlane = (0,-10,0), looks at center=(0,0,0).
 * GLM_FORCE_DEPTH_ZERO_TO_ONE → left-handed view (Vz = +y + 10).
 * Quad at y=-5 (5 units from eye toward center), Vz = 5.
 * NDC Z = (Vz - near) / (far - near) = (5 - (-10)) / 20 = 15/20 = 0.75
 * Quad spans full field to fill shadow map.
 * Tolerance: +/-3/255. Reference: first run SKIP, second PASS.
 */

#include <gtest/gtest.h>
#include "shared/TestVulkanShared.h"

#include "render/shaders/RenderShader.h"
#include "render/passes/ShadowDepthPass.h"
#include "render/RenderContext.h"
#include "render/RenderCache.h"
#include "render/Image.h"
#include "render/Barrier.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "asset/data/MeshData.h"
#include "render/buffers/VertexBuffer.h"
#include "render/buffers/IndexBuffer.h"
#include "render/passes/GeometryPass.h"
#include "shared/TestReferenceImage.h"
#include "core/Log.h"
#include <glm/glm.hpp>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

using namespace neurus;

class SunShadowDepthTest : public VulkanTestShared
{
protected:
	static constexpr uint32_t kRes   = ShadowDepthPass::kSunResolution;
	static constexpr float    kField = Light::sun_shadow_field;   // 2.5
	static constexpr float    kNear  = Light::sun_shadow_near;    // -10
	static constexpr float    kFar   = Light::sun_shadow_far;     // 30
	static constexpr float    kEyeDist = Light::sun_depth_range; // 10
	static constexpr float    kQuadY = -5.f;   // world-space y (quad at Y=-5, 5 units forward from eye — Z-up)
	static constexpr float    kQSize = 2.5f;   // half-width (quad = 5x5, fills full field)
	static constexpr float    kTol   = 3.f / 255.f;

	void SetUp() override { VulkanTestShared::SetUp(); if (!m_hasVulkan) return;
		auto& pd = PhysicalDevice();
		m_pass = std::make_unique<ShadowDepthPass>(*m_device,pd,m_queue,m_graphicsQueueFamily,kRes);
		m_cache = std::make_unique<RenderCache>(*m_device,pd);
		m_cache->SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily)); }
	void TearDown() override { VulkanTestShared::TearDown(); }

	static float ExpectedDepth() {
		// Z-up: eye at Y=-kEyeDist=-10 looking +Y, quad at Y=-5.
		// GLM_FORCE_DEPTH_ZERO_TO_ONE → left-handed view: +Z = forward.
		// Vz = world_y - eye_y = -5 - (-10) = 5 (in view-space)
		// ndc = (Vz - near) / (far - near) = (5 - (-10)) / (30 - (-10)) = 15/40 = 0.375
		float Vz = kQuadY + kEyeDist;  // -5 + 10 = 5
		return (Vz - kNear) / (kFar - kNear);  // (5 - (-10))/40 = 0.375
	}

	static std::vector<uint8_t> DepthToRGBA8(const std::vector<float>& d) {
		std::vector<uint8_t> rgba(d.size()*4);
		for (size_t i=0;i<d.size();++i){
			uint8_t v=uint8_t(std::clamp(d[i],0.f,1.f)*255.f+.5f);
			rgba[i*4+0]=v; rgba[i*4+1]=0; rgba[i*4+2]=0; rgba[i*4+3]=255;
		}
		return rgba;
	}

	struct TS { std::shared_ptr<Scene> s; int uid=-1; };
	TS BuildScene() {
		TS r; r.s=std::make_shared<Scene>();
		// Camera at origin targeting (0,0,0) - used for orthographic center computation
		auto cam=std::make_shared<Camera>();
		cam->SetPosition(glm::vec3(0.f, -5.f, 2.f));
		cam->SetTarPos(glm::vec3(0.f, 0.f, 0.f));
		cam->ChangeCamRatio(static_cast<float>(kRes), static_cast<float>(kRes));
		r.s->UseCamera(cam);
		// Quad at y=-5 in XZ plane, 5 units forward from the sun shadow camera at (0,-10,0)
		// (eye distance = sun_depth_range = 10, decoupled from far plane = 30)
		const char* ob =
			"v -2.5 -5 -2.5\n"
			"v  2.5 -5 -2.5\n"
			"v  2.5 -5  2.5\n"
			"v -2.5 -5  2.5\n"
			"f 1 2 3 4\n";
		auto md=std::make_shared<MeshData>(); md->LoadObjFromString(ob);
		auto m=std::make_shared<Mesh>(); m->o_name="Q"; m->o_mesh=md;
		// GPU buffers created lazily by RenderCache::GetMeshGPU()
		r.s->UseMesh(m);
		auto l=std::make_shared<Light>(LightType::SUNLIGHT,10.f,glm::vec3(1.f));
		l->o_name="S"; l->use_shadow=true;
		r.s->UseLight(l); r.uid=r.s->light_list.begin()->first;
		return r;
	}

	std::unique_ptr<ShadowDepthPass> m_pass;
	std::unique_ptr<RenderCache> m_cache;
};

TEST_F(SunShadowDepthTest, OrthoDepthMap)
{
	if (!m_hasVulkan) { GTEST_SKIP()<<"No Vulkan GPU."; }
	auto& pd=PhysicalDevice();
	auto ts=BuildScene();
	ASSERT_NE(ts.uid,-1);
	const int uid=ts.uid;

	// Pre-register GPU resources before pass recording
	VulkanTestShared::EnsureMeshesUploaded(*m_cache, *ts.s, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);
	VulkanTestShared::EnsureLightShadowsUploaded(*m_cache, *ts.s, *m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

	{ auto& cmd=BeginCmd();
		RenderContext ctx{};
		ctx.width=kRes; ctx.height=kRes;
		ctx.editor.scene=ts.s.get();
		// The sun path reads the viewing camera from the context, standing in for
		// what DeferredRenderer::recordFrame() injects.
		VulkanTestShared::PublishSceneCamera(*m_cache, ctx.editor);
		m_pass->Record(*cmd,*m_cache,ctx);
		EndSubmitWait(cmd); }

	std::vector<float> dd;
	{ auto* lgpu = m_cache->GetLightGPU(uid);
		ASSERT_NE(lgpu, nullptr);
		ASSERT_NE(lgpu->shadowDepthMap, nullptr);
		auto& sm = *lgpu->shadowDepthMap;
		auto data=sm.ReadImageData(*m_device,pd,m_queue,m_graphicsQueueFamily,nullptr,{kRes,kRes});
		const float* rd=reinterpret_cast<const float*>(data->GetPixelData().data());
		dd.assign(rd,rd+kRes*kRes);
		float mn=rd[0],mx=rd[0]; int zc=0,oc=0;
		for (uint32_t i=0,n=kRes*kRes;i<n;++i){
			mn=std::min(mn,rd[i]); mx=std::max(mx,rd[i]);
			if (rd[i]<=0.001f) zc++; if (rd[i]>=0.999f&&rd[i]<=1.001f) oc++;
		}
		std::cout<<"\n=== Sun Shadow Depth ==="<<std::endl;
		std::cout<<"[Depth] "<<kRes<<"x"<<kRes<<" min="<<mn<<" max="<<mx
		         <<" z="<<zc<<" o="<<oc<<std::endl; }

	{ const float exp=ExpectedDepth(); int bad=0;
		std::cout<<"[Depth] Expected: "<<exp<<std::endl;
		for (uint32_t i=0,n=kRes*kRes;i<n;++i)
			if (std::abs(dd[i]-exp)>kTol) { bad++;
				if (bad<=5) std::cout<<"BAD i="<<i<<" a="<<dd[i]<<" e="<<exp<<std::endl; }
		std::cout<<"[Depth] Bad: "<<bad<<"/"<<(kRes*kRes)<<" (tol="<<kTol<<")"<<std::endl;
		EXPECT_LT(bad,1); }

	{ const std::string rp=neurus::test::ReferencePath::Make("shadow/SunDepth.png");
		auto rgba=DepthToRGBA8(dd);
		ImageData img(rgba.data(),kRes,kRes,PixelFormat::RGBA8U);
		ASSERT_TRUE(img.SavePNG(rp+".tmp"));
		int rr=neurus::test::CheckReferenceOrGenerate(rp,2);
		if (rr<0) { std::cout<<"[Depth] Reference generated\n"; GTEST_SKIP()<<"Re-run."; }
		else EXPECT_EQ(rr,0); }
}
