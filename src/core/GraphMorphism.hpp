/**
 * @file GraphMorphism.hpp
 * @brief Functor between two graphs: node and connection images plus the laws.
 *
 * A graph is a small category: nodes are objects and connections are
 * morphisms. A map from one graph to another is a functor when it is total and
 * preserves incidence. Nothing here asks for an inverse, an injection or a
 * surjection; the map is deliberately one-way.
 */

#pragma once

#include "core/Graph.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neurus {

/// @brief One connection of a graph: producer socket and consumer socket.
template<typename GraphT>
struct ConnectionRef
{
	typename GraphT::SocketOutT* from = nullptr;
	typename GraphT::SocketInT*  to   = nullptr;
};

/**
 * @brief Every connection in the graph, one entry per connected input socket.
 *
 * Iterating inputs, not outputs, makes a fan-out produce one entry per consumer.
 */
template<typename GraphT>
std::vector<ConnectionRef<GraphT>> EnumerateConnections(const GraphT& graph)
{
	std::vector<ConnectionRef<GraphT>> connections;
	for (const auto& node : graph.nodes)
	{
		for (const auto& input : node->inputs)
		{
			if (!input->IsConnected())
				continue;
			connections.push_back({input->source, input.get()});
		}
	}
	return connections;
}

/**
 * @brief Functor from one graph to another.
 *
 * Records the node image and the connection image and checks the two functor
 * laws: totality and incidence preservation.
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

	struct Connection
	{
		SocketOutA* fromA = nullptr;
		SocketInA*  toA   = nullptr;
		SocketOutB* fromB = nullptr;
		SocketInB*  toB   = nullptr;
	};

	GraphMorphism() = default;
	GraphMorphism(const GraphA& from, const GraphB& to)
		: m_from(&from)
		, m_to(&to)
	{
		for (const auto& node : from.nodes)
		{
			m_fromNodes.insert(node.get());
			for (const auto& input : node->inputs)
				m_fromInputs.insert(input.get());
			for (const auto& output : node->outputs)
				m_fromOutputs.insert(output.get());
		}
		for (const auto& node : to.nodes)
		{
			m_toNodes.insert(node.get());
			for (const auto& input : node->inputs)
				m_toInputs.insert(input.get());
			for (const auto& output : node->outputs)
				m_toOutputs.insert(output.get());
		}
	}

	// --- Recording ---

	/**
	 * @brief Records a node image.
	 * @return false for a null/foreign pointer or a source that already has an image.
	 *
	 * A target already used by another source is accepted: injectivity is not a law.
	 */
	bool MapNode(NodeA* from, NodeB* to)
	{
		if (!from || !to || !m_fromNodes.contains(from) || !m_toNodes.contains(to))
			return false;
		if (m_nodes.contains(from))
			return false;

		m_nodes.emplace(from, to);
		return true;
	}

	/// @brief Records a connection image; same refusals as MapNode.
	bool MapConnection(SocketOutA* fromA, SocketInA* toA,
	                   SocketOutB* fromB, SocketInB* toB)
	{
		if (!fromA || !toA || !fromB || !toB)
			return false;
		if (!m_fromOutputs.contains(fromA) || !m_fromInputs.contains(toA))
			return false;
		if (!m_toOutputs.contains(fromB) || !m_toInputs.contains(toB))
			return false;

		auto& targets = m_edges[fromA];
		if (targets.contains(toA))
			return false;

		targets.emplace(toA, Connection{fromA, toA, fromB, toB});
		return true;
	}

	// --- Queries ---

	NodeB* ImageOf(const NodeA* node) const
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
		if (!m_from || !m_to)
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
		if (!m_from || !m_to)
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
		if (!m_from || !m_to)
		{
			report.problems.push_back("morphism has no source or target graph");
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
	const GraphA* m_from = nullptr;
	const GraphB* m_to   = nullptr;

	std::unordered_set<const NodeA*>      m_fromNodes;
	std::unordered_set<const NodeB*>      m_toNodes;
	std::unordered_set<const SocketInA*>  m_fromInputs;
	std::unordered_set<const SocketOutA*> m_fromOutputs;
	std::unordered_set<const SocketInB*>  m_toInputs;
	std::unordered_set<const SocketOutB*> m_toOutputs;

	std::unordered_map<const NodeA*, NodeB*> m_nodes;
	std::unordered_map<const SocketOutA*,
	                   std::unordered_map<const SocketInA*, Connection>> m_edges;
};

/// @brief Maps every node and connection of @p graph to itself.
template<typename GraphT>
GraphMorphism<GraphT, GraphT> IdentityMorphism(GraphT& graph)
{
	GraphMorphism<GraphT, GraphT> morphism(graph, graph);

	for (const auto& node : graph.nodes)
		morphism.MapNode(node.get(), node.get());

	for (const ConnectionRef<GraphT>& connection : EnumerateConnections(graph))
		morphism.MapConnection(connection.from, connection.to,
		                       connection.from, connection.to);

	return morphism;
}

/// @brief How MapGraph creates the target graph's objects for a source graph.
template<typename GraphA, typename GraphB>
struct GraphMapPolicy
{
	using NodeA      = typename GraphA::NodeType;
	using NodeB      = typename GraphB::NodeType;
	using SocketInA  = typename GraphA::SocketInT;
	using SocketOutA = typename GraphA::SocketOutT;
	using SocketInB  = typename GraphB::SocketInT;
	using SocketOutB = typename GraphB::SocketOutT;

	std::function<NodeB*(const NodeA&)>                         mapNode;
	std::function<SocketOutB*(const NodeA&, const SocketOutA&)> mapOutput;
	std::function<SocketInB*(const NodeA&, const SocketInA&)>   mapInput;
};

/**
 * @brief Applies @p policy to build @p to from @p from and returns the morphism.
 *
 * A null factory result, or a target Connect that refuses the edge, is left
 * unmapped so Validate() reports it by name.
 */
template<typename GraphA, typename GraphB>
GraphMorphism<GraphA, GraphB> MapGraph(const GraphA& from, GraphB& to,
                                       const GraphMapPolicy<GraphA, GraphB>& policy)
{
	GraphMorphism<GraphA, GraphB> morphism(from, to);

	std::unordered_map<const typename GraphA::NodeType*,
	                   typename GraphB::NodeType*> nodeImages;

	for (const auto& node : from.nodes)
	{
		typename GraphB::NodeType* toNode = policy.mapNode(*node);
		if (!toNode)
			continue;
		if (!morphism.MapNode(node.get(), toNode))
			continue;
		nodeImages.emplace(node.get(), toNode);
	}

	for (const ConnectionRef<GraphA>& connection : EnumerateConnections(from))
	{
		const auto fromIt = nodeImages.find(connection.from->owner);
		const auto toIt   = nodeImages.find(connection.to->owner);
		if (fromIt == nodeImages.end() || toIt == nodeImages.end())
			continue;

		typename GraphB::SocketOutT* fromB =
			policy.mapOutput(*connection.from->owner, *connection.from);
		typename GraphB::SocketInT* toB =
			policy.mapInput(*connection.to->owner, *connection.to);
		if (!fromB || !toB)
			continue;

		if (!to.Connect(fromB, toB))
			continue;

		morphism.MapConnection(connection.from, connection.to, fromB, toB);
	}

	return morphism;
}

} // namespace neurus
