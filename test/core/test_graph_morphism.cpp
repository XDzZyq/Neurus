/**
 * @file test_graph_images.cpp
 * @brief Functor laws between generic graphs.
 */

#include <gtest/gtest.h>

#include "core/GraphMorphism.hpp"

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

/// @brief Creates a target node with the same name, payload and sockets.
GraphI::NodeType* CopyNode(GraphI& target, const GraphI::NodeType& source)
{
	GraphI::NodeType* node = target.AddNode(source.name, source.data);
	for (const auto& input : source.inputs)
		node->AddInput(input->name)->data = input->data;
	for (const auto& output : source.outputs)
		node->AddOutput(output->name)->data = output->data;
	return node;
}

Morphism::Policy CopyPolicy()
{
	Morphism::Policy policy;
	policy.createNode = [](GraphI& target, const GraphI::NodeType& node)
	{
		return CopyNode(target, node);
	};
	policy.createOutput = [](GraphI::NodeType& node, const GraphI::SocketOutT& port)
	{
		return FindOutput(&node, port.name);
	};
	policy.createInput = [](GraphI::NodeType& node, const GraphI::SocketInT& port)
	{
		return FindInput(&node, port.name);
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

TEST(GraphMorphism, MapReportsNullFactory)
{
	const GraphI source = MakeChain();
	auto policy = CopyPolicy();
	policy.createNode = [](GraphI& target, const GraphI::NodeType& node)
	{
		if (node.name == "C")
			return static_cast<GraphI::NodeType*>(nullptr);
		return CopyNode(target, node);
	};

	const Morphism mapper(std::move(policy));
	const auto mapping = mapper.Map(source);

	EXPECT_FALSE(mapping.images.IsTotal(source));
	const auto report = mapping.images.Validate(source);
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("C"), std::string::npos);
}

TEST(GraphMorphism, TotalRejectsUnmappedNode)
{
	GraphI source = MakeChain();
	Morphism::Correspondence images;

	EXPECT_TRUE(images.RecordNode(*FindNode(source, "A"), *FindNode(source, "A")));
	EXPECT_TRUE(images.RecordNode(*FindNode(source, "B"), *FindNode(source, "B")));

	EXPECT_FALSE(images.IsTotal(source));
	const auto report = images.Validate(source);
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("C"), std::string::npos);
}

TEST(GraphMorphism, TotalRejectsUnmappedConnection)
{
	GraphI source = MakeChain();
	const Morphism mapper{CopyPolicy()};

	auto mapping = mapper.Map(source);

	// Record just the node images to exercise missing-edge diagnostics.
	Morphism::Correspondence nodesOnly;
	for (const auto& node : source.nodes)
		EXPECT_TRUE(nodesOnly.RecordNode(*node, *FindNode(mapping.target, node->name)));

	EXPECT_FALSE(nodesOnly.IsTotal(source));
	const auto report = nodesOnly.Validate(source);
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

	GraphI::NodeType* a = FindNode(source, "A");
	GraphI::NodeType* b = FindNode(source, "B");
	GraphI::NodeType* ta = FindNode(target, "A");
	GraphI::NodeType* tb = FindNode(target, "B");

	EXPECT_TRUE(images.RecordConnection(*FindOutput(a, "out"), *FindInput(b, "in"),
	                                   *FindOutput(tb, "out"), *FindInput(ta, "in")));

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

	GraphI::NodeType* a = FindNode(source, "A");
	GraphI::NodeType* b = FindNode(source, "B");
	GraphI::NodeType* ta = FindNode(target, "A");
	GraphI::NodeType* tc = FindNode(target, "C");

	EXPECT_TRUE(images.RecordConnection(*FindOutput(a, "out"), *FindInput(b, "in"),
	                                   *FindOutput(ta, "out"), *FindInput(tc, "in")));

	EXPECT_FALSE(images.PreservesIncidence());
}

TEST(GraphMorphism, EnumerateConnectionsSeesEveryEdge)
{
	GraphI graph = MakeFanOut();
	const auto connections = neurus::EnumerateConnections(graph);

	ASSERT_EQ(connections.size(), 2u);
	EXPECT_EQ(connections[0].from->owner->name, "A");
	EXPECT_EQ(connections[0].to->owner->name, "B");
	EXPECT_EQ(connections[1].to->owner->name, "C");
}

TEST(GraphMorphism, MapNodeMapsOneElement)
{
	GraphI source = MakeChain();
	Morphism::Result mapping;
	const Morphism mapper{CopyPolicy()};

	GraphI::NodeType* image = mapper.MapNode(mapping, *FindNode(source, "A"));
	ASSERT_NE(image, nullptr);
	EXPECT_EQ(mapping.images.ImageOf(FindNode(source, "A")), image);

	// A source node maps once; a second call declines instead of duplicating it.
	EXPECT_EQ(mapper.MapNode(mapping, *FindNode(source, "A")), nullptr);
}

TEST(GraphMorphism, MapConnectCreatesAndWires)
{
	GraphI source = MakeChain();
	Morphism::Result mapping;
	const Morphism mapper{CopyPolicy()};

	mapper.MapNode(mapping, *FindNode(source, "A"));
	mapper.MapNode(mapping, *FindNode(source, "B"));
	EXPECT_TRUE(mapper.MapConnect(mapping, *FindOutput(FindNode(source, "A"), "out"),
	                               *FindInput(FindNode(source, "B"), "in")));

	EXPECT_TRUE(FindInput(FindNode(mapping.target, "B"), "in")->IsConnected());
	EXPECT_EQ(FindInput(FindNode(mapping.target, "B"), "in")->source->owner->name, "A");
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
