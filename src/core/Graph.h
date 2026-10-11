/**
 * @file Graph.h
 * @brief Template DAG node-graph with typed sockets and topological sort.
 *
 * Provides four cooperating template classes:
 *   SocketIn<SData, NData>   - input slot, fed by one upstream SocketOut
 *   SocketOut<SData, NData>  - output slot, fans out to N downstream SocketIn
 *   Node<SData, NData>       - processing unit owning its sockets + NData
 *   Graph<SData, NData>      - pool of Nodes, connections, Kahn topological sort
 *
 * Sockets hold their SData payload by value; for shared ownership pass
 * std::shared_ptr<T> as SData. Nodes are independent of Graph - they can be
 * constructed standalone and moved into a Graph later (presets).
 */

#pragma once

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace neurus {

// --- Forward declarations ---
template<typename SData, typename NData> class SocketIn;
template<typename SData, typename NData> class SocketOut;
template<typename SData, typename NData> class Node;
template<typename SData, typename NData> class Graph;

// ================================================================================
// SocketIn<SData, NData> — input slot
// ================================================================================

/**
 * @brief Input socket: holds SData, connects to at most one upstream SocketOut.
 *
 * @tparam SData  Socket-level data type (e.g. float, glm::vec4, shared_ptr<T>).
 * @tparam NData  Node-level data type (e.g. std::string, operation descriptor).
 */
template<typename SData, typename NData>
class SocketIn
{
public:
	using NodeType   = Node<SData, NData>;
	using SocketOutT = SocketOut<SData, NData>;

	std::string  name;
	SData        data{};               ///< Payload owned by this socket.
	SocketOutT*  source = nullptr;     ///< Upstream producer (null = disconnected).
	NodeType*    owner  = nullptr;     ///< Back-pointer to owning Node.

	/** @return true if a source SocketOut feeds this slot. */
	bool IsConnected() const { return source != nullptr; }
};

// ================================================================================
// SocketOut<SData, NData> — output slot
// ================================================================================

/**
 * @brief Output socket: holds SData, fans out to zero or more SocketIn targets.
 *
 * @tparam SData  Socket-level data type.
 * @tparam NData  Node-level data type.
 */
template<typename SData, typename NData>
class SocketOut
{
public:
	using NodeType  = Node<SData, NData>;
	using SocketInT = SocketIn<SData, NData>;

	std::string             name;
	SData                   data{};       ///< Output payload.
	std::vector<SocketInT*> targets;      ///< Downstream input slots.
	NodeType*               owner = nullptr;

	/** @return Number of downstream connections. */
	size_t FanOut() const { return targets.size(); }
};

// ================================================================================
// Node<SData, NData> — processing unit
// ================================================================================

/**
 * @brief Graph node owning a set of input and output sockets plus node data.
 *
 * Nodes can be constructed independently (presets) then moved into a Graph
 * via Graph::AddNode(std::unique_ptr<Node>) - socket owner pointers remain valid.
 *
 * @tparam SData  Socket-level data type.
 * @tparam NData  Node-level data type.
 */
template<typename SData, typename NData>
class Node
{
public:
	using SocketInT  = SocketIn<SData, NData>;
	using SocketOutT = SocketOut<SData, NData>;

	std::string name;
	NData       data{}; ///< Node-specific payload.

	std::vector<std::unique_ptr<SocketInT>>  inputs;
	std::vector<std::unique_ptr<SocketOutT>> outputs;

	/**
	 * @brief Create and register a new input socket.
	 * @param socketName  Display / semantic name.
	 * @return Non-owning pointer (lifecycle tied to this Node).
	 */
	SocketInT* AddInput(std::string socketName)
	{
		auto& s = inputs.emplace_back(std::make_unique<SocketInT>());
		s->name  = std::move(socketName);
		s->owner = this;
		return s.get();
	}

	/**
	 * @brief Create and register a new output socket.
	 * @param socketName  Display / semantic name.
	 * @return Non-owning pointer (lifecycle tied to this Node).
	 */
	SocketOutT* AddOutput(std::string socketName)
	{
		auto& s = outputs.emplace_back(std::make_unique<SocketOutT>());
		s->name  = std::move(socketName);
		s->owner = this;
		return s.get();
	}

	// --- Convenience accessors ---

	/** @brief Number of input sockets. */
	size_t InputCount()  const { return inputs.size(); }

	/** @brief Number of output sockets. */
	size_t OutputCount() const { return outputs.size(); }
};

// ================================================================================
// Graph<SData, NData> — DAG pool + Kahn topological sort
// ================================================================================

/**
 * @brief Owns a pool of Nodes and manages their connections.
 *
 * Provides Kahn's-algorithm topological sort. Duplicate connections and
 * self-loops are rejected by Connect(); if a SocketIn already has a source,
 * Connect() breaks the old connection before establishing the new one.
 *
 * @tparam SData  Socket-level data type.
 * @tparam NData  Node-level data type.
 */
template<typename SData, typename NData>
class Graph
{
public:
	using NodeType   = Node<SData, NData>;
	using SocketInT  = SocketIn<SData, NData>;
	using SocketOutT = SocketOut<SData, NData>;

	std::vector<std::unique_ptr<NodeType>> nodes;

	// --- Factory ---

	/**
	 * @brief Create a new node directly in the graph.
	 * @param nodeName  Display / semantic name.
	 * @param nodeData  Node-level payload.
	 * @return Non-owning pointer (lifecycle tied to this Graph).
	 */
	NodeType* AddNode(std::string nodeName, NData nodeData = {})
	{
		auto& n = nodes.emplace_back(std::make_unique<NodeType>());
		n->name = std::move(nodeName);
		n->data = std::move(nodeData);
		return n.get();
	}

	/**
	 * @brief Adopt a pre-configured node (preset) into the graph.
	 * @param node  Unique-ownership node (moved in).
	 * @return Non-owning pointer (lifecycle transferred to this Graph).
	 *
	 * @note Socket owner pointers remain valid because the Node stays at the
	 *       same heap address.
	 */
	NodeType* AddNode(std::unique_ptr<NodeType> node)
	{
		auto* raw = node.get();
		nodes.push_back(std::move(node));
		return raw;
	}

	/**
	 * @brief Remove a node and break all its connections.
	 * @param node  Non-owning pointer previously returned by AddNode().
	 * @return true if the node was found and removed.
	 */
	bool RemoveNode(NodeType* node)
	{
		auto it = std::find_if(nodes.begin(), nodes.end(),
			[node](const auto& n) { return n.get() == node; });
		if (it == nodes.end()) return false;

		// Break all incoming connections to this node's inputs
		for (auto& in : (*it)->inputs)
		{
			if (in->source)
			{
				DisconnectRaw(in->source, in.get());
			}
		}

		// Break all outgoing connections from this node's outputs
		for (auto& out : (*it)->outputs)
		{
			// Copy targets — DisconnectRaw mutates the vector
			auto targets = out->targets;
			for (auto* target : targets)
				DisconnectRaw(out.get(), target);
		}

		nodes.erase(it);
		return true;
	}

	// --- Connection management ---

	/**
	 * @brief Connect one output → one input.
	 * @return true on success, false if self-loop, duplicate, or null args.
	 *
	 * If the input already has a source connection, it is disconnected first.
	 */
	bool Connect(SocketOutT* out, SocketInT* in)
	{
		if (!out || !in) return false;

		// Reject self-loop (same owning node)
		if (out->owner == in->owner) return false;

		// Reject duplicate
		if (std::find(out->targets.begin(), out->targets.end(), in) != out->targets.end())
			return false;

		// Break existing input connection if any
		if (in->source)
			DisconnectRaw(in->source, in);

		out->targets.push_back(in);
		in->source = out;
		return true;
	}

	/**
	 * @brief Disconnect a specific output→input edge.
	 */
	void Disconnect(SocketOutT* out, SocketInT* in)
	{
		DisconnectRaw(out, in);
	}

	/** @brief True if the graph has no nodes. */
	bool Empty() const { return nodes.empty(); }

	/** @brief Number of nodes in the graph. */
	size_t NodeCount() const { return nodes.size(); }

	// --- Topological sort ---

	/**
	 * @brief Linearize nodes into execution order via Kahn's algorithm.
	 *
	 * Nodes with zero in-degree go first.  In-degree comes from downstream
	 * SocketIn connections.
	 *
	 * @return Node pointers in topological order.
	 * @throws std::runtime_error if the graph contains a directed cycle.
	 */
	std::vector<NodeType*> TopologicalSort() const
	{
		const size_t n = nodes.size();
		std::vector<size_t> inDegree(n, 0);

		for (const auto& node : nodes)
		{
			for (const auto& out : node->outputs)
			{
				for (const auto* target : out->targets)
				{
					if (target && target->owner)
					{
						auto idx = IndexOf(target->owner);
						if (idx < n) ++inDegree[idx];
					}
				}
			}
		}

		std::vector<size_t> queue;
		queue.reserve(n);
		for (size_t i = 0; i < n; ++i)
			if (inDegree[i] == 0)
				queue.push_back(i);

		std::vector<NodeType*> sorted;
		sorted.reserve(n);
		while (!queue.empty())
		{
			size_t idx = queue.back();
			queue.pop_back();
			sorted.push_back(nodes[idx].get());

			for (const auto& out : nodes[idx]->outputs)
			{
				for (const auto* target : out->targets)
				{
					if (!target || !target->owner) continue;
					auto tgtIdx = IndexOf(target->owner);
					if (tgtIdx >= n) continue;
					if (--inDegree[tgtIdx] == 0)
						queue.push_back(tgtIdx);
				}
			}
		}

		// Cycle check: if not every node was emitted, the unprocessed nodes
		// (in-degree still > 0) are the ones on/downstream of a cycle. Name
		// them so a misconfigured graph is debuggable in one read.
		if (sorted.size() != n)
		{
			std::string cyclic;
			for (size_t i = 0; i < n; ++i)
			{
				if (inDegree[i] > 0)
				{
					if (!cyclic.empty()) cyclic += ", ";
					cyclic += nodes[i]->name;
				}
			}
			throw std::runtime_error(
				"Graph::TopologicalSort: cycle detected involving nodes: " + cyclic);
		}

		return sorted;
	}

	/**
	 * @brief Test whether the graph contains a directed cycle.
	 * @return true if a cycle exists (TopologicalSort would throw).
	 */
	bool HasCycle() const
	{
		try { TopologicalSort(); return false; }
		catch (const std::runtime_error&) { return true; }
	}

private:
	// Internal raw disconnect (skips public validity checks)
	void DisconnectRaw(SocketOutT* out, SocketInT* in)
	{
		if (!out || !in) return;
		auto it = std::find(out->targets.begin(), out->targets.end(), in);
		if (it != out->targets.end())
		{
			out->targets.erase(it);
			in->source = nullptr;
		}
	}

	// O(n) lookup; revisit if node counts grow past the hundreds
	size_t IndexOf(const NodeType* node) const
	{
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i].get() == node)
				return i;
		return nodes.size();
	}
};

} // namespace neurus
