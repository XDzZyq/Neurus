#include "editor/controllers/RenderConfigController.hpp"

#include <memory>
#include <optional>

#include "editor/events/ConfigEvents.hpp"
#include "editor/events/EditorEvents.hpp"
#include "editor/operations/ConfigOperations.hpp"

namespace neurus {

void RenderConfigController::Init(ControllerContext& ctx)
{
	const auto read = [ctx]() -> std::optional<RenderConfig>
	{
		if (ctx.configData)
		{
			const auto& data = ctx.configData();
			return data ? std::optional<RenderConfig>(*data) : std::nullopt;
		}
		if (ctx.config)
			if (RenderConfig* config = ctx.config())
				return *config;
		return std::nullopt;
	};
	ctx.events.Subscribe<ConfigEditBegin>([this, read](const ConfigEditBegin&) {
		m_before = read();
		m_editing = true;
	});
	ctx.events.Subscribe<RenderConfigChangedEvent>([this, ctx, read](const RenderConfigChangedEvent& event) {
		const auto before = read();
		const std::optional<RenderConfig> after = event.useDefault
			? std::nullopt : std::optional<RenderConfig>(event.config);
		if (ctx.configData)
		{
			auto& data = ctx.configData();
			if (!after)
				data.reset();
			else if (data)
				*data = *after;
			else
				data = std::make_shared<RenderConfig>(*after);
		}
		else
		{
			if (!ctx.config) return;
			RenderConfig* config = ctx.config();
			if (!config) return;
			*config = after.value_or(RenderConfig{});
		}
		ctx.events.Emit(RenderResetEvent{});
		if (!m_editing && before != after)
			ctx.ops.Submit(std::make_unique<SetRenderConfigOp>(before, after));
	});
	ctx.events.Subscribe<ConfigEditEnd>([this, ctx, read](const ConfigEditEnd&) {
		const auto after = read();
		if (m_editing && m_before != after)
			ctx.ops.Submit(std::make_unique<SetRenderConfigOp>(m_before, after));
		m_editing = false;
	});
}

} // namespace neurus
