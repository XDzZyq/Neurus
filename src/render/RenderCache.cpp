#include "RenderCache.h"

#include "core/Log.h"
#include "scene/DebugDrawList.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RenderCache::RenderCache(const vk::raii::Device& device,
                       const vk::raii::PhysicalDevice& physicalDevice)
	: rc_device(&device)
	, rc_physicalDevice(&physicalDevice)
	, rc_cameraGPU(std::make_unique<CameraGPU>(device, physicalDevice))
	, rc_debugCache(std::make_unique<DebugCache>(device, physicalDevice))
{
	NEURUS_LOG("[RenderCache] Created");
}

void RenderCache::SetLightingCache(std::unique_ptr<LightingCache> lightingCache)
{
	rc_lightingCache = std::move(lightingCache);
	NEURUS_LOG("[RenderCache] LightingCache set");
}

// ---------------------------------------------------------------------------
// Lazy attachment creation
// ---------------------------------------------------------------------------

void RenderCache::createAttachment(const AttachmentName name, const vk::Extent2D extent)
{
	const auto config = ConfigFor(name);

	Image image(*rc_device,
	                  *rc_physicalDevice,
	                  extent,
	                  config.format,
	                  config.usage,
	                  1,                // mipLevels
	                  config.imageType,
	                  AttachmentNameToString(name));  // debug name

	rc_attachments.emplace(name, std::move(image));
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

Image& RenderCache::GetAttachment(const AttachmentName name, const vk::Extent2D extent)
{
	auto it = rc_attachments.find(name);
	if (it == rc_attachments.end())
	{
		NEURUS_LOG("[RenderCache] Lazily creating attachment \""
		          << AttachmentNameToString(name) << "\" at "
		          << extent.width << "x" << extent.height);
		createAttachment(name, extent);
		it = rc_attachments.find(name);
	}
	return it->second;
}

const Image& RenderCache::GetAttachment(const AttachmentName name) const
{
	const auto it = rc_attachments.find(name);
	if (it == rc_attachments.end())
	{
		throw std::out_of_range("RenderCache::GetAttachment: attachment not found");
	}
	return it->second;
}

// ---------------------------------------------------------------------------
// Lazy attachment creation
// ---------------------------------------------------------------------------

Image& RenderCache::GetShadowIntensityArray(const vk::Extent2D extent)
{
	if (rc_shadowIntensityArray)
	{
		return *rc_shadowIntensityArray;
	}

	NEURUS_LOG("[RenderCache] Lazily creating shadow intensity array at "
	           << extent.width << "x" << extent.height
	           << " with " << MAX_SHADOW_LAYERS << " layers");

	rc_shadowIntensityArray = std::make_unique<Image>(
		*rc_device,
		*rc_physicalDevice,
		extent,
		vk::Format::eR8Unorm,
		vk::ImageUsageFlagBits::eStorage |
			vk::ImageUsageFlagBits::eSampled |
			vk::ImageUsageFlagBits::eTransferSrc |
			vk::ImageUsageFlagBits::eTransferDst,
		1,                              // mipLevels
		Image::ImageType::eArray,
		"ShadowIntensityArray",         // debug name
		false,                          // arrayView
		MAX_SHADOW_LAYERS);             // arrayLayers

	return *rc_shadowIntensityArray;
}

uint32_t RenderCache::GetShadowIntensityLayer(const int lightUID, const vk::Extent2D /*extent*/)
{
	// Fast path: check the shadow index built by UpdateLighting (always in sync
	// with the SSBO shadowMapIndex field and current visibility state).
	auto it = rc_uidToShadowLayer.find(lightUID);
	if (it != rc_uidToShadowLayer.end())
	{
		return it->second;
	}

	// Slow path: light not yet registered (e.g. test context or first frame).
	// Lazily allocate the next available index.
	const uint32_t layer = static_cast<uint32_t>(rc_uidToShadowLayer.size());
	if (layer >= MAX_SHADOW_LAYERS)
	{
		NEURUS_ERR("[RenderCache] Shadow intensity layer overflow: lightUID="
		           << lightUID << " exceeds MAX_SHADOW_LAYERS=" << MAX_SHADOW_LAYERS);
		assert(false && "MAX_SHADOW_LAYERS exceeded");
		return 0;
	}

	rc_uidToShadowLayer[lightUID] = layer;
	NEURUS_LOG("[RenderCache] Allocated shadow intensity layer " << layer
	           << " for lightUID=" << lightUID);
	return layer;
}

Image* RenderCache::GetShadowIntensityArray() const
{
	return rc_shadowIntensityArray.get();
}

uint32_t RenderCache::GetShadowIntensityLayerIndex(const int lightUID) const
{
	const auto it = rc_uidToShadowLayer.find(lightUID);
	return (it != rc_uidToShadowLayer.end()) ? it->second : 0;
}

std::vector<int> RenderCache::GetShadowMapUIDs() const
{
	std::vector<int> uids;
	uids.reserve(rc_lightGPUs.size());
	for (const auto& [uid, lgpu] : rc_lightGPUs)
	{
		if (lgpu.shadowDepthMap)
		{
			uids.push_back(uid);
		}
	}
	return uids;
}

void RenderCache::RemoveLight(const int lightUID)
{
	// Clear shadow maps within LightGPU if present
	auto it = rc_lightGPUs.find(lightUID);
	if (it != rc_lightGPUs.end())
	{
		it->second.shadowDepthMap.reset();
		it->second.shadowColorMap.reset();
	}
	rc_uidToShadowLayer.erase(lightUID);
}

bool RenderCache::HasAttachment(const AttachmentName name) const
{
	return rc_attachments.find(name) != rc_attachments.end();
}

// ---------------------------------------------------------------------------
// Mesh GPU resources (query-only)
// ---------------------------------------------------------------------------

MeshGPU* RenderCache::GetMeshGPU(const int objectId)
{
	auto it = rc_meshGPUs.find(objectId);
	return (it != rc_meshGPUs.end()) ? &it->second : nullptr;
}

const MeshGPU* RenderCache::GetMeshGPU(const int objectId) const
{
	auto it = rc_meshGPUs.find(objectId);
	return (it != rc_meshGPUs.end()) ? &it->second : nullptr;
}

void RenderCache::RemoveMeshGPU(const int objectId)
{
	rc_meshGPUs.erase(objectId);
}

void RenderCache::UseMeshGPU(const int objectId, MeshGPU meshGPU)
{
	rc_meshGPUs[objectId] = std::move(meshGPU);
	NEURUS_LOG("[RenderCache] Registered MeshGPU for objectId=" << objectId);
}

// ---------------------------------------------------------------------------
// Clean / CleanScreenSpace
// ---------------------------------------------------------------------------

void RenderCache::Clean()
{
	rc_attachments.clear();
	rc_shadowIntensityArray.reset();
	rc_meshGPUs.clear();
	rc_environmentGPUs.clear();
	rc_lightGPUs.clear();
	rc_uidToShadowLayer.clear();
	rc_lightingCache.reset();
	rc_pipelineCache.Clear();
}

void RenderCache::CleanScreenSpace()
{
	rc_attachments.clear();
	rc_shadowIntensityArray.reset();
	// rc_uidTo*Index maps preserved — shadow indexing survives resize
	// rc_meshGPUs preserved — mesh GPU buffers survive resize
	// rc_environmentGPUs preserved — IBL cubemaps survive resize
	// rc_lightGPUs preserved — shadow depth maps survive resize
}

// ---------------------------------------------------------------------------
// Environment GPU resources (registration/query)
// ---------------------------------------------------------------------------

EnvironmentGPU* RenderCache::GetEnvironmentGPU(const int envId)
{
	auto it = rc_environmentGPUs.find(envId);
	return (it != rc_environmentGPUs.end()) ? &it->second : nullptr;
}

const EnvironmentGPU* RenderCache::GetEnvironmentGPU(const int envId) const
{
	auto it = rc_environmentGPUs.find(envId);
	return (it != rc_environmentGPUs.end()) ? &it->second : nullptr;
}

void RenderCache::RemoveEnvironmentGPU(const int envId)
{
	rc_environmentGPUs.erase(envId);
}

void RenderCache::UseEnvironmentGPU(const int envId, EnvironmentGPU envGPU)
{
	rc_environmentGPUs[envId] = std::move(envGPU);
	NEURUS_LOG("[RenderCache] Registered EnvironmentGPU for envId=" << envId);
}

// ---------------------------------------------------------------------------
// Per-light GPU resource registration
// ---------------------------------------------------------------------------

void RenderCache::UseLightGPU(const int lightUID, LightGPU lightGPU)
{
	rc_lightGPUs[lightUID] = std::move(lightGPU);
	NEURUS_LOG("[RenderCache] Registered LightGPU for lightUID=" << lightUID);
}

LightGPU* RenderCache::GetLightGPU(const int lightUID)
{
	auto it = rc_lightGPUs.find(lightUID);
	return (it != rc_lightGPUs.end()) ? &it->second : nullptr;
}

const LightGPU* RenderCache::GetLightGPU(const int lightUID) const
{
	auto it = rc_lightGPUs.find(lightUID);
	return (it != rc_lightGPUs.end()) ? &it->second : nullptr;
}

void RenderCache::RemoveLightGPU(const int lightUID)
{
	rc_lightGPUs.erase(lightUID);
}

// ---------------------------------------------------------------------------
// Pipeline cache
// ---------------------------------------------------------------------------

PipelineCache& RenderCache::GetPipelineCache()
{
	return rc_pipelineCache;
}

Pipeline* RenderCache::GetPipeline(const int uid)
{
	return rc_pipelineCache.Get(uid);
}

Pipeline* RenderCache::GetPipeline(const int uid, const int version)
{
	return rc_pipelineCache.GetPipeline(uid, version);
}

void RenderCache::UsePipeline(const int uid, Pipeline pipeline)
{
	rc_pipelineCache.Store(uid, std::move(pipeline));
}

void RenderCache::UsePipeline(const int uid, Pipeline pipeline, const int version)
{
	rc_pipelineCache.UsePipeline(uid, std::move(pipeline), version);
}

void RenderCache::RemovePipeline(const int uid)
{
	rc_pipelineCache.Remove(uid);
}

// ---------------------------------------------------------------------------
// Lighting GPU resources
// ---------------------------------------------------------------------------

void RenderCache::UpdateLighting(const std::unordered_map<int,
                                 std::variant<PointLightStruct, SunLightStruct, SpotLightStruct>>& lightDict)
{
	assert(rc_lightingCache && "InitLightingCache must be called before UpdateLighting");

	// --- Clear index maps ---
	rc_uidToSSBOIdx.clear();
	rc_uidToShadowLayer.clear();

	// --- Separate and sort by UID for deterministic SSBO ordering ---
	std::vector<std::pair<int, PointLightStruct>> pointEntries;
	std::vector<std::pair<int, SunLightStruct>> sunEntries;
	std::vector<std::pair<int, SpotLightStruct>> spotEntries;

	for (const auto& [uid, lightVariant] : lightDict)
	{
		if (std::holds_alternative<PointLightStruct>(lightVariant))
		{
			pointEntries.push_back({uid, std::get<PointLightStruct>(lightVariant)});
		}
		else if (std::holds_alternative<SunLightStruct>(lightVariant))
		{
			sunEntries.push_back({uid, std::get<SunLightStruct>(lightVariant)});
		}
		else if (std::holds_alternative<SpotLightStruct>(lightVariant))
		{
			spotEntries.push_back({uid, std::get<SpotLightStruct>(lightVariant)});
		}
	}

	std::sort(pointEntries.begin(), pointEntries.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });
	std::sort(sunEntries.begin(), sunEntries.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });
	std::sort(spotEntries.begin(), spotEntries.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });

	// --- Build SSBO vectors and populate uid→index map ---
	std::vector<PointLightStruct> pointVec;
	for (size_t i = 0; i < pointEntries.size(); ++i)
	{
		rc_uidToSSBOIdx[pointEntries[i].first] = static_cast<uint32_t>(i);
		pointVec.push_back(pointEntries[i].second);
	}

	std::vector<SunLightStruct> sunVec;
	for (size_t i = 0; i < sunEntries.size(); ++i)
	{
		rc_uidToSSBOIdx[sunEntries[i].first] = static_cast<uint32_t>(i);
		sunVec.push_back(sunEntries[i].second);
	}

	std::vector<SpotLightStruct> spotVec;
	for (size_t i = 0; i < spotEntries.size(); ++i)
	{
		rc_uidToSSBOIdx[spotEntries[i].first] = static_cast<uint32_t>(i);
		spotVec.push_back(spotEntries[i].second);
	}

	// --- Assign shadow map indices (shared pool: point, then spot, then sun, max MAX_SHADOW_LAYERS) ---
	uint32_t shadowIdx = 0;
	for (const auto& [uid, pl] : pointEntries)
	{
		if (shadowIdx >= MAX_SHADOW_LAYERS) break;
		rc_uidToShadowLayer[uid] = shadowIdx++;
	}
	for (const auto& [uid, sp] : spotEntries)
	{
		if (shadowIdx >= MAX_SHADOW_LAYERS) break;
		rc_uidToShadowLayer[uid] = shadowIdx++;
	}
	for (const auto& [uid, sl] : sunEntries)
	{
		if (shadowIdx >= MAX_SHADOW_LAYERS) break;
		rc_uidToShadowLayer[uid] = shadowIdx++;
	}

	// --- Stamp shadowMapIndex on each struct ---
	for (size_t i = 0; i < pointEntries.size(); ++i)
	{
		const int uid = pointEntries[i].first;
		auto it = rc_uidToShadowLayer.find(uid);
		pointVec[i].shadowMapIndex = (it != rc_uidToShadowLayer.end())
			? static_cast<int32_t>(it->second) : -1;
	}
	for (size_t i = 0; i < sunEntries.size(); ++i)
	{
		const int uid = sunEntries[i].first;
		auto it = rc_uidToShadowLayer.find(uid);
		sunVec[i].shadowMapIndex = (it != rc_uidToShadowLayer.end())
			? static_cast<int32_t>(it->second) : -1;
	}
	for (size_t i = 0; i < spotEntries.size(); ++i)
	{
		const int uid = spotEntries[i].first;
		auto it = rc_uidToShadowLayer.find(uid);
		spotVec[i].shadowMapIndex = (it != rc_uidToShadowLayer.end())
			? static_cast<int32_t>(it->second) : -1;
	}

	// --- Upload to GPU ---
	rc_lightingCache->UpdatePointLights(pointVec);
	rc_lightingCache->UpdateSunLights(sunVec);
	rc_lightingCache->UpdateSpotLights(spotVec);

	NEURUS_LOG("[RenderCache] UpdateLighting: " << pointVec.size()
	           << " point lights, " << sunVec.size() << " sun lights, "
	           << spotVec.size() << " spot lights, "
	           << rc_uidToShadowLayer.size() << " shadow casters");
}

void RenderCache::UpdateLight(int lightUID,
                              const std::variant<PointLightStruct, SunLightStruct, SpotLightStruct>& light)
{
	assert(rc_lightingCache && "InitLightingCache must be called before UpdateLight");

	if (std::holds_alternative<PointLightStruct>(light))
	{
		auto it = rc_uidToSSBOIdx.find(lightUID);
		if (it == rc_uidToSSBOIdx.end()) return;

		PointLightStruct updated = std::get<PointLightStruct>(light);
		auto shadowIt = rc_uidToShadowLayer.find(lightUID);
		updated.shadowMapIndex = (shadowIt != rc_uidToShadowLayer.end())
			? static_cast<int32_t>(shadowIt->second) : -1;

		rc_lightingCache->UpdatePointLight(updated, it->second);
	}
	else if (std::holds_alternative<SunLightStruct>(light))
	{
		auto it = rc_uidToSSBOIdx.find(lightUID);
		if (it == rc_uidToSSBOIdx.end()) return;

		SunLightStruct updated = std::get<SunLightStruct>(light);
		auto shadowIt = rc_uidToShadowLayer.find(lightUID);
		updated.shadowMapIndex = (shadowIt != rc_uidToShadowLayer.end())
			? static_cast<int32_t>(shadowIt->second) : -1;

		rc_lightingCache->UpdateSunLight(updated, it->second);
	}
	else if (std::holds_alternative<SpotLightStruct>(light))
	{
		auto it = rc_uidToSSBOIdx.find(lightUID);
		if (it == rc_uidToSSBOIdx.end()) return;

		SpotLightStruct updated = std::get<SpotLightStruct>(light);
		auto shadowIt = rc_uidToShadowLayer.find(lightUID);
		updated.shadowMapIndex = (shadowIt != rc_uidToShadowLayer.end())
			? static_cast<int32_t>(shadowIt->second) : -1;

		rc_lightingCache->UpdateSpotLight(updated, it->second);
	}
}

uint32_t RenderCache::GetShadowIndex(int lightUID) const
{
	auto it = rc_uidToShadowLayer.find(lightUID);
	return (it != rc_uidToShadowLayer.end()) ? it->second : 0;
}

LightingCache* RenderCache::GetLightingCache()
{
	return rc_lightingCache.get();
}

const LightingCache* RenderCache::GetLightingCache() const
{
	return rc_lightingCache.get();
}

// ---------------------------------------------------------------------------
// Shared camera UBO / debug overlay geometry
// ---------------------------------------------------------------------------

void RenderCache::UpdateCamera(const glm::mat4& proj, const glm::mat4& view)
{
	rc_cameraGPU->Update(proj, view);
}

void RenderCache::UpdateDebugDraw(uint32_t frameIndex, const DebugDrawList& list)
{
	rc_debugCache->Update(frameIndex, list);
}

// ---------------------------------------------------------------------------
// Attachment configuration
// ---------------------------------------------------------------------------

RenderCache::AttachmentConfig RenderCache::ConfigFor(const AttachmentName name)
{
	// Common usage for color attachments:
	//   COLOR_ATTACHMENT - written by fragment shader
	//   SAMPLED          - read by subsequent passes (deferred shading, post-FX)
	//   TRANSFER_SRC     - screenshot capture (T24a), debug readback
	constexpr vk::ImageUsageFlags kColorAttachmentUsage =
		vk::ImageUsageFlagBits::eColorAttachment |
		vk::ImageUsageFlagBits::eSampled |
		vk::ImageUsageFlagBits::eTransferSrc;

	// Depth attachment usage:
	//   DEPTH_STENCIL_ATTACHMENT - written by depth test
	//   SAMPLED                  - read by SSAO, SSR, etc.
	constexpr vk::ImageUsageFlags kDepthAttachmentUsage =
		vk::ImageUsageFlagBits::eDepthStencilAttachment |
		vk::ImageUsageFlagBits::eSampled |
		vk::ImageUsageFlagBits::eTransferSrc;

	constexpr auto e2D = Image::ImageType::e2D;
	constexpr auto eDS = Image::ImageType::eDepthStencil;

	switch (name)
	{
	// --- G-Buffer ---
	case AttachmentName::Position:
		return { vk::Format::eR16G16B16A16Sfloat, kColorAttachmentUsage, e2D };
	case AttachmentName::Normal:
		return { vk::Format::eR16G16B16A16Sfloat, kColorAttachmentUsage, e2D };
	case AttachmentName::Albedo:
		return { vk::Format::eR8G8B8A8Srgb, kColorAttachmentUsage, e2D };
	case AttachmentName::MetallicRoughness:
		return { vk::Format::eR8G8B8A8Unorm, kColorAttachmentUsage, e2D };
	case AttachmentName::Depth:
		return { vk::Format::eD32Sfloat, kDepthAttachmentUsage, eDS };

	// --- Post-FX ---
	case AttachmentName::HDRColor:
		// STORAGE added for compute shader write (PBR lighting pass)
		return { vk::Format::eR16G16B16A16Sfloat,
		         kColorAttachmentUsage | vk::ImageUsageFlagBits::eStorage, e2D };
	case AttachmentName::SSAO:
		return { vk::Format::eR8Unorm,
		         kColorAttachmentUsage | vk::ImageUsageFlagBits::eStorage, e2D };
	case AttachmentName::SSR:
		return { vk::Format::eR16G16B16A16Sfloat, kColorAttachmentUsage, e2D };

	// --- Shadow (logical resources, not backed by GetAttachment) ---
	// ShadowDepth is a per-light bundle (LightGPU::shadowDepthMap); ShadowIntensity
	// is the 2D array from GetShadowIntensityArray(). They are graph/pipeline
	// identities only, resolved by their owning pass — never a single attachment.
	case AttachmentName::ShadowDepth:
	case AttachmentName::ShadowIntensity:
		throw std::invalid_argument(
			"RenderCache::ConfigFor: ShadowDepth/ShadowIntensity are logical "
			"resources resolved by their owning pass, not via GetAttachment");

	// --- ID ---
	case AttachmentName::IDBuffer:
		return { vk::Format::eR32Uint, kColorAttachmentUsage, e2D };

	// --- Gizmo ---
	case AttachmentName::GizmoHighlight:
		return { vk::Format::eR8Unorm,
		         kColorAttachmentUsage | vk::ImageUsageFlagBits::eStorage, e2D };

	// --- Composite ---
	case AttachmentName::ComposedOutput:
		return { vk::Format::eR16G16B16A16Sfloat,
		         kColorAttachmentUsage | vk::ImageUsageFlagBits::eStorage, e2D };

	// --- Anti-Aliasing ---
	case AttachmentName::FXAAOutput:
		return { vk::Format::eR16G16B16A16Sfloat,
		         kColorAttachmentUsage | vk::ImageUsageFlagBits::eStorage, e2D };
	case AttachmentName::FXAAOffsets:
		return { vk::Format::eR16G16Sfloat,
		         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc, e2D };
	}

	throw std::invalid_argument("RenderCache::ConfigFor: unknown attachment name");
}

// ---------------------------------------------------------------------------
// String conversion
// ---------------------------------------------------------------------------

const char* AttachmentNameToString(const AttachmentName name)
{
	switch (name)
	{
	case AttachmentName::Position:          return "Position";
	case AttachmentName::Normal:            return "Normal";
	case AttachmentName::Albedo:            return "Albedo";
	case AttachmentName::MetallicRoughness: return "MetallicRoughness";
	case AttachmentName::Depth:             return "Depth";
	case AttachmentName::HDRColor:          return "HDRColor";
	case AttachmentName::SSAO:              return "SSAO";
	case AttachmentName::SSR:               return "SSR";
	case AttachmentName::ShadowDepth:       return "ShadowDepth";
	case AttachmentName::ShadowIntensity:   return "ShadowIntensity";
	case AttachmentName::IDBuffer:          return "IDBuffer";
	case AttachmentName::GizmoHighlight:    return "GizmoHighlight";
	case AttachmentName::ComposedOutput:    return "ComposedOutput";
	case AttachmentName::FXAAOutput:        return "FXAAOutput";
	case AttachmentName::FXAAOffsets:       return "FXAAOffsets";
	}
	return "Unknown";
}

} // namespace neurus
