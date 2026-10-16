/**
 * @file ScriptConsole.h
 * @brief Thread-safe ring buffer of Script Console lines.
 *
 * Mirrors the `LogBuffer` design in `core/Log.h`: a header-only, mutex-guarded
 * fixed-capacity ring that drops the oldest line when full. The embedded
 * `ScriptEngine` writes captured Python `stdout`/`stderr` and formatted
 * tracebacks here; the Script Console panel reads it through the same
 * poll model `LogPanel` uses for `LogBuffer`.
 */

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace neurus
{

/** @brief Origin/kind of one Script Console line. */
enum class ScriptLineKind : uint8_t
{
	Input,   /**< echoed user snippet or file path */
	Output,  /**< captured Python `stdout` */
	Error,   /**< captured Python `stderr` or a formatted traceback */
	Result   /**< evaluation result of the last expression */
};

/** @brief One buffered Script Console line. */
struct ScriptLine
{
	ScriptLineKind kind;  // Input/Output/Error/Result
	uint64_t       seq;   // assigned by Append(), not call sites
	std::string    text;  // line body (may contain trailing newline)
};

/** @brief Ring buffer capacity (drops oldest lines beyond this). */
constexpr std::size_t kScriptConsoleCapacity = 2000;

/**
 * @brief Thread-safe fixed-capacity ring buffer for Script Console lines.
 *
 * Header-only; an internal mutex guards all accessors. `seq` is a monotonic
 * counter assigned inside Append(), so a polling consumer can detect growth
 * or a Clear without extra synchronisation.
 */
class ScriptConsoleBuffer
{
public:
	/**
	 * @brief Appends a line, assigning its monotonic seq.
	 * @note Drops the oldest line when full (ring semantics). Seq is assigned
	 *       here, not by the caller, so concurrent producers never race.
	 */
	void Append(ScriptLineKind kind, std::string text)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		ScriptLine line{kind, ++m_headSeq, std::move(text)};

		if (m_count < kScriptConsoleCapacity)
		{
			m_ring[(m_head + m_count) % kScriptConsoleCapacity] = std::move(line);
			++m_count;
		}
		else
		{
			// Full: evict the oldest, advance head, keep count constant.
			m_ring[m_head] = std::move(line);
			m_head = (m_head + 1) % kScriptConsoleCapacity;
		}
	}

	/** @brief Empties the buffer and resets the seq counter. */
	void Clear()
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		m_head = 0;
		m_count = 0;
		m_headSeq = 0;
	}

	/** @brief Live line count (<= kScriptConsoleCapacity). */
	std::size_t Size() const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		return m_count;
	}

	/**
	 * @brief Returns a copy of the line at a logical index (0 = oldest).
	 * @note Returns by value: a concurrent Append could otherwise invalidate
	 *       a reference while the caller holds it.
	 */
	ScriptLine At(std::size_t index) const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		assert(index < m_count);
		return m_ring[(m_head + index) % kScriptConsoleCapacity];
	}

private:
	mutable std::mutex m_mtx;
	std::array<ScriptLine, kScriptConsoleCapacity> m_ring;
	std::size_t m_head = 0;
	std::size_t m_count = 0;
	uint64_t    m_headSeq = 0;
};

}  // namespace neurus
