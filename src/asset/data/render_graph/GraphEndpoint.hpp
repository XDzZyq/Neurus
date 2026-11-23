#pragma once

#include <string>

namespace neurus {

/// A node and socket name. An empty node identifies a named scene output.
struct GraphEndpoint
{
	std::string node;
	std::string port;
	bool operator==(const GraphEndpoint&) const = default;
};

} // namespace neurus
