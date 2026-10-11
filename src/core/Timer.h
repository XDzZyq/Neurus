/**
 * @file Timer.h
 * @brief Scoped timer for debug-mode performance instrumentation.
 *
 * Prints elapsed wall-clock time via NEURUS_LOG on destruction; zero runtime
 * cost in Release (NEURUS_LOG compiles to a no-op).
 */

#pragma once

#include "core/Log.h"

#include <chrono>

namespace neurus {

/**
 * @brief Records wall-clock time from construction to destruction.
 *
 * Logs "label: X.XXX ms" through NEURUS_LOG.
 */
class ScopedTimer
{
public:
	/**
	 * @param label Human-readable operation name.
	 */
	explicit ScopedTimer(std::string label)
		: m_label(std::move(label))
		, m_start(std::chrono::high_resolution_clock::now())
	{}

	~ScopedTimer()
	{
		auto end = std::chrono::high_resolution_clock::now();
		double ms = std::chrono::duration<double, std::milli>(end - m_start).count();
		NEURUS_LOG("[Timer] " << m_label << ": " << ms << " ms");
	}

	// Non-copyable, non-movable
	ScopedTimer(const ScopedTimer&) = delete;
	ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
	std::string m_label;
	std::chrono::high_resolution_clock::time_point m_start;
};

} // namespace neurus

/// Shorthand for a named scoped timer
#define NEURUS_TIMER(label) neurus::ScopedTimer _neurus_timer(label)
