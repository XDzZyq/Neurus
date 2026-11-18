/**
 * @file GraphMorphism.hpp
 * @brief Functor between two graphs: the mapping policy, its images and the laws.
 *
 * A graph is a small category: nodes are objects and connections are
 * morphisms. A map from one graph to another is a functor when it is total and
 * preserves incidence. Nothing here asks for an inverse, an injection or a
 * surjection; the map is deliberately one-way.
 *
 * The morphism owns the policy that builds the target objects, so it knows how
 * to map `G_A` to `G_B` instead of asking a caller to combine a policy with a
 * free function:
 *
 *     GraphMorphism<A, B> morphism(Policy{...});
 *     B target = morphism.Map(source);
 *
 * @note The node and connection images point into the target graph `Map()`
 *       returned and into the source graph it was given, so both must outlive
 *       the morphism.
 */

#pragma once

#include "core/Graph.hpp"

#include <functional>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neurus {

/**
 * @brief Functor from one graph to another.
 *
 * Owns the @ref Policy that creates the target objects and records the node and
 * connection images. `Map()` maps a whole graph; `MapNode()` and `MapConnect()`
 * map one element at a time for callers that build the target themselves.
 */
template<typename GraphA, typename GraphB>
class GraphMorphism
{
public:
	using NodeA      = typename GraphA::NodeType;
	using NodeB      = typename GraphB::NodeType;
	using SocketInA  = typename GraphA::SocketInT;
	using SocketOutA = typename GraphA::SocketOutT;
	using SocketInB  = typename GraphB::SocketInT;
	using SocketOutB = typename GraphB::SocketOutT;

	/**
	 * @brief How the morphism creates the target objects of one source element.
	 *
	 * A factory returns null to leave an element unmapped; `Validate()` then
	 * names it. The factories receive the target graph or node so an
	 * implementation can create the object in place (and keep its own state out
	 * of the morphism).
	 */
	struct Policy
	{
		std::function<NodeB*(GraphB&, const NodeA&)>           createNode;
		std::function<SocketOutB*(NodeB&, const SocketOutA&)>  createOutput;
		std::function<SocketInB*(NodeB&, const SocketInA&)>    createInput;
	};

	struct Connection
	{
		SocketOutA* fromA = nullptr;
		SocketInA*  toA   = nullptr;
		SocketOutB* fromB = nullptr;
		SocketInB*  toB   = nullptr;
	};

	GraphMorphism() = default;

	/// @brief Binds the mapping policy only; `Map()` supplies the source.
	explicit GraphMorphism(Policy policy)
		: m_policy(std::move(policy))
	{
	}

	/// @brief Binds the source graph the functor laws are checked against.
	explicit GraphMorphism(const GraphA& from, Policy policy = {})
		: m_policy(std::move(policy))
		, m_from(&from)
	{
	}

	/**
	 * @brief Drops every recorded image so another graph can be mapped.
	 *
	 * A permanent morphism (a service's mapper) reuses its policy and clears the
	 * previous images; the source and target of the old images are no longer
	 * referenced.
	 */
	void Reset()
	{
		m_nodes.clear();
		m_edges.clear();
		m_from = nullptr;
	}

	// --- Mapping ---

	/**
	 * @brief Creates the target image of one source node and records it.
	 * @return The created node, or nullptr when the policy declines or the
	 *         source node already has an image.
	 */
	NodeB* MapNode(GraphB& target, const NodeA& node)
	{
		if (!m_policy.createNode || m_nodes.contains(&node))
			return nullptr;

		NodeB* image = m_policy.createNode(target, node);
		if (!image)
			return nullptr;

		m_nodes.emplace(&node, image);
		return image;
	}

	/// @brief Records an already-created node image.
	/// @return false for a null target or a source that already has an image.
	bool MapNode(const NodeA& from, NodeB& to)
	{
		if (m_nodes.contains(&from))
			return false;
		m_nodes.emplace(&from, &to);
		return true;
	}

	/**
	 * @brief Maps one source connection: creates both target sockets, wires them
	 *        in @p target, and records the image.
	 * @return false when an endpoint is not mapped, the policy declines, or the
	 *         target refuses the edge.
	 */
	bool MapConnect(GraphB& target, SocketOutA& from, SocketInA& to)
	{
		if (to.source != &from)
			return false;

		NodeB* fromNode = ImageOf(from.owner);
		NodeB* toNode   = ImageOf(to.owner);
		if (!fromNode || !toNode)
			return false;

		auto& targets = m_edges[&from];
		if (targets.contains(&to))
			return false;

		if (!m_policy.createOutput || !m_policy.createInput)
			return false;

		SocketOutB* fromB = m_policy.createOutput(*fromNode, from);
		SocketInB* toB    = m_policy.createInput(*toNode, to);
		if (!fromB || !toB || !target.Connect(fromB, toB))
			return false;

		targets.emplace(&to, Connection{&from, &to, fromB, toB});
		return true;
	}

	/// @brief Records an already-created connection image.
	bool MapConnect(SocketOutA& fromA, SocketInA& toA,
	                SocketOutB& fromB, SocketInB& toB)
	{
		if (toA.source != &fromA)
			return false;

		auto& targets = m_edges[&fromA];
		if (targets.contains(&toA))
			return false;

		targets.emplace(&toA, Connection{&fromA, &toA, &fromB, &toB});
		return true;
	}

	/**
	 * @brief Maps every node and connection of @p graph into a new target graph.
	 *
	 * Nodes map first, so a connection can always resolve both owners. An
	 * element the policy declines stays unmapped and `Validate()` reports it.
	 *
	 * @param graph  Source graph; it must outlive the morphism.
	 * @return The built target graph.
	 */
	GraphB Map(const GraphA& graph)
	{
		Reset();
		m_from = &graph;

		GraphB target;
		for (const auto& node : graph.nodes)
			MapNode(target, *node);

		for (const ConnectionRef<GraphA>& connection : EnumerateConnections(graph))
			MapConnect(target, *connection.from, *connection.to);

		return target;
	}

	// --- Queries ---

	NodeB* ImageOf(const NodeA* node)
	{
		const auto it = m_nodes.find(node);
		return it == m_nodes.end() ? nullptr : it->second;
	}

	const NodeB* ImageOf(const NodeA* node) const
	{
		const auto it = m_nodes.find(node);
		return it == m_nodes.end() ? nullptr : it->second;
	}

	const Connection* ImageOf(const SocketOutA* from, const SocketInA* to) const
	{
		const auto fromIt = m_edges.find(from);
		if (fromIt == m_edges.end())
			return nullptr;
		const auto toIt = fromIt->second.find(to);
		return toIt == fromIt->second.end() ? nullptr : &toIt->second;
	}

	size_t MappedNodeCount() const { return m_nodes.size(); }
	size_t MappedConnectionCount() const
	{
		size_t count = 0;
		for (const auto& [from, targets] : m_edges)
		{
			(void)from;
			count += targets.size();
		}
		return count;
	}

	bool CoversAllNodes() const
	{
		if (!m_from)
			return false;
		for (const auto& node : m_from->nodes)
		{
			if (!ImageOf(node.get()))
				return false;
		}
		return true;
	}

	bool CoversAllConnections() const
	{
		if (!m_from)
			return false;
		for (const ConnectionRef<GraphA>& connection : EnumerateConnections(*m_from))
		{
			if (!ImageOf(connection.from, connection.to))
				return false;
		}
		return true;
	}

	// --- Functor laws ---

	bool IsTotal() const
	{
		return CoversAllNodes() && CoversAllConnections();
	}

	bool PreservesIncidence() const
	{
		for (const auto& [from, targets] : m_edges)
		{
			for (const auto& [to, connection] : targets)
			{
				(void)from;
				(void)to;
				if (!connection.fromA || !connection.toA
				    || !connection.fromB || !connection.toB)
					return false;
				if (ImageOf(connection.fromA->owner) != connection.fromB->owner)
					return false;
				if (ImageOf(connection.toA->owner) != connection.toB->owner)
					return false;
				if (connection.toB->source != connection.fromB)
					return false;
			}
		}
		return true;
	}

	struct Report
	{
		std::vector<std::string> problems;

		bool Ok() const { return problems.empty(); }
	};

	Report Validate() const
	{
		Report report;
		if (!m_from)
		{
			report.problems.push_back("morphism has no source graph");
			return report;
		}

		for (const auto& node : m_from->nodes)
		{
			if (!ImageOf(node.get()))
				report.problems.push_back("node '" + node->name + "' is not mapped");
		}

		for (const ConnectionRef<GraphA>& connection : EnumerateConnections(*m_from))
		{
			const std::string name =
				connection.from->owner->name + "." + connection.from->name
				+ " -> " + connection.to->owner->name + "." + connection.to->name;

			const Connection* image = ImageOf(connection.from, connection.to);
			if (!image)
			{
				report.problems.push_back("connection '" + name + "' is not mapped");
				continue;
			}

			if (ImageOf(connection.from->owner) != image->fromB->owner
			    || ImageOf(connection.to->owner) != image->toB->owner
			    || image->toB->source != image->fromB)
			{
				report.problems.push_back(
					"connection '" + name + "' is mapped to a non-matching target");
			}
		}

		return report;
	}

private:
	Policy        m_policy;
	const GraphA* m_from = nullptr;

	std::unordered_map<const NodeA*, NodeB*> m_nodes;
	std::unordered_map<const SocketOutA*,
	                   std::unordered_map<const SocketInA*, Connection>> m_edges;
};

/// @brief Records every node and connection of @p graph as its own image.
template<typename GraphT>
GraphMorphism<GraphT, GraphT> IdentityMorphism(GraphT& graph)
{
	GraphMorphism<GraphT, GraphT> morphism(graph);

	for (const auto& node : graph.nodes)
		morphism.MapNode(*node, *node);

	for (const ConnectionRef<GraphT>& connection : EnumerateConnections(graph))
		morphism.MapConnect(*connection.from, *connection.to,
		                    *connection.from, *connection.to);

	return morphism;
}

} // namespace neurus
