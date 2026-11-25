#pragma once

#include <cereal/archives/json.hpp>

namespace neurus
{

class IResourceLookup;

/// Project-owned services used by resource types while reading their fields.
struct ResourceReadServices
{
	virtual ~ResourceReadServices() = default;
};

/// JSON input archive carrying the lookup for one pooled resource load.
class ResourceInputArchive final : public cereal::JSONInputArchive
{
public:
	ResourceInputArchive(std::istream& stream, const IResourceLookup& pool,
	                     const ResourceReadServices* services);

	const IResourceLookup& Pool() const { return m_pool; }
	const ResourceReadServices* Services() const { return m_services; }

private:
	const IResourceLookup& m_pool;
	const ResourceReadServices* m_services;
};

} // namespace neurus
