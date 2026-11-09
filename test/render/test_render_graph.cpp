/**
 * @file test_render_graph.cpp
 * @brief Wave 1 unit tests for the RenderGraph scaffolding.
 *
 * Scope: pure topology / declaration validation. No Vulkan calls: MockPass
 * inherits `Pass` and overrides `GetIO()` to describe its image I/O; its
 * Record() is a no-op. Barrier and descriptor auto-injection are Wave 2+
 * concerns and covered by GPU tests there.
 */

#include <gtest/gtest.h>

#include "render/render_graph/RenderGraph.h"
#include "render/passes/Pass.h"
#include "render/RenderCache.h"
#include "shared/TestVulkanShared.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace neurus;

namespace {

// ---------------------------------------------------------------------------
// MockPass — CPU-only stub satisfying Pass and returning a hand-built PassIO.
// ---------------------------------------------------------------------------
class MockPass : public Pass
{
public:
	MockPass(std::string name,
	         std::vector<AttachmentName> reads,
	         std::vector<AttachmentName> writes)
	    : Pass()
	{
		m_io.name = std::move(name);
		uint32_t binding = 0;
		for (auto r : reads)  m_io.reads .push_back({r, binding++});
		binding = 0;
		for (auto w : writes) m_io.writes.push_back({w, binding++});
	}

	// --- Pass ---
	PassStats Record(vk::CommandBuffer, RenderCache&, const RenderContext&) override { return {}; }
	PassIO GetIO() const override { return m_io; }

private:
	PassIO m_io;
};

// ---------------------------------------------------------------------------
// CounterPass - mirrors real passes: it reports draw/dispatch counts by
// returning a PassStats from Record() (passes stay stateless).
// ---------------------------------------------------------------------------
class CounterPass : public Pass
{
public:
	CounterPass(std::string name,
	            std::vector<AttachmentName> reads,
	            std::vector<AttachmentName> writes)
	    : Pass()
	{
		m_io.name = std::move(name);
		uint32_t binding = 0;
		for (auto r : reads)  m_io.reads .push_back({r, binding++});
		binding = 0;
		for (auto w : writes) m_io.writes.push_back({w, binding++});
	}

	// --- Pass ---
	PassStats Record(vk::CommandBuffer, RenderCache&, const RenderContext&) override
	{
		// Simulate a pass issuing two draws + one dispatch.
		return {/*drawCalls=*/2, /*dispatches=*/1};
	}
	PassIO GetIO() const override { return m_io; }

private:
	PassIO m_io;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// AddPass — materializes one socket per declared resource.
// ---------------------------------------------------------------------------

TEST(RenderGraphTest, AddPassDeclaresSockets)
{
	MockPass p("mock",
	           /*reads=*/  {AttachmentName::Position, AttachmentName::Normal},
	           /*writes=*/ {AttachmentName::HDRColor});

	RenderGraph g;
	auto* node = g.AddPass(&p);

	ASSERT_NE(node, nullptr);
	EXPECT_EQ(node->InputCount(),  2u);
	EXPECT_EQ(node->OutputCount(), 1u);
	EXPECT_EQ(node->data.pass, static_cast<Pass*>(&p));
}

TEST(RenderGraphTest, AddPassRejectsNullptr)
{
	RenderGraph g;
	EXPECT_THROW(g.AddPass(nullptr), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Connect — wires a producer's write into a consumer's read.
// ---------------------------------------------------------------------------

TEST(RenderGraphTest, ConnectWiresMatchingResource)
{
	MockPass producer("prod", {}, {AttachmentName::Position});
	MockPass consumer("cons", {AttachmentName::Position}, {});

	RenderGraph g;
	auto* pn = g.AddPass(&producer);
	auto* cn = g.AddPass(&consumer);

	EXPECT_TRUE(g.Connect(pn, AttachmentName::Position, cn));

	// Duplicate connection is rejected by the underlying Graph<>.
	EXPECT_FALSE(g.Connect(pn, AttachmentName::Position, cn));
}

TEST(RenderGraphTest, ConnectRejectsMissingSocket)
{
	MockPass producer("prod", {}, {AttachmentName::Position});
	MockPass consumer("cons", {AttachmentName::Normal}, {});

	RenderGraph g;
	auto* pn = g.AddPass(&producer);
	auto* cn = g.AddPass(&consumer);

	// Producer never declared Normal; consumer never declared Position.
	// Either direction fails without side effects.
	EXPECT_FALSE(g.Connect(pn, AttachmentName::Normal,   cn));
	EXPECT_FALSE(g.Connect(pn, AttachmentName::Position, cn));
}

// ---------------------------------------------------------------------------
// Compile — validates connectivity and produces a topological order.
// ---------------------------------------------------------------------------

TEST(RenderGraphTest, CompileLinearChain)
{
	// A(writes X) → B(reads X, writes Y) → C(reads Y)
	MockPass a("A", {}, {AttachmentName::Position});
	MockPass b("B", {AttachmentName::Position}, {AttachmentName::HDRColor});
	MockPass c("C", {AttachmentName::HDRColor}, {});

	RenderGraph g;
	auto* na = g.AddPass(&a);
	auto* nb = g.AddPass(&b);
	auto* nc = g.AddPass(&c);

	ASSERT_TRUE(g.Connect(na, AttachmentName::Position, nb));
	ASSERT_TRUE(g.Connect(nb, AttachmentName::HDRColor, nc));

	g.Compile();

	const auto& order = g.CompiledOrder();
	ASSERT_EQ(order.size(), 3u);
	EXPECT_EQ(order[0], na);
	EXPECT_EQ(order[1], nb);
	EXPECT_EQ(order[2], nc);
}

TEST(RenderGraphTest, CompileAllowsExternalInputs)
{
	// An input with no in-graph producer is "external" (produced by a
	// RenderCache attachment or a legacy pass during incremental migration).
	// Compile() must accept it rather than treating it as an error.
	MockPass consumer("cons", {AttachmentName::Position}, {});

	RenderGraph g;
	auto* cn = g.AddPass(&consumer);

	g.Compile();

	const auto& order = g.CompiledOrder();
	ASSERT_EQ(order.size(), 1u);
	EXPECT_EQ(order[0], cn);
}

TEST(RenderGraphTest, ConnectRejectsCycle)
{
	// Build a would-be 2-node cycle: A reads X and writes Y; B reads Y and
	// writes X. Connect now rejects the edge that would close the cycle, so
	// the graph stays acyclic and Compile() succeeds.
	MockPass a("A", {AttachmentName::Position}, {AttachmentName::HDRColor});
	MockPass b("B", {AttachmentName::HDRColor}, {AttachmentName::Position});

	RenderGraph g;
	auto* na = g.AddPass(&a);
	auto* nb = g.AddPass(&b);

	ASSERT_TRUE(g.Connect(na, AttachmentName::HDRColor, nb));   // A → B
	EXPECT_FALSE(g.Connect(nb, AttachmentName::Position, na));  // B → A rejected

	g.Compile();  // stays acyclic → no throw
	EXPECT_EQ(g.CompiledOrder().size(), 2u);
}

// ---------------------------------------------------------------------------
// Execute preconditions.
// ---------------------------------------------------------------------------

TEST(RenderGraphTest, ExecuteThrowsIfNotCompiled)
{
	RenderGraph g;
	MockPass p("solo", {}, {});
	g.AddPass(&p);

	// Passing a null command buffer is fine here because Execute() throws
	// before any pass is invoked. This is a precondition-check test.
	RenderCache*         nullCache = nullptr;
	const RenderContext* nullCtx   = nullptr;
	GPUProfiler          profiler;      // default (uninitialized) — unused before throw
	FrameProfile         frameProfile;
	EXPECT_THROW(g.Execute(vk::CommandBuffer{}, *nullCache, *nullCtx, profiler, frameProfile),
	             std::runtime_error);
}

TEST(RenderGraphTest, ClearRemovesAllNodes)
{
	// Rebuild-on-toggle relies on Clear() emptying the graph so it can be
	// repopulated with the currently-active pass set.
	MockPass a("A", {}, {AttachmentName::Position});
	MockPass b("B", {AttachmentName::Position}, {});

	RenderGraph g;
	auto* na = g.AddPass(&a);
	auto* nb = g.AddPass(&b);
	g.Connect(na, AttachmentName::Position, nb);
	g.Compile();
	ASSERT_EQ(g.PassCount(), 2u);

	g.Clear();
	EXPECT_EQ(g.PassCount(), 0u);
	EXPECT_TRUE(g.CompiledOrder().empty());

	// Graph is reusable after Clear().
	g.AddPass(&a);
	g.Compile();
	EXPECT_EQ(g.PassCount(), 1u);
}

// ---------------------------------------------------------------------------
// Reset — invalidates the compiled order without dropping nodes.
// ---------------------------------------------------------------------------

TEST(RenderGraphTest, ResetInvalidatesCompiledOrder)
{
	MockPass a("A", {}, {});
	RenderGraph g;
	g.AddPass(&a);
	g.Compile();

	EXPECT_EQ(g.CompiledOrder().size(), 1u);

	g.Reset();
	EXPECT_TRUE(g.CompiledOrder().empty());
}

// ---------------------------------------------------------------------------
// Topology shape coverage: fan-out, diamond, multi-resource edges.
// ---------------------------------------------------------------------------

namespace {
// Position of a node in the compiled order (or SIZE_MAX if absent).
size_t OrderIndex(const RenderGraph& g, const RenderGraph::NodeT* node)
{
	const auto& order = g.CompiledOrder();
	for (size_t i = 0; i < order.size(); ++i)
		if (order[i] == node) return i;
	return SIZE_MAX;
}
} // anonymous namespace

TEST(RenderGraphTest, FanOutOneProducerManyConsumers)
{
	// prod writes X; consA and consB both read X.
	MockPass prod ("prod",  {},                        {AttachmentName::Position});
	MockPass consA("consA", {AttachmentName::Position}, {});
	MockPass consB("consB", {AttachmentName::Position}, {});

	RenderGraph g;
	auto* np = g.AddPass(&prod);
	auto* na = g.AddPass(&consA);
	auto* nb = g.AddPass(&consB);

	ASSERT_TRUE(g.Connect(np, AttachmentName::Position, na));
	ASSERT_TRUE(g.Connect(np, AttachmentName::Position, nb));

	g.Compile();

	// Producer must precede both consumers.
	EXPECT_LT(OrderIndex(g, np), OrderIndex(g, na));
	EXPECT_LT(OrderIndex(g, np), OrderIndex(g, nb));
	EXPECT_EQ(g.CompiledOrder().size(), 3u);
}

TEST(RenderGraphTest, DiamondDependency)
{
	// A → B, A → C, B → D, C → D.
	MockPass a("A", {},                          {AttachmentName::Position});
	MockPass b("B", {AttachmentName::Position},  {AttachmentName::Normal});
	MockPass c("C", {AttachmentName::Position},  {AttachmentName::Albedo});
	MockPass d("D", {AttachmentName::Normal, AttachmentName::Albedo}, {});

	RenderGraph g;
	auto* na = g.AddPass(&a);
	auto* nb = g.AddPass(&b);
	auto* nc = g.AddPass(&c);
	auto* nd = g.AddPass(&d);

	ASSERT_TRUE(g.Connect(na, AttachmentName::Position, nb));
	ASSERT_TRUE(g.Connect(na, AttachmentName::Position, nc));
	ASSERT_TRUE(g.Connect(nb, AttachmentName::Normal,   nd));
	ASSERT_TRUE(g.Connect(nc, AttachmentName::Albedo,   nd));

	g.Compile();

	const size_t ia = OrderIndex(g, na), ib = OrderIndex(g, nb),
	             ic = OrderIndex(g, nc), id = OrderIndex(g, nd);
	// A before B and C; B and C before D.
	EXPECT_LT(ia, ib);
	EXPECT_LT(ia, ic);
	EXPECT_LT(ib, id);
	EXPECT_LT(ic, id);
}

TEST(RenderGraphTest, MultipleResourceEdgesBetweenSamePair)
{
	// A writes X and Y; B reads both. Two distinct edges, one per resource.
	MockPass a("A", {}, {AttachmentName::Position, AttachmentName::Normal});
	MockPass b("B", {AttachmentName::Position, AttachmentName::Normal}, {});

	RenderGraph g;
	auto* na = g.AddPass(&a);
	auto* nb = g.AddPass(&b);

	ASSERT_TRUE(g.Connect(na, AttachmentName::Position, nb));
	ASSERT_TRUE(g.Connect(na, AttachmentName::Normal,   nb));
	// Re-connecting an existing edge is rejected.
	EXPECT_FALSE(g.Connect(na, AttachmentName::Position, nb));

	g.Compile();
	EXPECT_LT(OrderIndex(g, na), OrderIndex(g, nb));
}

// ---------------------------------------------------------------------------
// Rebuild determinism + cycle diagnostics.
// ---------------------------------------------------------------------------

TEST(RenderGraphTest, RebuildProducesSameOrder)
{
	MockPass a("A", {},                         {AttachmentName::Position});
	MockPass b("B", {AttachmentName::Position}, {AttachmentName::HDRColor});
	MockPass c("C", {AttachmentName::HDRColor}, {});

	auto build = [&](RenderGraph& g) {
		auto* na = g.AddPass(&a);
		auto* nb = g.AddPass(&b);
		auto* nc = g.AddPass(&c);
		g.Connect(na, AttachmentName::Position, nb);
		g.Connect(nb, AttachmentName::HDRColor, nc);
		g.Compile();
	};

	RenderGraph g;
	build(g);
	std::vector<std::string> first;
	for (auto* n : g.CompiledOrder()) first.push_back(n->name);

	g.Clear();
	build(g);
	std::vector<std::string> second;
	for (auto* n : g.CompiledOrder()) second.push_back(n->name);

	EXPECT_EQ(first, second);
	ASSERT_EQ(first.size(), 3u);
	EXPECT_EQ(first[0], "A");
	EXPECT_EQ(first[2], "C");
}

TEST(RenderGraphTest, CycleErrorNamesInvolvedNodes)
{
	// Connect keeps the graph acyclic, so to exercise Compile's own cycle
	// guard we force the closing edge raw (bypassing Connect). The thrown
	// message should still name the cyclic passes so a misconfiguration is
	// debuggable without bisecting.
	MockPass a("AlphaPass", {AttachmentName::Position}, {AttachmentName::HDRColor});
	MockPass b("BetaPass",  {AttachmentName::HDRColor}, {AttachmentName::Position});

	RenderGraph g;
	auto* na = g.AddPass(&a);
	auto* nb = g.AddPass(&b);
	ASSERT_TRUE(g.Connect(na, AttachmentName::HDRColor, nb));   // A → B

	// Force B → A raw: each pass here has exactly one input/output socket.
	ASSERT_EQ(na->inputs.size(), 1u);
	ASSERT_EQ(nb->outputs.size(), 1u);
	nb->outputs[0]->targets.push_back(na->inputs[0].get());
	na->inputs[0]->source = nb->outputs[0].get();

	try
	{
		g.Compile();
		FAIL() << "expected cycle to throw";
	}
	catch (const std::runtime_error& e)
	{
		const std::string msg = e.what();
		EXPECT_NE(msg.find("AlphaPass"), std::string::npos) << msg;
		EXPECT_NE(msg.find("BetaPass"),  std::string::npos) << msg;
	}
}

// ---------------------------------------------------------------------------
// Pass profiling counters - passes report draw/dispatch counts by returning a
// PassStats from Record() so RenderContext stays an immutable snapshot; the
// RenderGraph aggregates them per pass while profiling (see RenderGraph::Execute).
// ---------------------------------------------------------------------------

class PassCountersTest : public VulkanTestShared
{
protected:
	// VulkanTestShared provides SetUp/TearDown (device, command pool).
};

TEST_F(PassCountersTest, RecordReturnsStats)
{
	if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan-capable GPU found.";

	CounterPass pass("A", {}, {AttachmentName::Position});

	RenderCache cache(*m_device, PhysicalDevice());
	RenderContext ctx;
	ctx.frameIndex = 0;

	// CounterPass reports two draws + one dispatch per Record().
	const PassStats stats = pass.Record(vk::CommandBuffer{}, cache, ctx);
	EXPECT_EQ(stats.drawCalls, 2u);
	EXPECT_EQ(stats.dispatches, 1u);

	// Record() is stateless: a second call returns the same stats, not an
	// accumulation.
	const PassStats stats2 = pass.Record(vk::CommandBuffer{}, cache, ctx);
	EXPECT_EQ(stats2.drawCalls, 2u);
	EXPECT_EQ(stats2.dispatches, 1u);
}
