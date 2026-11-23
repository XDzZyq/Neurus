#pragma once

#include "asset/data/render_graph/RenderGraphRecord.hpp"
#include "editor/events/GraphTopologyEvents.hpp"
#include "editor/operations/GraphOperation.hpp"

namespace neurus {

/// Adds or removes a disconnected node at its original position in the document.
class GraphNodeOp : public GraphOperation
{
public:
	GraphNodeOp() = default;
	GraphNodeOp(int uid, project::RenderGraphNodeRecord node, size_t index, bool present)
		: m_uid(uid), m_node(std::move(node)), m_index(index), m_present(present) {}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.Emit(GraphNodeMembershipChanged{m_uid, m_node, m_index, m_present});
	}
	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<GraphNodeOp>(m_uid, m_node, m_index, !m_present);
	}
	std::string Label() const override { return m_present ? "Add Graph Node" : "Remove Graph Node"; }
	template<class Archive> void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(m_uid), CEREAL_NVP(m_node), CEREAL_NVP(m_index), CEREAL_NVP(m_present));
	}

private:
	int m_uid = 0;
	project::RenderGraphNodeRecord m_node;
	size_t m_index = 0;
	bool m_present = true;
};

class RenameGraphNodeOp : public GraphOperation
{
public:
	RenameGraphNodeOp() = default;
	RenameGraphNodeOp(int uid, std::string before, std::string after)
		: m_uid(uid), m_before(std::move(before)), m_after(std::move(after)) {}

	void Apply(OperationContext& ctx) override { ctx.events.Emit(GraphNodeRenamed{m_uid, m_before, m_after}); }
	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<RenameGraphNodeOp>(m_uid, m_after, m_before);
	}
	std::string Label() const override { return "Rename Graph Node"; }
	template<class Archive> void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(m_uid), CEREAL_NVP(m_before), CEREAL_NVP(m_after));
	}

private:
	int m_uid = 0;
	std::string m_before, m_after;
};

/// A single edge. An empty destination node addresses a public scene output.
class GraphConnectionOp : public GraphOperation
{
public:
	GraphConnectionOp() = default;
	GraphConnectionOp(int uid, GraphEndpoint source, GraphEndpoint destination, bool linked)
		: m_uid(uid), m_source(std::move(source)), m_destination(std::move(destination)), m_linked(linked) {}

	void Apply(OperationContext& ctx) override
	{
		if (m_linked)
			ctx.events.Emit(GraphProducerChanged{m_uid, m_destination, {}, m_source});
		else
			ctx.events.Emit(GraphProducerChanged{m_uid, m_destination, m_source, {}});
	}
	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<GraphConnectionOp>(m_uid, m_source, m_destination, !m_linked);
	}
	std::string Label() const override { return m_linked ? "Link Graph Socket" : "Unlink Graph Socket"; }
	template<class Archive> void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(m_uid), cereal::make_nvp("from", m_source.node), cereal::make_nvp("fromPort", m_source.port),
		   cereal::make_nvp("to", m_destination.node), cereal::make_nvp("toPort", m_destination.port),
		   CEREAL_NVP(m_linked));
	}

private:
	int m_uid = 0;
	GraphEndpoint m_source, m_destination;
	bool m_linked = false;
};

class SetGraphOutputTagOp : public GraphOperation
{
public:
	SetGraphOutputTagOp() = default;
	SetGraphOutputTagOp(int uid, GraphEndpoint output, std::optional<GraphOutputTag> before,
	                    std::optional<GraphOutputTag> after)
		: m_uid(uid), m_output(std::move(output)), m_before(before), m_after(after) {}

	void Apply(OperationContext& ctx) override { ctx.events.Emit(GraphOutputTagChanged{m_uid, m_output, m_after}); }
	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SetGraphOutputTagOp>(m_uid, m_output, m_after, m_before);
	}
	std::string Label() const override { return "Set Graph Output Tag"; }
	template<class Archive> void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(m_uid), cereal::make_nvp("node", m_output.node), cereal::make_nvp("port", m_output.port),
		   CEREAL_NVP(m_before), CEREAL_NVP(m_after));
	}

private:
	int m_uid = 0;
	GraphEndpoint m_output;
	std::optional<GraphOutputTag> m_before, m_after;
};

class SetGraphShaderOp : public GraphOperation
{
public:
	SetGraphShaderOp() = default;
	SetGraphShaderOp(int uid, std::string node, int before, int after)
		: m_uid(uid), m_node(std::move(node)), m_before(before), m_after(after) {}

	void Apply(OperationContext& ctx) override { ctx.events.Emit(GraphNodeShaderChanged{m_uid, m_node, m_after}); }
	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SetGraphShaderOp>(m_uid, m_node, m_after, m_before);
	}
	std::string Label() const override { return "Set Graph Node Shader"; }
	template<class Archive> void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(m_uid), CEREAL_NVP(m_node), CEREAL_NVP(m_before), CEREAL_NVP(m_after));
	}

private:
	int m_uid = 0;
	std::string m_node;
	int m_before = 0, m_after = 0;
};

} // namespace neurus
