/**
 * @file PassShaders.hpp
 * @brief Pools the shaders of every built-in pass kind and answers its ports.
 *
 * This mirrors `CreateDefaultScene`: the built-in content is created by a
 * factory, registered in the app-scoped pool, and reused on a later call. A
 * project load restores the pooled shaders first, so construction finds them by
 * name instead of creating duplicates, and a new scene starts from the kind's
 * default source.
 *
 * The class is the Editor's `IPortSource`: the CPU graph derives its sockets
 * from the same pooled shaders the renderer will be handed as snapshots.
 */

#pragma once

#include "asset/data/render_graph/PassKinds.hpp"
#include "asset/data/render_graph/PortSource.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neurus {

class ResourceManager;
class ShaderData;

/// @brief Creates the default shader for one source of a built-in pass kind.
using PassShaderFactory = std::function<std::shared_ptr<ShaderData>(const PassShaderSource&)>;

/**
 * @brief Pooled pass shaders, keyed by pass kind.
 *
 * Non-copyable: it holds pool identities, not shader data.
 */
class PassShaders : public IPortSource
{
public:
	/**
	 * @brief Creates or adopts the shaders of every built-in pass kind.
	 *
	 * A pooled `ShaderData` whose name equals a source name is reused, so a
	 * restored project does not duplicate its shaders. A source whose factory
	 * fails, or whose shader cannot be resolved, appends a message to
	 * @p problems; a kind with no usable shader is left out and the graph
	 * reports it as an unknown pass.
	 *
	 * @param resources  App-scoped pool.
	 * @param factory    Creates one shader from its source row.
	 * @param problems   Receives one message per source that could not be seeded.
	 */
	PassShaders(ResourceManager& resources, const PassShaderFactory& factory,
	            std::vector<std::string>& problems);

	PassShaders(const PassShaders&)            = delete;
	PassShaders& operator=(const PassShaders&) = delete;
	PassShaders(PassShaders&&)                 = delete;
	PassShaders& operator=(PassShaders&&)      = delete;

	/// @brief Returns the merged ports of the kind's shaders, or nullptr when unseeded.
	const PassPorts* FindPassPorts(std::string_view passKind) const override;

	/// @brief Returns the pass kind's out-of-graph inputs.
	std::vector<std::string> ExternalInputs(std::string_view passKind) const override;

	/// @brief Pooled UID of the kind's primary shader, or 0 when unseeded.
	int ShaderUid(std::string_view passKind) const;

	/// @brief Pooled UIDs of every shader the kind owns, in table order.
	std::vector<int> ShaderUids(std::string_view passKind) const;

private:
	struct Kind
	{
		std::vector<int>         shaderUids;
		PassPorts                ports;
		std::vector<std::string> externals;
	};

	std::unordered_map<std::string, Kind> m_kinds;
};

} // namespace neurus
