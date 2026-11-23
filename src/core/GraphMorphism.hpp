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
	using ConnectionA = typename GraphA::ConnectionT;
	using ConnectionB = typename GraphB::ConnectionT;

	/** A factory may return nullptr to decline an element. Validation names each missing image. */
	struct Policy
	{
		std::function<NodeB*(GraphB&, const NodeA&)>          createNode;
		std::function<SocketOutB*(NodeB&, const SocketOutA&)> createOutput;
		std::function<SocketInB*(NodeB&, const SocketInA&)>   createInput;
	};

	struct Report
	{
		std::vector<std::string> problems;
		bool Ok() const { return problems.empty(); }
	};

	/**
	 * @brief Images of nodes and connections for one mapping.
	 *
	 * The correspondence borrows graph-owned nodes and incoming connections. Keep
	 * mapped sockets and nodes alive. Rebuild the mapping after rewiring them.
	 */
	class Correspondence
	{
	public:
		bool RecordNode(const NodeA& from, NodeB& to)
		{
			return m_nodes.emplace(&from, &to).second;
		}

		bool RecordConnection(const ConnectionA& edge, const ConnectionB& image)
		{
			return edge.from && image.from && m_edges.emplace(&edge, &image).second;
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

		const ConnectionB* ImageOf(const ConnectionA* edge) const
		{
			const auto it = m_edges.find(edge);
			return it == m_edges.end() ? nullptr : it->second;
		}

		size_t MappedNodeCount() const { return m_nodes.size(); }
		size_t MappedConnectionCount() const { return m_edges.size(); }

		bool IsTotal(const GraphA& source) const
		{
			for (const auto& node : source.nodes)
				if (!ImageOf(node.get()))
					return false;
			for (const auto* edge : source.Connections())
				if (!ImageOf(edge))
					return false;
			return true;
		}

		bool PreservesIncidence() const
		{
			for (const auto& [edge, image] : m_edges)
				if (!MatchesEndpoints(*edge, *image))
					return false;
			return true;
		}

		Report Validate(const GraphA& source) const
		{
			Report report;
			for (const auto& node : source.nodes)
				if (!ImageOf(node.get()))
					report.problems.push_back("node '" + node->name + "' is not mapped");

			for (const auto* edge : source.Connections())
			{
				const ConnectionB* image = ImageOf(edge);
				if (image && MatchesEndpoints(*edge, *image))
					continue;

				const std::string name = edge->from->owner->name + "." + edge->from->name
				                       + " -> " + edge->to->owner->name + "." + edge->to->name;
				report.problems.push_back("connection '" + name +
					(image ? "' is mapped to a non-matching target" : "' is not mapped"));
			}
			return report;
		}

	private:
		bool MatchesEndpoints(const ConnectionA& edge, const ConnectionB& image) const
		{
			return edge.from && image.from
			    && ImageOf(edge.from->owner) == image.from->owner
			    && ImageOf(edge.to->owner) == image.to->owner;
		}

		std::unordered_map<const NodeA*, NodeB*> m_nodes;
		std::unordered_map<const ConnectionA*, const ConnectionB*> m_edges;
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
	bool MapConnect(Result& result, const ConnectionA& edge) const
	{
		if (!edge.from || result.images.ImageOf(&edge)
		    || !m_policy.createOutput || !m_policy.createInput)
			return false;

		NodeB* fromNode = result.images.ImageOf(edge.from->owner);
		NodeB* toNode = result.images.ImageOf(edge.to->owner);
		if (!fromNode || !toNode)
			return false;

		SocketOutB* output = m_policy.createOutput(*fromNode, *edge.from);
		SocketInB* input = m_policy.createInput(*toNode, *edge.to);
		if (!output || !input)
			return false;
		// Two source edges can share an image, provided they resolve to the same producer.
		if (!input->IsConnected())
			result.target.Connect(output, input);
		if (input->incoming.from != output)
			return false;

		return result.images.RecordConnection(edge, input->incoming);
	}

	/** @brief Creates a fresh target and correspondence for the supplied source. */
	Result Map(const GraphA& source) const
	{
		Result result;
		for (const auto& node : source.nodes)
			MapNode(result, *node);
		for (const auto* edge : source.Connections())
			MapConnect(result, *edge);
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
	for (const auto* edge : graph.Connections())
		images.RecordConnection(*edge, *edge);
	return images;
}

} // namespace neurus
