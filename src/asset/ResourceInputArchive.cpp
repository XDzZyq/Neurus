#include "asset/ResourceInputArchive.hpp"

namespace neurus
{

ResourceInputArchive::ResourceInputArchive(std::istream& stream, const IResourceLookup& pool,
                                           const ResourceReadServices* services)
	: cereal::JSONInputArchive(stream), m_pool(pool), m_services(services)
{
}

} // namespace neurus
