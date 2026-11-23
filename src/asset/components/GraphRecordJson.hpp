#pragma once

#include <nlohmann/json_fwd.hpp>

namespace neurus::project
{

/// Checks nested JSON field types before cereal reads a graph topology.
bool ValidGraphShape(const nlohmann::ordered_json& graph);

} // namespace neurus::project
