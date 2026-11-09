/**
 * @file test_graph.cpp
 * @brief Unit tests for the DAG node-graph template classes (Graph.h).
 */

#include <gtest/gtest.h>

#include "core/Graph.h"
#include "core/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace neurus;

// ============================================================================
// Test type aliases — use simple types for clarity
// ============================================================================

using SData    = float;          ///< Socket payload is a scalar float.
using NData    = std::string;    ///< Node payload is an operation label.
using SockIn   = SocketIn<SData, NData>;
using SockOut  = SocketOut<SData, NData>;
using NodeT    = Node<SData, NData>;
using GraphT   = Graph<SData, NData>;

// Wire a raw output→input edge directly, bypassing Graph::Connect's cycle
// rejection. Now that Connect keeps the graph acyclic by construction, this is
// the only way to build a cyclic graph — used to exercise the traversal-time
// cycle guards that still backstop Connect against sockets wired by hand.
static void ForceEdge(SockOut* out, SockIn* in)
{
	out->targets.push_back(in);
	in->source = out;
}

// ============================================================================
// Node tests (independent of Graph)
// ============================================================================

TEST(NodeTest, ConstructEmpty)
{
	NodeT node;
	EXPECT_EQ(node.InputCount(), 0u);
	EXPECT_EQ(node.OutputCount(), 0u);
	EXPECT_TRUE(node.name.empty());
}

TEST(NodeTest, AddSockets)
{
	NodeT node;
	node.name = "TestNode";

	auto* in = node.AddInput("a");
	auto* out = node.AddOutput("b");

	ASSERT_NE(in, nullptr);
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(in->name, "a");
	EXPECT_EQ(out->name, "b");
	EXPECT_EQ(in->owner, &node);
	EXPECT_EQ(out->owner, &node);
	EXPECT_EQ(node.InputCount(), 1u);
	EXPECT_EQ(node.OutputCount(), 1u);
	EXPECT_EQ(in->data, 0.0f);      // default SData
	EXPECT_FALSE(in->IsConnected());
}

TEST(NodeTest, SocketData)
{
	NodeT node;
	auto* in  = node.AddInput("x");
	auto* out = node.AddOutput("y");

	in->data  = 3.14f;
	out->data = 2.71f;

	EXPECT_FLOAT_EQ(in->data, 3.14f);
	EXPECT_FLOAT_EQ(out->data, 2.71f);
}

TEST(NodeTest, OwnsSockets)
{
	// Verify that sockets are destroyed when the node is destroyed.
	std::unique_ptr<NodeT> node = std::make_unique<NodeT>();
	auto* in  = node->AddInput("a");
	auto* out = node->AddOutput("b");

	node.reset(); // destroy node + its sockets

	// The raw pointers are now dangling — this test just ensures no
	// heap-use-after-free in the destructor path (valgrind/asan would catch).
	(void)in;
	(void)out;
	SUCCEED();
}

// ============================================================================
// Preset node → graph adoption
// ============================================================================

TEST(PresetTest, MoveNodeIntoGraph)
{
	auto preset = std::make_unique<NodeT>();
	preset->name = "Multiply";
	preset->data = "x * y";
	auto* inRaw  = preset->AddInput("lhs");
	auto* outRaw = preset->AddOutput("product");

	ASSERT_EQ(inRaw->owner, preset.get());
	ASSERT_EQ(outRaw->owner, preset.get());

	GraphT g;
	auto* adopted = g.AddNode(std::move(preset));

	EXPECT_EQ(adopted->name, "Multiply");
	EXPECT_EQ(adopted->data, "x * y");
	EXPECT_EQ(adopted->InputCount(), 1u);
	EXPECT_EQ(adopted->OutputCount(), 1u);

	// Owner pointers should still be valid (same heap address)
	EXPECT_EQ(inRaw->owner, adopted);
	EXPECT_EQ(outRaw->owner, adopted);
}

// ============================================================================
// Graph — connection tests
// ============================================================================

class GraphTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_a = m_graph.AddNode("A", "node A");
		m_b = m_graph.AddNode("B", "node B");
		m_c = m_graph.AddNode("C", "node C");

		m_aOut = m_a->AddOutput("a→");
		m_bIn  = m_b->AddInput("→b");
		m_bOut = m_b->AddOutput("b→");
		m_cIn  = m_c->AddInput("→c");
	}

	GraphT    m_graph;
	NodeT*    m_a = nullptr;
	NodeT*    m_b = nullptr;
	NodeT*    m_c = nullptr;
	SockOut*  m_aOut = nullptr;
	SockIn*   m_bIn  = nullptr;
	SockOut*  m_bOut = nullptr;
	SockIn*   m_cIn  = nullptr;
};

TEST_F(GraphTest, ConnectSingle)
{
	EXPECT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	EXPECT_EQ(m_bIn->source, m_aOut);
	EXPECT_EQ(m_aOut->targets.size(), 1u);
	EXPECT_EQ(m_aOut->targets[0], m_bIn);
}

TEST_F(GraphTest, ConnectSelfLoopRejected)
{
	auto* aIn = m_a->AddInput("self");
	EXPECT_FALSE(m_graph.Connect(m_aOut, aIn)); // same owner → self-loop
}

TEST_F(GraphTest, ConnectDuplicateRejected)
{
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	EXPECT_FALSE(m_graph.Connect(m_aOut, m_bIn)); // already connected
	EXPECT_EQ(m_aOut->targets.size(), 1u);         // no duplicate
}

TEST_F(GraphTest, ConnectDirectCycleRejected)
{
	// a → b exists; b → a would close a 2-cycle, so Connect must refuse it
	// and leave the sockets untouched.
	auto* aIn = m_a->AddInput("aIn");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));   // a → b
	EXPECT_FALSE(m_graph.Connect(m_bOut, aIn));     // b → a rejected
	EXPECT_EQ(aIn->source, nullptr);                // nothing wired
	EXPECT_EQ(m_bOut->targets.size(), 0u);
	EXPECT_FALSE(m_graph.HasCycle());               // graph stayed acyclic
}

TEST_F(GraphTest, ConnectTransitiveCycleRejected)
{
	// a → b → c; c → a would close a 3-cycle through the chain.
	auto* aIn  = m_a->AddInput("aIn");
	auto* cOut = m_c->AddOutput("c→");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));   // a → b
	ASSERT_TRUE(m_graph.Connect(m_bOut, m_cIn));   // b → c
	EXPECT_FALSE(m_graph.Connect(cOut, aIn));       // c → a rejected
	EXPECT_EQ(aIn->source, nullptr);
	EXPECT_FALSE(m_graph.HasCycle());
}

TEST_F(GraphTest, ConnectReverseEdgeAllowedAfterDisconnect)
{
	// Reversing an edge is legal once the forward edge is gone: a → b, then
	// disconnect and connect b → a.
	auto* aIn = m_a->AddInput("aIn");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));   // a → b
	EXPECT_FALSE(m_graph.Connect(m_bOut, aIn));     // b → a rejected while a→b lives
	m_graph.Disconnect(m_aOut, m_bIn);
	EXPECT_TRUE(m_graph.Connect(m_bOut, aIn));      // now legal
	EXPECT_FALSE(m_graph.HasCycle());
}

TEST_F(GraphTest, ConnectAutoBreakOldSource)
{
	// m_bIn gets connected to m_aOut
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));

	// Create another output and connect it to m_bIn — old connection should break
	auto* cOut = m_c->AddOutput("c→");
	EXPECT_TRUE(m_graph.Connect(cOut, m_bIn));

	EXPECT_EQ(m_bIn->source, cOut);
	EXPECT_EQ(m_aOut->targets.size(), 0u);  // old edge removed
}

TEST_F(GraphTest, Disconnect)
{
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	m_graph.Disconnect(m_aOut, m_bIn);

	EXPECT_EQ(m_bIn->source, nullptr);
	EXPECT_EQ(m_aOut->targets.size(), 0u);
}

TEST_F(GraphTest, FanOut)
{
	// aOut → bIn, aOut → cIn
	EXPECT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	EXPECT_TRUE(m_graph.Connect(m_aOut, m_cIn));

	EXPECT_EQ(m_aOut->FanOut(), 2u);
	EXPECT_NE(std::find(m_aOut->targets.begin(), m_aOut->targets.end(), m_bIn), m_aOut->targets.end());
	EXPECT_NE(std::find(m_aOut->targets.begin(), m_aOut->targets.end(), m_cIn), m_aOut->targets.end());
}

TEST_F(GraphTest, RemoveNodeBreaksConnections)
{
	// a → b → c
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(m_bOut, m_cIn));

	// Remove middle node b
	EXPECT_TRUE(m_graph.RemoveNode(m_b));
	EXPECT_EQ(m_graph.NodeCount(), 2u);

	// a's output should be disconnected from b (b is gone)
	EXPECT_EQ(m_aOut->targets.size(), 0u);

	// c's input should be disconnected
	EXPECT_EQ(m_cIn->source, nullptr);
}

// ============================================================================
// Topological sort
// ============================================================================

TEST_F(GraphTest, TopologicalSortLinearChain)
{
	// a → b → c
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(m_bOut, m_cIn));

	auto sorted = m_graph.TopologicalSort();
	ASSERT_EQ(sorted.size(), 3u);
	EXPECT_EQ(sorted[0], m_a);
	EXPECT_EQ(sorted[1], m_b);
	EXPECT_EQ(sorted[2], m_c);
}

TEST_F(GraphTest, TopologicalSortDiamond)
{
	// a → b, a → c, b → d, c → d
	auto* d     = m_graph.AddNode("D", "node D");
	auto* bOut  = m_b->AddOutput("b→");
	auto* cOut  = m_c->AddOutput("c→");
	auto* dIn1  = d->AddInput("→d1");
	auto* dIn2  = d->AddInput("→d2");

	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));   // a → b
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_cIn));   // a → c  (fan-out)
	ASSERT_TRUE(m_graph.Connect(bOut, dIn1));       // b → d
	ASSERT_TRUE(m_graph.Connect(cOut, dIn2));       // c → d

	auto sorted = m_graph.TopologicalSort();
	ASSERT_EQ(sorted.size(), 4u);

	// a must come first (zero in-degree)
	EXPECT_EQ(sorted[0], m_a);

	// b and c can come in either order after a, but both before d
	auto posA = std::find(sorted.begin(), sorted.end(), m_a);
	auto posB = std::find(sorted.begin(), sorted.end(), m_b);
	auto posC = std::find(sorted.begin(), sorted.end(), m_c);
	auto posD = std::find(sorted.begin(), sorted.end(), d);

	EXPECT_LT(posA, posB);
	EXPECT_LT(posA, posC);
	EXPECT_LT(posB, posD);
	EXPECT_LT(posC, posD);
	EXPECT_EQ(sorted.back(), d);
}

TEST_F(GraphTest, TopologicalSortDisconnectedNodesAnyOrder)
{
	// Three nodes with no connections — all have in-degree 0.
	// Any order is valid.
	auto sorted = m_graph.TopologicalSort();
	ASSERT_EQ(sorted.size(), 3u);
	// All three must be present
	for (auto* n : {m_a, m_b, m_c})
		EXPECT_NE(std::find(sorted.begin(), sorted.end(), n), sorted.end());
}

TEST_F(GraphTest, TopologicalSortSingleNode)
{
	GraphT g;
	auto* single = g.AddNode("alone");
	auto sorted  = g.TopologicalSort();
	ASSERT_EQ(sorted.size(), 1u);
	EXPECT_EQ(sorted[0], single);
}

TEST_F(GraphTest, TopologicalSortEmptyGraph)
{
	GraphT g;
	auto sorted = g.TopologicalSort();
	EXPECT_TRUE(sorted.empty());
}

// ============================================================================
// Cycle detection
// ============================================================================

TEST_F(GraphTest, HasCycleDirectCycle)
{
	// a → b → a. Connect() now rejects the closing edge, so force it raw to
	// exercise the traversal-time cycle guard behind Connect.
	auto* aIn = m_a->AddInput("aIn");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));   // a → b
	EXPECT_FALSE(m_graph.Connect(m_bOut, aIn));     // b → a rejected (would cycle)
	ForceEdge(m_bOut, aIn);                          // force the cycle anyway

	EXPECT_TRUE(m_graph.HasCycle());
	EXPECT_THROW(m_graph.TopologicalSort(), std::runtime_error);
}

TEST_F(GraphTest, HasCycleSelfLoopRejectedAtConnect)
{
	// Self-loop blocked by Connect(), so no cycle should exist
	auto* aIn = m_a->AddInput("aIn");
	EXPECT_FALSE(m_graph.Connect(m_aOut, aIn));
	EXPECT_FALSE(m_graph.HasCycle());
}

TEST_F(GraphTest, HasCycleNoCycleDiamond)
{
	auto* d     = m_graph.AddNode("D");
	auto* bOut2 = m_b->AddOutput("b→");
	auto* cOut  = m_c->AddOutput("c→");
	auto* dIn1  = d->AddInput("→d1");
	auto* dIn2  = d->AddInput("→d2");

	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_cIn));
	ASSERT_TRUE(m_graph.Connect(bOut2, dIn1));
	ASSERT_TRUE(m_graph.Connect(cOut, dIn2));

	EXPECT_FALSE(m_graph.HasCycle());
}

TEST_F(GraphTest, HasCycleThreeNodeCycle)
{
	// a → b → c → a.
	auto* aIn = m_a->AddInput("aIn");
	auto* cOut = m_c->AddOutput("c→");  // was m_bOut

	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));   // a → b
	ASSERT_TRUE(m_graph.Connect(m_bOut, m_cIn));   // b → c
	EXPECT_FALSE(m_graph.Connect(cOut, aIn));       // c → a rejected (would cycle)
	ForceEdge(cOut, aIn);                            // force the cycle anyway

	EXPECT_TRUE(m_graph.HasCycle());
}

// ============================================================================
// SequentialTraverse — serial dependency-ordered visitor
// ============================================================================

TEST_F(GraphTest, SequentialTraverseVisitsEachOnceInDependencyOrder)
{
	// a → b, a → c, b → d, c → d: every producer must precede its consumer,
	// and each node is visited exactly once.
	auto* d    = m_graph.AddNode("D", "node D");
	auto* bOut = m_b->AddOutput("b→");
	auto* cOut = m_c->AddOutput("c→");
	auto* dIn1 = d->AddInput("→d1");
	auto* dIn2 = d->AddInput("→d2");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_cIn));
	ASSERT_TRUE(m_graph.Connect(bOut, dIn1));
	ASSERT_TRUE(m_graph.Connect(cOut, dIn2));

	std::vector<NodeT*>             order;
	std::unordered_map<NodeT*, int> visitCount;
	m_graph.SequentialTraverse([&](NodeT* n)
	{
		order.push_back(n);
		++visitCount[n];
	});

	ASSERT_EQ(order.size(), 4u);
	for (auto* n : { m_a, m_b, m_c, d })
		EXPECT_EQ(visitCount[n], 1) << "node " << n->name;

	auto pos = [&](NodeT* n) {
		return std::find(order.begin(), order.end(), n) - order.begin();
	};
	EXPECT_LT(pos(m_a), pos(m_b));
	EXPECT_LT(pos(m_a), pos(m_c));
	EXPECT_LT(pos(m_b), pos(d));
	EXPECT_LT(pos(m_c), pos(d));
}

TEST_F(GraphTest, SequentialTraverseEmptyGraph)
{
	GraphT g;
	int visits = 0;
	g.SequentialTraverse([&](NodeT*) { ++visits; });
	EXPECT_EQ(visits, 0);
}

TEST_F(GraphTest, SequentialTraverseCycleThrows)
{
	// a → b → a: Connect rejects the closing edge, so force it raw. The walk
	// must throw up front rather than visit a partial order.
	auto* aIn = m_a->AddInput("aIn");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	EXPECT_FALSE(m_graph.Connect(m_bOut, aIn));
	ForceEdge(m_bOut, aIn);

	int visits = 0;
	EXPECT_THROW(
		m_graph.SequentialTraverse([&](NodeT*) { ++visits; }),
		std::runtime_error);
	EXPECT_EQ(visits, 0) << "cyclic graph must visit nothing";
}

TEST_F(GraphTest, SequentialTraversePropagatesVisitException)
{
	// A → B → C: B throws, so C must never run and the error propagates.
	auto* bOut = m_b->AddOutput("b→");
	auto* cIn2 = m_c->AddInput("→c2");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(bOut, cIn2));

	std::unordered_set<NodeT*> done;
	EXPECT_THROW(
		m_graph.SequentialTraverse([&](NodeT* node)
		{
			if (node == m_b)
				throw std::runtime_error("boom");
			done.insert(node);
		}),
		std::runtime_error);

	EXPECT_EQ(done.count(m_a), 1u);  // ran before the failure
	EXPECT_EQ(done.count(m_c), 0u);  // downstream of the failed node → skipped
}

// ============================================================================
// ParallelTraverse — completion-driven dependency executor
// ============================================================================

namespace {

// Minimal Executor: runs each submitted task synchronously on the caller's
// thread. Exercises ParallelTraverse's dependency logic deterministically,
// with no threading in play.
struct InlineExecutor
{
	void submit(const std::function<void()>& task) { task(); }
};

// Direct producers of `node` = owners of each connected input socket's source.
std::vector<NodeT*> ProducersOf(NodeT* node)
{
	std::vector<NodeT*> producers;
	for (const auto& in : node->inputs)
		if (in->source && in->source->owner)
			producers.push_back(in->source->owner);
	return producers;
}

} // anonymous namespace

TEST_F(GraphTest, ParallelTraverseInlineVisitsEachNodeOnceAfterProducers)
{
	// Diamond A → {B, C} → D on the synchronous executor.
	auto* d    = m_graph.AddNode("D", "node D");
	auto* bOut = m_b->AddOutput("b→");
	auto* cOut = m_c->AddOutput("c→");
	auto* dIn1 = d->AddInput("→d1");
	auto* dIn2 = d->AddInput("→d2");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_cIn));
	ASSERT_TRUE(m_graph.Connect(bOut, dIn1));
	ASSERT_TRUE(m_graph.Connect(cOut, dIn2));

	std::unordered_map<NodeT*, int>   visitCount;
	std::unordered_set<NodeT*>        done;

	InlineExecutor exec;
	m_graph.ParallelTraverse([&](NodeT* node)
	{
		// Every direct producer must have finished before we are visited.
		for (NodeT* p : ProducersOf(node))
			EXPECT_TRUE(done.count(p) == 1) << node->name << " ran before " << p->name;
		++visitCount[node];
		done.insert(node);
	}, exec);

	// Exactly once per node, all four reached.
	EXPECT_EQ(visitCount.size(), 4u);
	for (auto* n : { m_a, m_b, m_c, d })
		EXPECT_EQ(visitCount[n], 1) << "node " << n->name;
}

TEST_F(GraphTest, ParallelTraverseInlineEmptyGraphIsNoOp)
{
	GraphT g;
	InlineExecutor exec;
	int visits = 0;
	g.ParallelTraverse([&](NodeT*) { ++visits; }, exec);
	EXPECT_EQ(visits, 0);
}

TEST_F(GraphTest, ParallelTraverseInlineSingleNode)
{
	GraphT g;
	auto* only = g.AddNode("only", "x");
	InlineExecutor exec;
	int visits = 0;
	g.ParallelTraverse([&](NodeT* n) { EXPECT_EQ(n, only); ++visits; }, exec);
	EXPECT_EQ(visits, 1);
}

TEST_F(GraphTest, ParallelTraverseCycleThrowsBeforeDispatch)
{
	// a → b → a: never drains, so it must be rejected up front (no hang).
	// Connect() rejects the closing edge, so force it raw.
	auto* aIn = m_a->AddInput("aIn");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	EXPECT_FALSE(m_graph.Connect(m_bOut, aIn));
	ForceEdge(m_bOut, aIn);

	InlineExecutor exec;
	int visits = 0;
	EXPECT_THROW(
		m_graph.ParallelTraverse([&](NodeT*) { ++visits; }, exec),
		std::runtime_error);
	EXPECT_EQ(visits, 0) << "cyclic graph must dispatch nothing";
}

TEST_F(GraphTest, ParallelTraverseRethrowsFirstVisitException)
{
	// A → B → C: B throws, so C must never run and the error propagates.
	auto* bOut = m_b->AddOutput("b→");
	auto* cIn2 = m_c->AddInput("→c2");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(bOut, cIn2));

	std::unordered_set<NodeT*> done;
	std::mutex                 mtx;

	InlineExecutor exec;
	EXPECT_THROW(
		m_graph.ParallelTraverse([&](NodeT* node)
		{
			if (node == m_b)
				throw std::runtime_error("boom");
			std::lock_guard<std::mutex> lk(mtx);
			done.insert(node);
		}, exec),
		std::runtime_error);

	EXPECT_EQ(done.count(m_a), 1u);  // ran before the failure
	EXPECT_EQ(done.count(m_c), 0u);  // downstream of the failed node → skipped
}

TEST_F(GraphTest, ParallelTraversePoolHonoursDependenciesUnderConcurrency)
{
	// Wide diamond: A → {B, C, E} → D, run many times on a real pool. Each
	// visit asserts its producers already finished; every node runs exactly
	// once. Repeating hammers the readiness bookkeeping for races.
	auto* e    = m_graph.AddNode("E", "node E");
	auto* d    = m_graph.AddNode("D", "node D");
	auto* bOut = m_b->AddOutput("b→");
	auto* cOut = m_c->AddOutput("c→");
	auto* eIn  = e->AddInput("→e");
	auto* eOut = e->AddOutput("e→");
	auto* dIn1 = d->AddInput("→d1");
	auto* dIn2 = d->AddInput("→d2");
	auto* dIn3 = d->AddInput("→d3");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_cIn));
	ASSERT_TRUE(m_graph.Connect(m_aOut, eIn));
	ASSERT_TRUE(m_graph.Connect(bOut, dIn1));
	ASSERT_TRUE(m_graph.Connect(cOut, dIn2));
	ASSERT_TRUE(m_graph.Connect(eOut, dIn3));

	ThreadPool pool(4);

	for (int iter = 0; iter < 50; ++iter)
	{
		std::mutex                      mtx;
		std::unordered_set<NodeT*>      done;
		std::unordered_map<NodeT*, int> visitCount;

		m_graph.ParallelTraverse([&](NodeT* node)
		{
			auto producers = ProducersOf(node);
			{
				std::lock_guard<std::mutex> lk(mtx);
				for (NodeT* p : producers)
					EXPECT_EQ(done.count(p), 1u)
						<< node->name << " ran before producer " << p->name;
			}
			// A little work so overlapping visits actually interleave.
			std::this_thread::sleep_for(std::chrono::microseconds(50));
			{
				std::lock_guard<std::mutex> lk(mtx);
				++visitCount[node];
				done.insert(node);
			}
		}, pool);

		ASSERT_EQ(visitCount.size(), 5u) << "iter " << iter;
		for (auto* n : { m_a, m_b, m_c, e, d })
			EXPECT_EQ(visitCount[n], 1) << "node " << n->name << " iter " << iter;
	}
}

TEST_F(GraphTest, ParallelTraversePoolRunsIndependentNodesConcurrently)
{
	// Three independent sources (no edges) on a 3-worker pool should overlap:
	// peak observed concurrency must exceed 1. Each visit holds briefly so the
	// workers genuinely coincide.
	std::atomic<int> inside{ 0 };
	std::atomic<int> peak{ 0 };

	ThreadPool pool(3);
	m_graph.ParallelTraverse([&](NodeT*)
	{
		int cur = ++inside;
		int prev = peak.load();
		while (cur > prev && !peak.compare_exchange_weak(prev, cur)) {}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		--inside;
	}, pool);

	EXPECT_GT(peak.load(), 1) << "independent nodes should run in parallel";
}

TEST_F(GraphTest, ParallelTraversePoolRethrowsAndTerminates)
{
	// Pool path must also capture a visit exception, stop feeding the
	// frontier, drain in-flight work, and rethrow — without hanging.
	auto* bOut = m_b->AddOutput("b→");
	auto* cIn2 = m_c->AddInput("→c2");
	ASSERT_TRUE(m_graph.Connect(m_aOut, m_bIn));
	ASSERT_TRUE(m_graph.Connect(bOut, cIn2));

	std::atomic<bool> sawC{ false };
	ThreadPool pool(2);
	EXPECT_THROW(
		m_graph.ParallelTraverse([&](NodeT* node)
		{
			if (node == m_b)
				throw std::runtime_error("boom");
			if (node == m_c)
				sawC = true;
		}, pool),
		std::runtime_error);

	EXPECT_FALSE(sawC.load()) << "node downstream of failure must be skipped";
}
