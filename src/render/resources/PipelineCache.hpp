#pragma once

#include "../Pipeline.hpp"

#include <functional>
#include <unordered_map>

namespace neurus {

class PipelineCache
{
public:
	PipelineCache() = default;
	~PipelineCache() = default;

	PipelineCache(const PipelineCache&) = delete;
	PipelineCache& operator=(const PipelineCache&) = delete;
	PipelineCache(PipelineCache&&) noexcept = default;
	PipelineCache& operator=(PipelineCache&&) noexcept = default;

	Pipeline* Get(int uid);
	const Pipeline* Get(int uid) const;

	/** @brief Version-aware lookup. Returns nullptr if entry not found or version mismatch. */
	Pipeline* GetPipeline(int uid, int version);

	Pipeline& GetOrCreate(int uid,
	                      std::function<Pipeline()> factory);

	void Store(int uid, Pipeline pipeline);

	/** @brief Store a pipeline with version tracking. Overwrites existing entry. */
	void UsePipeline(int uid, Pipeline pipeline, int version);

	void Remove(int uid);
	void Clear();

private:
	struct CacheEntry
	{
		Pipeline pipeline;
		int version = -1;
	};
	std::unordered_map<int, CacheEntry> p_entries;
};

} // namespace neurus
