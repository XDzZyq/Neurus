/**
 * @file GPUProfiler.cpp
 * @brief Implementation of the section-based GPU timestamp profiler.
 */

#include "GPUProfiler.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace neurus {

void GPUProfiler::Initialize(const vk::raii::Device& device,
                             const vk::raii::PhysicalDevice& physicalDevice,
                             uint32_t graphicsQueueFamily,
                             uint32_t maxFramesInFlight)
{
	m_maxFramesInFlight = maxFramesInFlight;
	m_currentSlot = 0;
	m_pool.reset();
	m_available = false;

	m_slotSections.assign(maxFramesInFlight, {});
	m_written.assign(maxFramesInFlight, 0);
	m_resolved.clear();
	m_resolvedFrameMs = 0.0;

	const vk::PhysicalDeviceProperties props = physicalDevice.getProperties();
	const std::vector<vk::QueueFamilyProperties> queueFamilies =
		physicalDevice.getQueueFamilyProperties();
	const bool queueTimestamps = graphicsQueueFamily < queueFamilies.size() &&
	                             queueFamilies[graphicsQueueFamily].timestampValidBits > 0;

	if (!props.limits.timestampComputeAndGraphics || !queueTimestamps)
	{
		NEURUS_LOG("[GPUProfiler] Timestamp queries unsupported - GPU timing disabled "
		           << "(timestampComputeAndGraphics=" << props.limits.timestampComputeAndGraphics
		           << ", queueTimestampBits="
		           << (graphicsQueueFamily < queueFamilies.size()
		                   ? queueFamilies[graphicsQueueFamily].timestampValidBits : 0) << ")");
		return;
	}

	m_timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);
	vk::QueryPoolCreateInfo poolCI(vk::QueryPoolCreateFlags{},
	                               vk::QueryType::eTimestamp,
	                               maxFramesInFlight * QueriesPerFrame());
	m_pool = std::make_unique<vk::raii::QueryPool>(device, poolCI);
	m_available = true;

	NEURUS_LOG("[GPUProfiler] Timestamp query pool created ("
	           << maxFramesInFlight << " frames x " << QueriesPerFrame()
	           << " queries, period=" << m_timestampPeriodNs << "ns)");
}

void GPUProfiler::BeginFrame(vk::CommandBuffer cmd)
{
	if (!m_available)
		return;

	const uint32_t base = SlotBase(m_currentSlot);
	cmd.resetQueryPool(**m_pool, base, QueriesPerFrame());
	m_slotSections[m_currentSlot].clear();
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, **m_pool, base);
}

void GPUProfiler::BeginPass(vk::CommandBuffer cmd, std::string_view name)
{
	if (!m_available)
		return;

	auto& sections = m_slotSections[m_currentSlot];
	const uint32_t i = static_cast<uint32_t>(sections.size());
	if (i >= kMaxPasses)
		return; // Overflow guard: silently drop extra passes.

	const uint32_t beginQuery = SlotBase(m_currentSlot) + 2 + 2 * i;
	sections.push_back(Section{ std::string(name), beginQuery, beginQuery + 1 });
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, **m_pool, beginQuery);
}

void GPUProfiler::EndPass(vk::CommandBuffer cmd)
{
	if (!m_available)
		return;

	auto& sections = m_slotSections[m_currentSlot];
	if (sections.empty())
		return;

	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
	                   **m_pool, sections.back().endQuery);
}

void GPUProfiler::EndFrame(vk::CommandBuffer cmd)
{
	if (!m_available)
		return;

	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
	                   **m_pool, SlotBase(m_currentSlot) + 1);
	m_written[m_currentSlot] = 1;
	m_currentSlot = (m_currentSlot + 1) % m_maxFramesInFlight;
}

bool GPUProfiler::Resolve()
{
	if (!m_available || !m_written[m_currentSlot])
		return false;

	const auto& sections = m_slotSections[m_currentSlot];
	const uint32_t passCount = static_cast<uint32_t>(sections.size());
	const uint32_t queryCount = 2 + 2 * passCount;
	const uint32_t base = SlotBase(m_currentSlot);

	try
	{
		const auto resultValue = m_pool->getResults<uint64_t>(
			base, queryCount, queryCount * sizeof(uint64_t),
			sizeof(uint64_t), vk::QueryResultFlagBits::e64);
		if (resultValue.result != vk::Result::eSuccess)
			return false; // eNotReady - keep previous profile.
		m_results = resultValue.value;
	}
	catch (const vk::SystemError&)
	{
		return false; // Device lost / OOM - keep previous profile.
	}

	const double ticksToMs = m_timestampPeriodNs / 1'000'000.0;

	m_resolved.clear();
	m_resolved.reserve(passCount);
	for (uint32_t i = 0; i < passCount; ++i)
	{
		const double begin = static_cast<double>(m_results[2 + 2 * i]);
		const double end   = static_cast<double>(m_results[2 + 2 * i + 1]);
		m_resolved.push_back(SectionTime{ sections[i].name,
		                                  std::max(0.0, (end - begin) * ticksToMs) });
	}

	m_resolvedFrameMs = std::max(0.0,
		(static_cast<double>(m_results[1]) - static_cast<double>(m_results[0])) * ticksToMs);
	return true;
}

} // namespace neurus
