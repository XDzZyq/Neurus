/**
 * @file test_graph_clone.cpp
 * @brief Deep copy of a generic graph.
 */

#include <gtest/gtest.h>

#include "core/Graph.hpp"

#include <string>
#include <tuple>
#include <vector>

namespace {

using GraphI = neurus::Graph<int, std::string>;

struct Connection
{
	std::string from;
	std::string fromPort;
	std::string to;
	std::string toPort;

	bool operator==(const Connection&) const = default;
};

GraphI::NodeType* FindNode(GraphI& graph, const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

const GraphI::NodeType* FindNode(const GraphI& graph, const std::string& name)
{
	for (const auto& node : graph.nodes)
	{
		if (node->name == name)
			return node.get();
	}
	return nullptr;
}

/// @brief Every connection, in node and socket order.
std::vector<Connection> Connections(const GraphI& graph)
{
	std::vector<Connection> connections;
	for (const auto& node : graph.nodes)
	{
		for (const auto& input : node->inputs)
		{
			if (!input->source)
				continue;
			connections.push_back({input->source->owner->name, input->source->name,
			                       node->name, input->name});
		}
	}
	return connections;
}

GraphI MakeChain()
{
	GraphI graph;
	GraphI::NodeType* a = graph.AddNode("A", "first");
	GraphI::NodeType* b = graph.AddNode("B", "second");
	GraphI::NodeType* c = graph.AddNode("C", "third");

	for (GraphI::NodeType* node : {a, b, c})
	{
		node->AddInput("in")->data = 1;
		node->AddOutput("out")->data = 2;
	}

	EXPECT_TRUE(graph.Connect(FindNode(graph, "A")->outputs[0].get(),
	                          FindNode(graph, "B")->inputs[0].get()));
	EXPECT_TRUE(graph.Connect(FindNode(graph, "B")->outputs[0].get(),
	                          FindNode(graph, "C")->inputs[0].get()));
	return graph;
}

GraphI MakeFanOut()
{
	GraphI graph;
	GraphI::NodeType* a = graph.AddNode("A", "root");
	GraphI::NodeType* b = graph.AddNode("B", "left");
	GraphI::NodeType* c = graph.AddNode("C", "right");

	for (GraphI::NodeType* node : {a, b, c})
	{
		node->AddInput("in")->data = 0;
		node->AddOutput("out")->data = 7;
	}

	EXPECT_TRUE(graph.Connect(FindNode(graph, "A")->outputs[0].get(),
	                          FindNode(graph, "B")->inputs[0].get()));
	EXPECT_TRUE(graph.Connect(FindNode(graph, "A")->outputs[0].get(),
	                          FindNode(graph, "C")->inputs[0].get()));
	return graph;
}

} // anonymous namespace

TEST(GraphClone, EqualPayloads)
{
	const GraphI original = MakeChain();
	const GraphI copy = neurus::CloneGraph(original);

	ASSERT_EQ(copy.nodes.size(), original.nodes.size());
	for (size_t i = 0; i < copy.nodes.size(); ++i)
	{
		EXPECT_EQ(copy.nodes[i]->name, original.nodes[i]->name);
		EXPECT_EQ(copy.nodes[i]->data, original.nodes[i]->data);
	}

	for (const auto& node : copy.nodes)
	{
		ASSERT_EQ(node->inputs.size(), 1u);
		ASSERT_EQ(node->outputs.size(), 1u);
		EXPECT_EQ(node->inputs[0]->name, "in");
		EXPECT_EQ(node->inputs[0]->data, 1);
		EXPECT_EQ(node->outputs[0]->name, "out");
		EXPECT_EQ(node->outputs[0]->data, 2);
	}
}

TEST(GraphClone, EqualTopology)
{
	const GraphI original = MakeFanOut();
	const GraphI copy = neurus::CloneGraph(original);

	EXPECT_EQ(Connections(copy), Connections(original));

	ASSERT_EQ(Connections(copy).size(), 2u);
	EXPECT_EQ(Connections(copy)[0].from, "A");
	EXPECT_EQ(Connections(copy)[0].to, "B");
	EXPECT_EQ(Connections(copy)[1].to, "C");  // fan-out is preserved per consumer
	EXPECT_EQ(FindNode(copy, "A")->outputs[0]->FanOut(), 2u);
}

TEST(GraphClone, IsIndependent)
{
	const GraphI original = MakeChain();
	GraphI copy = neurus::CloneGraph(original);

	EXPECT_TRUE(copy.RemoveNode(FindNode(copy, "B")));
	EXPECT_EQ(copy.nodes.size(), 2u);
	EXPECT_TRUE(Connections(copy).empty());

	EXPECT_EQ(original.nodes.size(), 3u);
	EXPECT_EQ(Connections(original).size(), 2u);

	FindNode(copy, "C")->data = "changed";
	EXPECT_EQ(FindNode(original, "C")->data, "third");
}

TEST(GraphClone, PreservesSocketOrder)
{
	GraphI graph;
	GraphI::NodeType* node = graph.AddNode("N", "node");
	for (const char* name : {"a", "b", "c"})
		node->AddInput(name);
	for (const char* name : {"x", "y"})
		node->AddOutput(name);

	const GraphI copy = neurus::CloneGraph(graph);
	ASSERT_EQ(copy.nodes.size(), 1u);

	const GraphI::NodeType& cloned = *copy.nodes[0];
	ASSERT_EQ(cloned.inputs.size(), 3u);
	ASSERT_EQ(cloned.outputs.size(), 2u);
	EXPECT_EQ(cloned.inputs[0]->name, "a");
	EXPECT_EQ(cloned.inputs[1]->name, "b");
	EXPECT_EQ(cloned.inputs[2]->name, "c");
	EXPECT_EQ(cloned.outputs[0]->name, "x");
	EXPECT_EQ(cloned.outputs[1]->name, "y");
}
