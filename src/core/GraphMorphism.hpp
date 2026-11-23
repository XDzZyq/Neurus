/**
 * @file GraphMorphism.hpp
 * @brief One-way mapping policies for typed graphs.
 */

#pragma once

#include "core/Graph.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neurus {

/** @brief Maps source nodes and connections with a reusable construction policy. */
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

	/** A factory may return nullptr to decline an element. Validation names each missing image. */
	struct Policy
	{
		std::function<NodeB*(GraphB&, const NodeA&)>          createNode;
		std::function<SocketOutB*(NodeB&, const SocketOutA&)> createOutput;
		std::function<SocketInB*(NodeB&, const SocketInA&)>   createInput;
	};

	struct Connection
	{
		const SocketOutA* fromA;
		const SocketInA*  toA;
		const SocketOutB* fromB;
		const SocketInB*  toB;
	};

	struct Report
	{
		std::vector<std::string> problems;
		bool Ok() const { return problems.empty(); }
	};

	/**
	 * @brief Images of nodes and connections for one mapping.
	 *
	 * The graphs own the nodes and sockets. Keep both alive while inspecting
	 * their images. Independent mappings have independent correspondence tables.
	 */
	class Correspondence
	{
	public:
		bool RecordNode(const NodeA& from, NodeB& to)
		{
			return m_nodes.emplace(&from, &to).second;
		}

		bool RecordConnection(const SocketOutA& fromA, const SocketInA& toA,
		                      SocketOutB& fromB, SocketInB& toB)
		{
			// An input has one producer, so it uniquely identifies its source edge.
			return toA.source == &fromA
			    && m_edges.emplace(&toA, Connection{&fromA, &toA, &fromB, &toB}).second;
		}

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
			const auto it = m_edges.find(to);
			return it != m_edges.end() && it->second.fromA == from ? &it->second : nullptr;
		}

		size_t MappedNodeCount() const { return m_nodes.size(); }
		size_t MappedConnectionCount() const { return m_edges.size(); }

		bool IsTotal(const GraphA& source) const
		{
			for (const auto& node : source.nodes)
				if (!ImageOf(node.get()))
					return false;
			for (const auto& edge : EnumerateConnections(source))
				if (!ImageOf(edge.from, edge.to))
					return false;
			return true;
		}

		bool PreservesIncidence() const
		{
			for (const auto& entry : m_edges)
				if (!MatchesEndpoints(entry.second))
					return false;
			return true;
		}

		Report Validate(const GraphA& source) const
		{
			Report report;
			for (const auto& node : source.nodes)
				if (!ImageOf(node.get()))
					report.problems.push_back("node '" + node->name + "' is not mapped");

			for (const auto& edge : EnumerateConnections(source))
			{
				const Connection* image = ImageOf(edge.from, edge.to);
				if (image && MatchesEndpoints(*image))
					continue;

				const std::string name = edge.from->owner->name + "." + edge.from->name
				                       + " -> " + edge.to->owner->name + "." + edge.to->name;
				report.problems.push_back("connection '" + name +
					(image ? "' is mapped to a non-matching target" : "' is not mapped"));
			}
			return report;
		}

	private:
		bool MatchesEndpoints(const Connection& edge) const
		{
			return edge.toA->source == edge.fromA
			    && ImageOf(edge.fromA->owner) == edge.fromB->owner
			    && ImageOf(edge.toA->owner) == edge.toB->owner
			    && edge.toB->source == edge.fromB;
		}

		std::unordered_map<const NodeA*, NodeB*> m_nodes;
		std::unordered_map<const SocketInA*, Connection> m_edges;
	};

	struct Result
	{
		GraphB target;
		Correspondence images;
	};

	explicit GraphMorphism(Policy policy) : m_policy(std::move(policy)) {}

	/** @brief Creates the image of a single node in this result. */
	NodeB* MapNode(Result& result, const NodeA& node) const
	{
		if (!m_policy.createNode || result.images.ImageOf(&node))
			return nullptr;
		NodeB* image = m_policy.createNode(result.target, node);
		if (image)
			result.images.RecordNode(node, *image);
		return image;
	}

	/** @brief Resolves both owners and records a connected pair of target sockets. */
	bool MapConnect(Result& result, const SocketOutA& from, const SocketInA& to) const
	{
		if (to.source != &from || result.images.ImageOf(&from, &to)
		    || !m_policy.createOutput || !m_policy.createInput)
			return false;

		NodeB* fromNode = result.images.ImageOf(from.owner);
		NodeB* toNode = result.images.ImageOf(to.owner);
		if (!fromNode || !toNode)
			return false;

		SocketOutB* output = m_policy.createOutput(*fromNode, from);
		SocketInB* input = m_policy.createInput(*toNode, to);
		if (!output || !input)
			return false;
		if (input->source != output && !result.target.Connect(output, input))
			return false;

		return result.images.RecordConnection(from, to, *output, *input);
	}

	/** @brief Creates a fresh target and correspondence for the supplied source. */
	Result Map(const GraphA& source) const
	{
		Result result;
		for (const auto& node : source.nodes)
			MapNode(result, *node);
		for (const auto& edge : EnumerateConnections(source))
			MapConnect(result, *edge.from, *edge.to);
		return result;
	}

private:
	Policy m_policy;
};

/** @brief Maps each element to itself. The returned images borrow the supplied graph. */
template<typename GraphT>
auto IdentityMorphism(GraphT& graph)
{
	typename GraphMorphism<GraphT, GraphT>::Correspondence images;
	for (const auto& node : graph.nodes)
		images.RecordNode(*node, *node);
	for (const auto& edge : EnumerateConnections(graph))
		images.RecordConnection(*edge.from, *edge.to, *edge.from, *edge.to);
	return images;
}

} // namespace neurus
