#pragma once

#include <cereal/archives/json.hpp>

namespace neurus
{

class IResourceLookup;

/// JSON input archive carrying the lookup for one pooled resource load.
class ResourceInputArchive final : public cereal::JSONInputArchive
{
public:
	ResourceInputArchive(std::istream& stream, const IResourceLookup& pool);

	const IResourceLookup& Pool() const { return m_pool; }

private:
	const IResourceLookup& m_pool;
};

} // namespace neurus
