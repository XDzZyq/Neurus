#include "asset/ResourceInputArchive.hpp"

namespace neurus
{

ResourceInputArchive::ResourceInputArchive(std::istream& stream, const IResourceLookup& pool)
	: cereal::JSONInputArchive(stream), m_pool(pool)
{
}

} // namespace neurus
