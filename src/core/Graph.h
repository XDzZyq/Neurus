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
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
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
	 * @return true on success, false if self-loop, duplicate, null args, or
	 *         the edge would close a directed cycle.
	 *
	 * If the input already has a source connection, it is disconnected first.
	 * Cycle rejection keeps the graph acyclic by construction, so a bad edge
	 * fails *here* (localized to the offending Connect) rather than later at
	 * Compile()/traversal time. The traversal routines keep their own cycle
	 * guards too — the public sockets can be wired outside Connect, so this is
	 * eager validation layered on top, not a replacement.
	 */
	bool Connect(SocketOutT* out, SocketInT* in)
	{
		if (!out || !in) return false;

		// Reject self-loop (same owning node)
		if (out->owner == in->owner) return false;

		// Reject duplicate
		if (std::find(out->targets.begin(), out->targets.end(), in) != out->targets.end())
			return false;

		// Reject cycle-closing edges: adding out→in makes out->owner a
		// producer of in->owner, so it is illegal iff in->owner can already
		// reach out->owner along existing edges.
		if (Reaches(in->owner, out->owner)) return false;

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
		const Topology topo = BuildTopology();
		if (!topo.acyclic)
			throw std::runtime_error(
				"Graph::TopologicalSort: cycle detected involving nodes: "
				+ CyclicNodeNames(topo));

		std::vector<NodeType*> sorted;
		sorted.reserve(topo.order.size());
		for (size_t idx : topo.order)
			sorted.push_back(nodes[idx].get());
		return sorted;
	}

	/**
	 * @brief Visit every node once, in dependency (topological) order, on the
	 *        calling thread.
	 *
	 * The serial counterpart to ParallelTraverse: `visit(node)` is invoked
	 * exactly once per node, and every one of a node's direct producers is
	 * visited before it. No executor, no threads — the single guarantee is the
	 * ordering, which makes it the natural choice for the serial assembly path
	 * (and a deterministic baseline for debugging a visitor against the
	 * concurrent traverse). Because visits never overlap, the visitor needs no
	 * internal synchronization.
	 *
	 * A `visit` exception propagates immediately and stops the walk, so later
	 * nodes are not visited.
	 *
	 * @param visit  Per-node work; called once per node after its producers.
	 * @throws std::runtime_error if the graph contains a directed cycle.
	 * @throws (propagated) whatever a `visit` invocation throws.
	 */
	void SequentialTraverse(const std::function<void(NodeType*)>& visit) const
	{
		const Topology topo = BuildTopology();
		if (!topo.acyclic)
			throw std::runtime_error(
				"Graph::SequentialTraverse: cycle detected involving nodes: "
				+ CyclicNodeNames(topo));

		for (size_t idx : topo.order)
			visit(nodes[idx].get());
	}

	/**
	 * @brief Execute a visitor over every node, respecting dependencies, with
	 *        independent nodes dispatched concurrently through an executor.
	 *
	 * Completion-driven dataflow (not a static wavefront): `visit(node)` is
	 * invoked exactly once, and only after every one of that node's direct
	 * producers has *returned* from its own `visit`. A node fires the instant
	 * its last producer finishes — dependencies are honoured at edge
	 * granularity, with no coarse level barriers, so a fast branch never waits
	 * on a slow sibling it does not actually depend on. Nodes with no unmet
	 * dependency may run simultaneously. The call blocks until every node has
	 * been visited (the frontier fully drains), then rethrows the first
	 * exception thrown by any `visit`, if one occurred.
	 *
	 * The `Executor` is intentionally minimal: it need only expose
	 * `submit(std::function<void()>)` to run a task on some worker. This keeps
	 * the generic graph free of any concrete thread-pool or platform
	 * dependency — a trivial inline executor (running tasks synchronously) and
	 * a std::jthread pool satisfy it equally, and the method is unit-testable
	 * with no GPU. The visitor runs *outside* all internal locks, so visits may
	 * genuinely overlap; only the small readiness bookkeeping is serialized.
	 *
	 * On a directed cycle the dependency frontier could never drain, so the
	 * cycle is detected up front and reported via std::runtime_error *before*
	 * any task is dispatched — the method never deadlocks on a cyclic graph.
	 * If a `visit` throws, no further nodes are dispatched; already-running
	 * visits finish, in-flight tasks drain, and the first exception is
	 * rethrown to the caller.
	 *
	 * @tparam Executor  Any type with `submit(std::function<void()>)`.
	 * @param visit  Per-node work; called once per node after its producers.
	 * @param exec   Executor used to run ready nodes.
	 * @throws std::runtime_error if the graph contains a directed cycle.
	 * @throws (rethrown) the first exception any `visit` invocation threw.
	 */
	template<typename Executor>
	void ParallelTraverse(const std::function<void(NodeType*)>& visit,
	                      Executor& exec) const
	{
		const size_t n = nodes.size();
		if (n == 0)
			return;

		// Shared edge walk: successor adjacency + in-degrees, plus an up-front
		// cycle result. A cycle leaves some node permanently un-ready, which
		// would hang the traverse, so reject it *before* any dispatch.
		const Topology topo = BuildTopology();
		if (!topo.acyclic)
			throw std::runtime_error(
				"Graph::ParallelTraverse: cycle detected involving nodes: "
				+ CyclicNodeNames(topo));

		const std::vector<std::vector<size_t>>& successors = topo.successors;
		const std::vector<size_t>&              inDegree   = topo.inDegree;

		// --- Shared traversal state (all guarded by `m`) ---
		std::mutex              m;
		std::condition_variable cv;
		std::vector<size_t>     remaining = inDegree;  // unmet producers per node
		size_t                  active    = 0;         // submitted-but-not-finished
		std::exception_ptr      firstError;
		bool                    failed    = false;

		// Worker body for one node. visit() runs outside the lock so visits
		// overlap; only readiness bookkeeping is serialized under `m`.
		std::function<void(size_t)> run = [&](size_t i)
		{
			bool skip;
			{
				std::lock_guard<std::mutex> lk(m);
				skip = failed;
			}

			if (!skip)
			{
				try
				{
					visit(nodes[i].get());
				}
				catch (...)
				{
					std::lock_guard<std::mutex> lk(m);
					if (!failed)
					{
						failed     = true;
						firstError = std::current_exception();
					}
				}
			}

			std::vector<size_t> ready;
			{
				std::lock_guard<std::mutex> lk(m);
				// Only promote successors while healthy; on failure we stop
				// feeding the frontier and let the in-flight tasks drain.
				if (!failed)
				{
					for (size_t t : successors[i])
						if (--remaining[t] == 0)
							ready.push_back(t);
				}
				active += ready.size();  // adopt children before releasing self
				--active;                // this task is finishing
				if (active == 0)
					cv.notify_all();
			}

			for (size_t t : ready)
				exec.submit([&run, t] { run(t); });
		};

		// Seed every zero-in-degree node; count them before dispatch so the
		// caller cannot observe a transient active==0 between seeds.
		std::vector<size_t> seeds;
		for (size_t i = 0; i < n; ++i)
			if (inDegree[i] == 0)
				seeds.push_back(i);
		{
			std::lock_guard<std::mutex> lk(m);
			active += seeds.size();
		}
		for (size_t i : seeds)
			exec.submit([&run, i] { run(i); });

		// Block until the frontier fully drains (success or first failure).
		{
			std::unique_lock<std::mutex> lk(m);
			cv.wait(lk, [&] { return active == 0; });
		}

		if (firstError)
			std::rethrow_exception(firstError);
	}


	/**
	 * @brief Test whether the graph contains a directed cycle.
	 * @return true if a cycle exists (TopologicalSort would throw).
	 */
	bool HasCycle() const
	{
		return !BuildTopology().acyclic;
	}

private:
	/**
	 * @brief Reusable topology snapshot: the edge walk every traversal needs.
	 *
	 * `successors`/`inDegree` are the adjacency and producer counts derived
	 * from the socket edges (indices into `nodes`); `order` is a Kahn
	 * topological linearization and `acyclic` is true iff it covers every node
	 * (`order.size() == nodes.size()`). Bundling them lets TopologicalSort,
	 * ParallelTraverse and HasCycle share one O(V+E) walk instead of each
	 * re-deriving the same data.
	 */
	struct Topology
	{
		std::vector<std::vector<size_t>> successors;  // adjacency by node index
		std::vector<size_t>              inDegree;     // direct producers per node
		std::vector<size_t>              order;        // Kahn order (node indices)
		bool                             acyclic = false;
	};

	// Build successor adjacency + in-degrees from the socket edges, then run
	// Kahn's algorithm (LIFO frontier, matching the historical TopologicalSort
	// ordering) to produce a topological order. A cycle shows up as an `order`
	// shorter than `nodes` — recorded in `acyclic`, no throw here.
	Topology BuildTopology() const
	{
		const size_t n = nodes.size();
		Topology topo;
		topo.successors.assign(n, {});
		topo.inDegree.assign(n, 0);

		for (size_t i = 0; i < n; ++i)
			for (const auto& out : nodes[i]->outputs)
				for (const auto* target : out->targets)
				{
					if (!target || !target->owner) continue;
					size_t t = IndexOf(target->owner);
					if (t >= n) continue;
					topo.successors[i].push_back(t);
					++topo.inDegree[t];
				}

		std::vector<size_t> deg = topo.inDegree;  // scratch: preserve inDegree
		std::vector<size_t> stack;
		stack.reserve(n);
		for (size_t i = 0; i < n; ++i)
			if (deg[i] == 0) stack.push_back(i);

		topo.order.reserve(n);
		while (!stack.empty())
		{
			size_t i = stack.back();
			stack.pop_back();
			topo.order.push_back(i);
			for (size_t t : topo.successors[i])
				if (--deg[t] == 0) stack.push_back(t);
		}

		topo.acyclic = (topo.order.size() == n);
		return topo;
	}

	// Comma-separated names of the nodes on/downstream of a cycle: those
	// absent from `topo.order`. Shared by the throwing traversal callers so
	// the diagnostic reads identically regardless of entry point.
	std::string CyclicNodeNames(const Topology& topo) const
	{
		const size_t n = nodes.size();
		std::vector<bool> emitted(n, false);
		for (size_t idx : topo.order) emitted[idx] = true;

		std::string cyclic;
		for (size_t i = 0; i < n; ++i)
			if (!emitted[i])
			{
				if (!cyclic.empty()) cyclic += ", ";
				cyclic += nodes[i]->name;
			}
		return cyclic;
	}

	// True if `to` is reachable from `from` along existing successor edges.
	// Side-effect-free iterative DFS — used by Connect() to reject a new edge
	// that would close a directed cycle, without any tentative insert/rollback.
	bool Reaches(const NodeType* from, const NodeType* to) const
	{
		if (from == to) return true;
		const size_t n = nodes.size();
		const size_t start = IndexOf(from);
		if (start >= n) return false;

		std::vector<bool>   visited(n, false);
		std::vector<size_t> stack{ start };
		visited[start] = true;
		while (!stack.empty())
		{
			size_t i = stack.back();
			stack.pop_back();
			for (const auto& out : nodes[i]->outputs)
				for (const auto* target : out->targets)
				{
					if (!target || !target->owner) continue;
					if (target->owner == to) return true;
					size_t t = IndexOf(target->owner);
					if (t < n && !visited[t])
					{
						visited[t] = true;
						stack.push_back(t);
					}
				}
		}
		return false;
	}

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
