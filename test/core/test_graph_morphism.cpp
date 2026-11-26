/**
 * @file test_graph_morphism.cpp
 * @brief Node and connection mappings between generic graphs.
 */

#include <gtest/gtest.h>

#include "core/GraphMorphism.hpp"

#include <optional>
#include <string>

namespace {

using GraphI = neurus::Graph<int, int>;
using Morphism = neurus::GraphMorphism<GraphI, GraphI>;

GraphI::NodeType* FindNode(GraphI& graph, const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

GraphI::SocketOutT* FindOutput(GraphI::NodeType* node, const std::string& name)
{
	for (const auto& output : node->outputs)
	{
		if (output->name == name)
			return output.get();
	}
	return nullptr;
}

GraphI::SocketInT* FindInput(GraphI::NodeType* node, const std::string& name)
{
	for (const auto& input : node->inputs)
	{
		if (input->name == name)
			return input.get();
	}
	return nullptr;
}

GraphI MakeChain()
{
	GraphI graph;
	auto* a = graph.AddNode("A", 1);
	auto* b = graph.AddNode("B", 2);
	auto* c = graph.AddNode("C", 3);

	for (GraphI::NodeType* node : {a, b, c})
	{
		node->AddInput("in");
		node->AddOutput("out");
	}

	EXPECT_TRUE(graph.Connect(FindOutput(a, "out"), FindInput(b, "in")));
	EXPECT_TRUE(graph.Connect(FindOutput(b, "out"), FindInput(c, "in")));
	return graph;
}

GraphI MakeFanOut()
{
	GraphI graph;
	auto* a = graph.AddNode("A", 1);
	auto* b = graph.AddNode("B", 2);
	auto* c = graph.AddNode("C", 3);

	for (GraphI::NodeType* node : {a, b, c})
	{
		node->AddInput("in");
		node->AddOutput("out");
	}

	EXPECT_TRUE(graph.Connect(FindOutput(a, "out"), FindInput(b, "in")));
	EXPECT_TRUE(graph.Connect(FindOutput(a, "out"), FindInput(c, "in")));
	return graph;
}

Morphism::Policy CopyPolicy()
{
	Morphism::Policy policy;
	policy.createNode = [](const GraphI::NodeType& node) {
		return node.data;
	};
	policy.createOutput = [](const GraphI::SocketOutT& port) {
		return port.data;
	};
	policy.createInput = [](const GraphI::SocketInT& port) {
		return port.data;
	};
	return policy;
}

} // anonymous namespace

TEST(GraphMorphism, IdentityIsTotalAndPreservesIncidence)
{
	GraphI graph = MakeChain();
	const auto images = neurus::IdentityMorphism(graph);

	EXPECT_TRUE(images.IsTotal(graph));
	EXPECT_TRUE(images.PreservesIncidence());
	EXPECT_TRUE(images.Validate(graph).Ok());
	EXPECT_EQ(images.MappedNodeCount(), 3u);
	EXPECT_EQ(images.MappedConnectionCount(), 2u);
	for (const auto* edge : graph.Connections())
		EXPECT_EQ(images.ImageOf(edge), edge);
}

TEST(GraphMorphism, MapBuildsTheTarget)
{
	GraphI source = MakeChain();
	const Morphism mapper{CopyPolicy()};

	auto mapping = mapper.Map(source);

	EXPECT_TRUE(mapping.images.IsTotal(source));
	EXPECT_TRUE(mapping.images.PreservesIncidence());
	EXPECT_TRUE(mapping.images.Validate(source).Ok());
	EXPECT_EQ(mapping.images.MappedNodeCount(), 3u);
	EXPECT_EQ(mapping.images.MappedConnectionCount(), 2u);

	ASSERT_EQ(mapping.target.nodes.size(), source.nodes.size());
	EXPECT_EQ(mapping.images.ImageOf(FindNode(source, "B"))->name, "B");
	EXPECT_EQ(FindNode(mapping.target, "B")->data, 2);
}

TEST(GraphMorphism, MapReportsDeclinedNode)
{
	const GraphI source = MakeChain();
	auto policy = CopyPolicy();
	policy.createNode = [](const GraphI::NodeType& node) -> std::optional<int> {
		if (node.name == "C")
			return std::nullopt;
		return node.data;
	};

	const Morphism mapper(std::move(policy));
	const auto mapping = mapper.Map(source);

	EXPECT_FALSE(mapping.images.IsTotal(source));
	const auto report = mapping.images.Validate(source);
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("C"), std::string::npos);
}

TEST(GraphMorphism, MapsEverySocketOnceAndUsesMappedValues)
{
	auto source = MakeFanOut();
	FindOutput(FindNode(source, "A"), "out")->data = 20;
	int inputsMapped = 0;
	int outputsMapped = 0;
	auto policy = CopyPolicy();
	policy.createOutput = [&](const GraphI::SocketOutT& socket) {
		++outputsMapped;
		return socket.data + 10;
	};
	policy.createInput = [&](const GraphI::SocketInT& socket) {
		++inputsMapped;
		return socket.data + 10;
	};

	const auto mapping = Morphism(std::move(policy)).Map(source);

	EXPECT_TRUE(mapping.images.IsTotal(source));
	EXPECT_EQ(inputsMapped, 3);
	EXPECT_EQ(outputsMapped, 3);
	const auto* original = FindOutput(FindNode(source, "A"), "out");
	const auto* mapped = mapping.images.ImageOf(original);
	ASSERT_NE(mapped, nullptr);
	EXPECT_EQ(mapped->name, "out");
	EXPECT_EQ(mapped->data, 30);
	EXPECT_EQ(mapped->FanOut(), 2u);
	EXPECT_EQ(original->name, "out");
	EXPECT_EQ(original->data, 20);
	const auto* unconnected = mapping.images.ImageOf(FindInput(FindNode(source, "A"), "in"));
	ASSERT_NE(unconnected, nullptr);
	EXPECT_EQ(unconnected->name, "in");
	EXPECT_FALSE(unconnected->IsConnected());
}

TEST(GraphMorphism, ReportsDeclinedUnconnectedSocket)
{
	auto source = MakeChain();
	auto policy = CopyPolicy();
	policy.createInput = [](const GraphI::SocketInT& input) -> std::optional<int> {
		if (input.owner->name == "A")
			return std::nullopt;
		return input.data;
	};

	const auto mapping = Morphism(std::move(policy)).Map(source);

	EXPECT_FALSE(mapping.images.IsTotal(source));
	EXPECT_EQ(mapping.images.MappedConnectionCount(), 2u);
	const auto report = mapping.images.Validate(source);
	ASSERT_EQ(report.problems.size(), 1u);
	EXPECT_NE(report.problems.front().find("A.in"), std::string::npos);
}

TEST(GraphMorphism, TotalRejectsUnmappedConnection)
{
	GraphI source = MakeChain();
	const Morphism mapper{CopyPolicy()};

	auto mapping = mapper.Map(source);

	Morphism::Correspondence withoutEdges;
	for (const auto& node : source.nodes)
	{
		EXPECT_TRUE(withoutEdges.RecordNode(*node, *mapping.images.ImageOf(node.get())));
		for (const auto& input : node->inputs)
			EXPECT_TRUE(withoutEdges.RecordSocket(*input, *mapping.images.ImageOf(input.get())));
		for (const auto& output : node->outputs)
			EXPECT_TRUE(withoutEdges.RecordSocket(*output, *mapping.images.ImageOf(output.get())));
	}

	EXPECT_FALSE(withoutEdges.IsTotal(source));
	const auto report = withoutEdges.Validate(source);
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("A.out"), std::string::npos);
	EXPECT_NE(report.problems[0].find("B.in"), std::string::npos);
}

TEST(GraphMorphism, IncidenceRejectsReversedEdge)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	Morphism::Correspondence images;

	for (const char* name : {"A", "B", "C"})
		EXPECT_TRUE(images.RecordNode(*FindNode(source, name), *FindNode(target, name)));

	auto* aIn = FindInput(FindNode(target, "A"), "in");
	auto* bIn = FindInput(FindNode(target, "B"), "in");
	target.Disconnect(bIn->incoming.from, bIn);
	ASSERT_TRUE(target.Connect(FindOutput(FindNode(target, "B"), "out"), aIn));
	EXPECT_TRUE(images.RecordConnection(FindInput(FindNode(source, "B"), "in")->incoming,
	                                    aIn->incoming));

	EXPECT_FALSE(images.PreservesIncidence());
	EXPECT_FALSE(images.Validate(source).Ok());
}

TEST(GraphMorphism, IncidenceRejectsCrossedEdge)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	Morphism::Correspondence images;

	for (const char* name : {"A", "B", "C"})
		EXPECT_TRUE(images.RecordNode(*FindNode(source, name), *FindNode(target, name)));

	auto* cIn = FindInput(FindNode(target, "C"), "in");
	ASSERT_TRUE(target.Connect(FindOutput(FindNode(target, "A"), "out"), cIn));
	EXPECT_TRUE(images.RecordConnection(FindInput(FindNode(source, "B"), "in")->incoming,
	                                    cIn->incoming));

	EXPECT_FALSE(images.PreservesIncidence());
}

TEST(GraphMorphism, IncidenceRejectsWrongSocketOnMappedOwner)
{
	GraphI source = MakeChain();
	auto mapping = Morphism{CopyPolicy()}.Map(source);
	const auto* edge = source.Connections().front();
	auto* input = mapping.images.ImageOf(edge->to);
	auto* output = mapping.images.ImageOf(edge->from);
	auto* other = output->owner->AddOutput("other");
	ASSERT_TRUE(mapping.target.Connect(other, input));

	EXPECT_FALSE(mapping.images.PreservesIncidence());
	EXPECT_FALSE(mapping.images.Validate(source).Ok());
}

TEST(GraphMorphism, FanOutMapsToDistinctTargetConnectionsAfterMove)
{
	GraphI source = MakeFanOut();
	const Morphism mapper{CopyPolicy()};
	auto result = mapper.Map(source);
	auto moved = std::move(result);

	const auto edges = source.Connections();
	const auto images = moved.target.Connections();
	ASSERT_EQ(edges.size(), 2u);
	ASSERT_EQ(images.size(), 2u);
	EXPECT_NE(images[0], images[1]);
	EXPECT_EQ(moved.images.ImageOf(edges[0]), images[0]);
	EXPECT_EQ(moved.images.ImageOf(edges[1]), images[1]);
}

TEST(GraphMorphism, IncrementalMapCreatesEachNodeAndConnectionOnce)
{
	GraphI source = MakeChain();
	Morphism::Result mapping;
	const Morphism mapper{CopyPolicy()};

	GraphI::NodeType* image = mapper.MapNode(mapping, *FindNode(source, "A"));
	ASSERT_NE(image, nullptr);
	EXPECT_EQ(mapping.images.ImageOf(FindNode(source, "A")), image);

	// A source node maps once; a second call declines instead of duplicating it.
	EXPECT_EQ(mapper.MapNode(mapping, *FindNode(source, "A")), nullptr);

	mapper.MapNode(mapping, *FindNode(source, "B"));
	const auto& edge = FindInput(FindNode(source, "B"), "in")->incoming;
	EXPECT_TRUE(mapper.MapConnect(mapping, edge));
	EXPECT_FALSE(mapper.MapConnect(mapping, edge));

	EXPECT_TRUE(FindInput(FindNode(mapping.target, "B"), "in")->IsConnected());
	EXPECT_EQ(FindInput(FindNode(mapping.target, "B"), "in")->incoming.from->owner->name, "A");
}

TEST(GraphMorphism, SocketLabelsDoNotDetermineCorrespondence)
{
	GraphI source;
	auto* producer = source.AddNode("pass");
	auto* consumer = source.AddNode("pass");
	auto* left = producer->AddOutput("port");
	auto* right = producer->AddOutput("port");
	auto* first = consumer->AddInput("port");
	auto* second = consumer->AddInput("port");
	ASSERT_TRUE(source.Connect(left, first));
	ASSERT_TRUE(source.Connect(right, second));

	const auto mapping = Morphism{CopyPolicy()}.Map(source);

	EXPECT_NE(mapping.images.ImageOf(producer), mapping.images.ImageOf(consumer));
	EXPECT_NE(mapping.images.ImageOf(left), mapping.images.ImageOf(right));
	EXPECT_NE(mapping.images.ImageOf(first), mapping.images.ImageOf(second));
	const auto* firstEdge = mapping.images.ImageOf(&first->incoming);
	const auto* secondEdge = mapping.images.ImageOf(&second->incoming);
	ASSERT_NE(firstEdge, nullptr);
	ASSERT_NE(secondEdge, nullptr);
	EXPECT_EQ(firstEdge->from, mapping.images.ImageOf(left));
	EXPECT_EQ(secondEdge->from, mapping.images.ImageOf(right));
	EXPECT_TRUE(mapping.images.Validate(source).Ok());
}

TEST(GraphMorphism, ConflictingProducersCannotOverwriteAMappedConnection)
{
	GraphI source;
	auto* producer = source.AddNode("producer");
	auto* consumer = source.AddNode("consumer");
	auto* first = consumer->AddInput("first");
	auto* second = consumer->AddInput("second");
	ASSERT_TRUE(source.Connect(producer->AddOutput("left"), first));
	ASSERT_TRUE(source.Connect(producer->AddOutput("right"), second));

	const Morphism mapper{CopyPolicy()};
	Morphism::Result mapping;
	mapper.MapNode(mapping, *producer);
	auto* target = mapping.target.AddNode("consumer");
	auto* shared = target->AddInput("shared");
	ASSERT_TRUE(mapping.images.RecordNode(*consumer, *target));
	ASSERT_TRUE(mapping.images.RecordSocket(*first, *shared));
	ASSERT_TRUE(mapping.images.RecordSocket(*second, *shared));

	EXPECT_TRUE(mapper.MapConnect(mapping, first->incoming));
	EXPECT_FALSE(mapper.MapConnect(mapping, second->incoming));
	const auto* image = mapping.images.ImageOf(&first->incoming);
	ASSERT_NE(image, nullptr);
	EXPECT_EQ(image->to, shared);
	EXPECT_EQ(image->from->name, "left");
	EXPECT_EQ(mapping.images.ImageOf(&second->incoming), nullptr);
	EXPECT_FALSE(mapping.images.Validate(source).Ok());
}

TEST(GraphMorphism, SharedTargetIsAccepted)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	Morphism::Correspondence images;

	EXPECT_TRUE(images.RecordNode(*FindNode(source, "A"), *FindNode(target, "A")));
	EXPECT_TRUE(images.RecordNode(*FindNode(source, "B"), *FindNode(target, "A")));
	EXPECT_EQ(images.ImageOf(FindNode(source, "B")), FindNode(target, "A"));
}

TEST(GraphMorphism, OnePolicyProducesIndependentResults)
{
	const Morphism mapper{CopyPolicy()};
	auto chain = MakeChain();
	auto fanOut = MakeFanOut();
	auto first = mapper.Map(chain);
	auto second = mapper.Map(fanOut);

	auto* chainA = FindNode(chain, "A");
	auto* fanOutA = FindNode(fanOut, "A");
	EXPECT_EQ(first.images.ImageOf(chainA), FindNode(first.target, "A"));
	EXPECT_EQ(second.images.ImageOf(fanOutA), FindNode(second.target, "A"));
	EXPECT_EQ(first.images.ImageOf(fanOutA), nullptr);
	EXPECT_EQ(second.images.ImageOf(chainA), nullptr);
	EXPECT_TRUE(first.images.Validate(chain).Ok());
	EXPECT_TRUE(second.images.Validate(fanOut).Ok());

	// Reusing the same source must produce another independent target as well.
	const auto third = mapper.Map(chain);
	ASSERT_NE(first.images.ImageOf(chainA), third.images.ImageOf(chainA));
	auto* output = FindOutput(FindNode(first.target, "A"), "out");
	auto* input = FindInput(FindNode(first.target, "B"), "in");
	first.target.Disconnect(output, input);
	EXPECT_FALSE(first.images.Validate(chain).Ok());
	EXPECT_TRUE(third.images.Validate(chain).Ok());
	EXPECT_TRUE(second.images.Validate(fanOut).Ok());
}
