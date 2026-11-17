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

GraphI MakeNodesOnly()
{
	GraphI graph;
	for (const char* name : {"A", "B", "C"})
	{
		GraphI::NodeType* node = graph.AddNode(name, 1);
		node->AddInput("in");
		node->AddOutput("out");
	}
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

neurus::GraphMapPolicy<GraphI, GraphI> PolicyFor(GraphI& target)
{
	neurus::GraphMapPolicy<GraphI, GraphI> policy;
	policy.mapNode = [&target](const GraphI::NodeType& source)
	{
		return FindNode(target, source.name);
	};
	policy.mapOutput = [&target](const GraphI::NodeType& sourceNode,
	                             const GraphI::SocketOutT& sourcePort)
	{
		GraphI::NodeType* node = FindNode(target, sourceNode.name);
		return node ? FindOutput(node, sourcePort.name) : nullptr;
	};
	policy.mapInput = [&target](const GraphI::NodeType& sourceNode,
	                            const GraphI::SocketInT& sourcePort)
	{
		GraphI::NodeType* node = FindNode(target, sourceNode.name);
		return node ? FindInput(node, sourcePort.name) : nullptr;
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

TEST(GraphMorphism, TotalRejectsUnmappedNode)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, target);

	EXPECT_TRUE(morphism.MapNode(FindNode(source, "A"), FindNode(target, "A")));
	EXPECT_TRUE(morphism.MapNode(FindNode(source, "B"), FindNode(target, "B")));

	EXPECT_FALSE(morphism.IsTotal());
	const auto report = morphism.Validate();
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("C"), std::string::npos);
}

TEST(GraphMorphism, TotalRejectsUnmappedConnection)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, target);

	for (const char* name : {"A", "B", "C"})
		EXPECT_TRUE(morphism.MapNode(FindNode(source, name), FindNode(target, name)));

	GraphI::NodeType* a = FindNode(source, "A");
	GraphI::NodeType* b = FindNode(source, "B");
	GraphI::NodeType* ta = FindNode(target, "A");
	GraphI::NodeType* tb = FindNode(target, "B");

	EXPECT_TRUE(morphism.MapConnection(FindOutput(a, "out"), FindInput(b, "in"),
	                                   FindOutput(ta, "out"), FindInput(tb, "in")));

	EXPECT_FALSE(morphism.IsTotal());
	const auto report = morphism.Validate();
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("B.out"), std::string::npos);
	EXPECT_NE(report.problems[0].find("C.in"), std::string::npos);
}

TEST(GraphMorphism, IncidenceRejectsReversedEdge)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, target);

	for (const char* name : {"A", "B", "C"})
		EXPECT_TRUE(morphism.MapNode(FindNode(source, name), FindNode(target, name)));

	GraphI::NodeType* a = FindNode(source, "A");
	GraphI::NodeType* b = FindNode(source, "B");
	GraphI::NodeType* ta = FindNode(target, "A");
	GraphI::NodeType* tb = FindNode(target, "B");

	EXPECT_TRUE(morphism.MapConnection(FindOutput(a, "out"), FindInput(b, "in"),
	                                   FindOutput(tb, "out"), FindInput(ta, "in")));

	EXPECT_FALSE(morphism.PreservesIncidence());
	EXPECT_FALSE(morphism.Validate().Ok());
}

TEST(GraphMorphism, IncidenceRejectsCrossedEdge)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, target);

	for (const char* name : {"A", "B", "C"})
		EXPECT_TRUE(morphism.MapNode(FindNode(source, name), FindNode(target, name)));

	GraphI::NodeType* a = FindNode(source, "A");
	GraphI::NodeType* b = FindNode(source, "B");
	GraphI::NodeType* ta = FindNode(target, "A");
	GraphI::NodeType* tc = FindNode(target, "C");

	EXPECT_TRUE(morphism.MapConnection(FindOutput(a, "out"), FindInput(b, "in"),
	                                   FindOutput(ta, "out"), FindInput(tc, "in")));

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

TEST(GraphMorphism, MapGraphBuildsFromPolicy)
{
	GraphI source = MakeChain();
	GraphI target = MakeNodesOnly();

	const auto morphism = neurus::MapGraph(source, target, PolicyFor(target));

	EXPECT_TRUE(morphism.IsTotal());
	EXPECT_TRUE(morphism.PreservesIncidence());
	EXPECT_TRUE(morphism.Validate().Ok());
	EXPECT_EQ(morphism.MappedNodeCount(), 3u);
	EXPECT_EQ(morphism.MappedConnectionCount(), 2u);
	EXPECT_EQ(morphism.ImageOf(FindNode(source, "B")), FindNode(target, "B"));
}

TEST(GraphMorphism, MapGraphReportsNullFactory)
{
	GraphI source = MakeChain();
	GraphI target = MakeNodesOnly();

	auto policy = PolicyFor(target);
	policy.mapNode = [&target](const GraphI::NodeType& node)
	{
		if (node.name == "C")
			return static_cast<GraphI::NodeType*>(nullptr);
		return FindNode(target, node.name);
	};

	const auto morphism = neurus::MapGraph(source, target, policy);

	EXPECT_FALSE(morphism.IsTotal());
	const auto report = morphism.Validate();
	ASSERT_FALSE(report.Ok());
	EXPECT_NE(report.problems[0].find("C"), std::string::npos);
}

TEST(GraphMorphism, ForeignPointerIsRefused)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	GraphI other = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, target);

	EXPECT_FALSE(morphism.MapNode(FindNode(other, "A"), FindNode(target, "A")));
	EXPECT_FALSE(morphism.MapNode(nullptr, FindNode(target, "A")));
}

TEST(GraphMorphism, SharedTargetIsAccepted)
{
	GraphI source = MakeChain();
	GraphI target = MakeChain();
	neurus::GraphMorphism<GraphI, GraphI> morphism(source, target);

	EXPECT_TRUE(morphism.MapNode(FindNode(source, "A"), FindNode(target, "A")));
	EXPECT_TRUE(morphism.MapNode(FindNode(source, "B"), FindNode(target, "A")));
	EXPECT_EQ(morphism.ImageOf(FindNode(source, "B")), FindNode(target, "A"));
}
