#include "PipelineCache.hpp"
#include "core/Log.hpp"

namespace neurus {

Pipeline* PipelineCache::Get(int uid)
{
	auto it = p_entries.find(uid);
	return (it != p_entries.end()) ? &it->second.pipeline : nullptr;
}

const Pipeline* PipelineCache::Get(int uid) const
{
	auto it = p_entries.find(uid);
	return (it != p_entries.end()) ? &it->second.pipeline : nullptr;
}

Pipeline* PipelineCache::GetPipeline(int uid, int version)
{
	auto it = p_entries.find(uid);
	if (it == p_entries.end()) return nullptr;
	if (it->second.version != version) return nullptr;
	return &it->second.pipeline;
}

Pipeline& PipelineCache::GetOrCreate(int uid,
                                     std::function<Pipeline()> factory)
{
	auto it = p_entries.find(uid);
	if (it != p_entries.end())
		return it->second.pipeline;

	auto [newIt, inserted] = p_entries.emplace(uid, CacheEntry{factory(), -1});
	NEURUS_LOG("[PipelineCache] Created pipeline '" << uid << "'");
	return newIt->second.pipeline;
}

void PipelineCache::Store(int uid, Pipeline pipeline)
{
	p_entries[uid] = CacheEntry{std::move(pipeline), -1};
	NEURUS_LOG("[PipelineCache] Stored pipeline '" << uid << "'");
}

void PipelineCache::UsePipeline(int uid, Pipeline pipeline, int version)
{
	p_entries[uid] = CacheEntry{std::move(pipeline), version};
	NEURUS_LOG("[PipelineCache] Used pipeline '" << uid << "' version=" << version);
}

void PipelineCache::Remove(int uid)
{
	p_entries.erase(uid);
	NEURUS_LOG("[PipelineCache] Removed pipeline '" << uid << "'");
}

void PipelineCache::Clear()
{
	p_entries.clear();
	NEURUS_LOG("[PipelineCache] Cleared all pipelines");
}

} // namespace neurus
