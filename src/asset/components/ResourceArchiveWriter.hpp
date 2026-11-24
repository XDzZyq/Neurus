#pragma once

namespace cereal { class JSONOutputArchive; }
namespace neurus { class ResourceManager; }

namespace neurus::project
{

/// Writes the m_resources project component without materializing untouched records.
void WriteResourcePool(cereal::JSONOutputArchive& archive, const ResourceManager& pool);

} // namespace neurus::project
