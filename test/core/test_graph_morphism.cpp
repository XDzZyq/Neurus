/**
 * @file test_graph_morphism.cpp
 * @brief Functor laws between generic graphs.
 */

#include <gtest/gtest.h>

#include "core/GraphMorphism.hpp"

#include <string>

namespace {

using GraphI = neurus::Graph<int, int>;

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

neurus::GraphMorphism<GraphI, GraphI>::Policy CopyPolicy()
{
	neurus::GraphMorphism<GraphI, GraphI>::Policy policy;
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
	const auto morphism = neurus::IdentityMorphism(graph);

	EXPECT_TRUE(morphism.IsTotal());
	EXPECT_TRUE(morphism.PreservesIncidence());
	EXPECT_TRUE(morphism.Validate().Ok());
	EXPECT_EQ(morphism.MappedNodeCount(), 3u);
	EXPECT_EQ(morphism.MappedConnectionCount(), 2u);
}

TEST(GraphMorphism, MapBuildsTheTarget)
{
	GraphI source = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, CopyPolicy());

	GraphI target = morphism.Map(source);

	EXPECT_TRUE(morphism.IsTotal());
	EXPECT_TRUE(morphism.PreservesIncidence());
	EXPECT_TRUE(morphism.Validate().Ok());
	EXPECT_EQ(morphism.MappedNodeCount(), 3u);
	EXPECT_EQ(morphism.MappedConnectionCount(), 2u);

	ASSERT_EQ(target.nodes.size(), source.nodes.size());
	EXPECT_EQ(morphism.ImageOf(FindNode(source, "B"))->name, "B");
	EXPECT_EQ(FindNode(target, "B")->data, 2);
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

	neurus::GraphMorphism<GraphI, GraphI> morphism(source, std::move(policy));
	const GraphI target = morphism.Map(source);
	(void)target;

	EXPECT_FALSE(morphism.IsTotal());
	const auto report = morphism.Validate();
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("C"), std::string::npos);
}

TEST(GraphMorphism, TotalRejectsUnmappedNode)
{
	GraphI source = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source);

	EXPECT_TRUE(morphism.MapNode(*FindNode(source, "A"), *FindNode(source, "A")));
	EXPECT_TRUE(morphism.MapNode(*FindNode(source, "B"), *FindNode(source, "B")));

	EXPECT_FALSE(morphism.IsTotal());
	const auto report = morphism.Validate();
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("C"), std::string::npos);
}

TEST(GraphMorphism, TotalRejectsUnmappedConnection)
{
	GraphI source = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, CopyPolicy());

	GraphI target = morphism.Map(source);

	// A fresh morphism records nodes only, so its connections are unmapped.
	neurus::GraphMorphism<GraphI, GraphI> nodesOnly(source);
	for (const auto& node : source.nodes)
		EXPECT_TRUE(nodesOnly.MapNode(*node, *FindNode(target, node->name)));

	EXPECT_FALSE(nodesOnly.IsTotal());
	const auto report = nodesOnly.Validate();
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("A.out"), std::string::npos);
	EXPECT_NE(report.problems[0].find("B.in"), std::string::npos);
}

TEST(GraphMorphism, IncidenceRejectsReversedEdge)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source);

	for (const char* name : {"A", "B", "C"})
		EXPECT_TRUE(morphism.MapNode(*FindNode(source, name), *FindNode(target, name)));

	GraphI::NodeType* a = FindNode(source, "A");
	GraphI::NodeType* b = FindNode(source, "B");
	GraphI::NodeType* ta = FindNode(target, "A");
	GraphI::NodeType* tb = FindNode(target, "B");

	EXPECT_TRUE(morphism.MapConnect(*FindOutput(a, "out"), *FindInput(b, "in"),
	                                *FindOutput(tb, "out"), *FindInput(ta, "in")));

	EXPECT_FALSE(morphism.PreservesIncidence());
	EXPECT_FALSE(morphism.Validate().Ok());
}

TEST(GraphMorphism, IncidenceRejectsCrossedEdge)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source);

	for (const char* name : {"A", "B", "C"})
		EXPECT_TRUE(morphism.MapNode(*FindNode(source, name), *FindNode(target, name)));

	GraphI::NodeType* a = FindNode(source, "A");
	GraphI::NodeType* b = FindNode(source, "B");
	GraphI::NodeType* ta = FindNode(target, "A");
	GraphI::NodeType* tc = FindNode(target, "C");

	EXPECT_TRUE(morphism.MapConnect(*FindOutput(a, "out"), *FindInput(b, "in"),
	                                *FindOutput(ta, "out"), *FindInput(tc, "in")));

	EXPECT_FALSE(morphism.PreservesIncidence());
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
	GraphI target;
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, CopyPolicy());

	GraphI::NodeType* image = morphism.MapNode(target, *FindNode(source, "A"));
	ASSERT_NE(image, nullptr);
	EXPECT_EQ(morphism.ImageOf(FindNode(source, "A")), image);

	// A source node maps once; a second call declines instead of duplicating it.
	EXPECT_EQ(morphism.MapNode(target, *FindNode(source, "A")), nullptr);
}

TEST(GraphMorphism, MapConnectCreatesAndWires)
{
	GraphI source = MakeChain();
	GraphI target;
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, CopyPolicy());

	morphism.MapNode(target, *FindNode(source, "A"));
	morphism.MapNode(target, *FindNode(source, "B"));
	EXPECT_EQ(morphism.MapConnect(target, *FindOutput(FindNode(source, "A"), "out"),
	                              *FindInput(FindNode(source, "B"), "in")), true);

	EXPECT_TRUE(FindInput(FindNode(target, "B"), "in")->IsConnected());
	EXPECT_EQ(FindInput(FindNode(target, "B"), "in")->source->owner->name, "A");
}

TEST(GraphMorphism, SharedTargetIsAccepted)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source);

	EXPECT_TRUE(morphism.MapNode(*FindNode(source, "A"), *FindNode(target, "A")));
	EXPECT_TRUE(morphism.MapNode(*FindNode(source, "B"), *FindNode(target, "A")));
	EXPECT_EQ(morphism.ImageOf(FindNode(source, "B")), FindNode(target, "A"));
}
