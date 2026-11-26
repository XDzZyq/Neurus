/**
 * @file GraphMorphism.hpp
 * @brief One-way mapping policies for typed graphs.
 */

#pragma once

#include "core/Graph.hpp"

#include <functional>
#include <memory>
#include <optional>
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

	/** Maps source elements to target payloads. An empty result declines the mapping. */
	struct Policy
	{
		std::function<std::optional<decltype(NodeB::data)>(const NodeA&)> createNode;
		std::function<std::optional<decltype(SocketOutB::data)>(const SocketOutA&)> createOutput;
		std::function<std::optional<decltype(SocketInB::data)>(const SocketInA&)> createInput;
	};

	struct Report
	{
		std::vector<std::string> problems;
		bool Ok() const { return problems.empty(); }
	};

	/**
	 * @brief Images of nodes, sockets and connections for one mapping.
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

		bool RecordSocket(const SocketInA& source, SocketInB& image)
		{
			return m_inputs.emplace(&source, &image).second;
		}

		bool RecordSocket(const SocketOutA& source, SocketOutB& image)
		{
			return m_outputs.emplace(&source, &image).second;
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

		SocketInB* ImageOf(const SocketInA* socket) { return FindSocket(m_inputs, socket); }
		const SocketInB* ImageOf(const SocketInA* socket) const { return FindSocket(m_inputs, socket); }
		SocketOutB* ImageOf(const SocketOutA* socket) { return FindSocket(m_outputs, socket); }
		const SocketOutB* ImageOf(const SocketOutA* socket) const { return FindSocket(m_outputs, socket); }

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
			{
				if (!ImageOf(node.get()))
					return false;
				for (const auto& input : node->inputs)
					if (!ImageOf(input.get()))
						return false;
				for (const auto& output : node->outputs)
					if (!ImageOf(output.get()))
						return false;
			}
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
			{
				if (!ImageOf(node.get()))
				{
					report.problems.push_back("node '" + node->name + "' is not mapped");
					continue;
				}
				for (const auto& input : node->inputs)
					if (!ImageOf(input.get()))
						report.problems.push_back("input socket '" + node->name + "." + input->name + "' is not mapped");
				for (const auto& output : node->outputs)
					if (!ImageOf(output.get()))
						report.problems.push_back("output socket '" + node->name + "." + output->name + "' is not mapped");
			}

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
		template<class SocketA, class SocketB>
		static SocketB* FindSocket(const std::unordered_map<const SocketA*, SocketB*>& sockets, const SocketA* source)
		{
			const auto it = sockets.find(source);
			return it == sockets.end() ? nullptr : it->second;
		}

		bool MatchesEndpoints(const ConnectionA& edge, const ConnectionB& image) const
		{
			return edge.from && image.from
			    && ImageOf(edge.from) == image.from
			    && ImageOf(edge.to) == image.to
			    && ImageOf(edge.from->owner) == image.from->owner
			    && ImageOf(edge.to->owner) == image.to->owner;
		}

		std::unordered_map<const NodeA*, NodeB*> m_nodes;
		std::unordered_map<const SocketInA*, SocketInB*> m_inputs;
		std::unordered_map<const SocketOutA*, SocketOutB*> m_outputs;
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
		auto value = m_policy.createNode(node);
		if (!value)
			return nullptr;

		NodeB* image = result.target.AddNode(node.name, std::move(*value));
		result.images.RecordNode(node, *image);
		MapSockets(node.inputs, m_policy.createInput, result.images,
		           [image](std::string name) { return image->AddInput(std::move(name)); });
		MapSockets(node.outputs, m_policy.createOutput, result.images,
		           [image](std::string name) { return image->AddOutput(std::move(name)); });
		return image;
	}

	/** @brief Resolves both owners and records a connected pair of target sockets. */
	bool MapConnect(Result& result, const ConnectionA& edge) const
	{
		if (!edge.from || result.images.ImageOf(&edge))
			return false;

		SocketOutB* output = result.images.ImageOf(edge.from);
		SocketInB* input = result.images.ImageOf(edge.to);
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
	template<class SocketA, class CreateSocket, class AddSocket>
	static void MapSockets(const std::vector<std::unique_ptr<SocketA>>& source,
	                       const CreateSocket& create, Correspondence& images, AddSocket add)
	{
		if (!create)
			return;
		for (const auto& socket : source)
		{
			auto value = create(*socket);
			if (!value)
				continue;

			auto* image = add(socket->name);
			image->data = std::move(*value);
			images.RecordSocket(*socket, *image);
		}
	}

	Policy m_policy;
};

/** @brief Maps each element to itself. The returned images borrow the supplied graph. */
template<typename GraphT>
auto IdentityMorphism(GraphT& graph)
{
	typename GraphMorphism<GraphT, GraphT>::Correspondence images;
	for (const auto& node : graph.nodes)
	{
		images.RecordNode(*node, *node);
		for (const auto& input : node->inputs)
			images.RecordSocket(*input, *input);
		for (const auto& output : node->outputs)
			images.RecordSocket(*output, *output);
	}
	for (const auto* edge : graph.Connections())
		images.RecordConnection(*edge, *edge);
	return images;
}

} // namespace neurus
