/**
 * @file Log.h
 * @brief Debug logging utility with an in-process ring buffer.
 *
 * Provides:
 *   NEURUS_LOG - debug-only info logging (compiled out in Release).
 *   NEURUS_ERR - always-on error logging (prints in all builds).
 *   LogBuffer  - thread-safe fixed-capacity ring buffer that every macro
 *                emission appends to, in addition to stdout/stderr output.
 *
 * Both macros automatically inject __func__ and __LINE__ for traceability.
 */

#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <io.h>
#define NEURUS_ISATTY(fd) _isatty(_fileno(fd))
#else
#include <unistd.h>
#define NEURUS_ISATTY(fd) isatty(fileno(fd))
#endif

namespace neurus
{

/** @brief Log severity level. */
enum class LogLevel : uint8_t
{
	Info,  /**< NEURUS_LOG (debug builds only) */
	Error  /**< NEURUS_ERR (all builds) */
};

/**
 * @brief One buffered log line.
 * @note `func` points to compiler-internal static storage (`__func__`),
 *       which lives for the program duration - safe to store and read
 *       from any thread.
 */
struct LogEntry
{
	LogLevel                              level;      // Info or Error
	std::chrono::system_clock::time_point timestamp;  // captured at emission
	const char*                           func;       // __func__
	int                                   line;       // __LINE__
	std::string                           message;    // formatted text
	uint64_t                              seq;        // assigned by Append(), not call sites
};

/** @brief Ring buffer capacity (drops oldest entries beyond this). */
constexpr std::size_t kLogCapacity = 10000;

/**
 * @brief Thread-safe fixed-capacity ring buffer for log entries.
 *
 * Header-only Meyers singleton; an internal mutex guards all accessors.
 * Counts track entries currently in the buffer (InfoCount + ErrorCount ==
 * Size()).
 */
class LogBuffer
{
public:
	/** @brief Returns the process-wide log buffer. */
	static LogBuffer& instance()
	{
		static LogBuffer s_instance;
		return s_instance;
	}

	/**
	 * @brief Appends an entry, assigning its monotonic seq.
	 * @note Drops the oldest entry when full (ring semantics). Seq is
	 *       assigned here, not by the caller, so concurrent producers
	 *       never race on a counter.
	 */
	void Append(LogEntry entry)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		const LogLevel newLevel = entry.level;
		entry.seq = ++m_headSeq;

		if (m_count < kLogCapacity)
		{
			m_ring[(m_head + m_count) % kLogCapacity] = std::move(entry);
			++m_count;
		}
		else
		{
			// Full: evict the oldest, advance head, keep count constant.
			if (m_ring[m_head].level == LogLevel::Error)
				--m_errorCount;
			else
				--m_infoCount;
			m_ring[m_head] = std::move(entry);
			m_head = (m_head + 1) % kLogCapacity;
		}

		if (newLevel == LogLevel::Error)
			++m_errorCount;
		else
			++m_infoCount;
	}

	/** @brief Empties the buffer and resets counts and seq counter. */
	void Clear()
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		m_head = 0;
		m_count = 0;
		m_infoCount = 0;
		m_errorCount = 0;
		m_headSeq = 0;
	}

	/** @brief Live entry count (<= kLogCapacity). */
	std::size_t Size() const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		return m_count;
	}

	/** @brief Count of Info entries currently in the buffer. */
	std::size_t InfoCount() const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		return m_infoCount;
	}

	/** @brief Count of Error entries currently in the buffer. */
	std::size_t ErrorCount() const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		return m_errorCount;
	}

	/**
	 * @brief Returns a copy of the entry at a logical index (0 = oldest).
	 * @note Returns by value: a concurrent Append could otherwise invalidate
	 *       a reference while the caller holds it.
	 */
	LogEntry At(std::size_t logicalIdx) const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		assert(logicalIdx < m_count);
		return m_ring[(m_head + logicalIdx) % kLogCapacity];
	}

	/**
	 * @brief Returns a copy of the entry with a given seq, if still buffered.
	 * @return False when the entry was evicted, cleared or not yet appended.
	 * @note Seqs in the buffer are contiguous, so the newest Size() seqs end at
	 *       HeadSeq(). A reader that indexes by seq keeps pointing at the same
	 *       entry while other threads append and the ring wraps.
	 */
	bool BySeq(uint64_t seq, LogEntry& out) const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (seq == 0 || seq > m_headSeq || m_headSeq - seq >= m_count)
			return false;
		const std::size_t logicalIdx = m_count - 1 - static_cast<std::size_t>(m_headSeq - seq);
		out = m_ring[(m_head + logicalIdx) % kLogCapacity];
		return true;
	}

	/**
	 * @brief Monotonic seq of the newest entry (0 when empty/cleared).
	 * @note A consumer polls this each frame to detect growth (insert rows)
	 *       or a Clear (seq drops -> reset).
	 */
	uint64_t HeadSeq() const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		return m_headSeq;
	}

	/** @brief Newest seq and live size, read together. */
	struct State
	{
		uint64_t    headSeq = 0;
		std::size_t size    = 0;
	};

	/**
	 * @brief Returns HeadSeq() and Size() under one lock.
	 * @note Any thread may Append; two separate reads could straddle an Append
	 *       and report a seq that does not match the size.
	 */
	State Snapshot() const
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		return State{m_headSeq, m_count};
	}

private:
	LogBuffer() = default;

	mutable std::mutex m_mtx;
	std::array<LogEntry, kLogCapacity> m_ring;
	std::size_t m_head = 0;
	std::size_t m_count = 0;
	std::size_t m_infoCount = 0;
	std::size_t m_errorCount = 0;
	uint64_t    m_headSeq = 0;
};

} // namespace neurus

/**
 * @brief Debug-only info log. Prints to std::cout (TTY-colored in Debug) and
 *        appends a LogEntry to the LogBuffer. Compiled out entirely in
 *        Release builds (no append, no count, no cost).
 *
 * Debug detection accepts either MSVC's _DEBUG or a missing NDEBUG, so the
 * log is active in Debug on every supported compiler.
 *
 * @note `msg` is streamed into an ostringstream exactly ONCE, preserving
 *       single-evaluation semantics. __func__/__LINE__ expand at the call
 *       site and feed both the buffer entry and the console line.
 */
#if defined(_DEBUG) || !defined(NDEBUG)
#define NEURUS_LOG(msg) \
	do { \
		std::ostringstream neurusLogOs; \
		neurusLogOs << msg; \
		neurus::LogBuffer::instance().Append( \
			neurus::LogEntry{ \
				neurus::LogLevel::Info, \
				std::chrono::system_clock::now(), \
				__func__, __LINE__, \
				neurusLogOs.str(), 0 /* seq assigned inside Append() */ }); \
		if (NEURUS_ISATTY(stdout)) { \
			std::cout << "\033[36m[" << __func__ << ":" << __LINE__ << "]\033[0m " \
			          << neurusLogOs.str() << "\n"; \
		} else { \
			std::cout << "[" << __func__ << ":" << __LINE__ << "] " \
			          << neurusLogOs.str() << "\n"; \
		} \
	} while(0)
#else
#define NEURUS_LOG(msg) ((void)0)
#endif

/**
 * @brief Always-on error log. Prints to std::cerr (TTY-colored red in
 *        terminals) and appends a LogEntry to the LogBuffer. Active in all
 *        build configurations so errors are never silently swallowed.
 */
#define NEURUS_ERR(msg) \
	do { \
		std::ostringstream neurusLogOs; \
		neurusLogOs << msg; \
		neurus::LogBuffer::instance().Append( \
			neurus::LogEntry{ \
				neurus::LogLevel::Error, \
				std::chrono::system_clock::now(), \
				__func__, __LINE__, \
				neurusLogOs.str(), 0 /* seq assigned inside Append() */ }); \
		if (NEURUS_ISATTY(stderr)) { \
			std::cerr << "\033[1;31m[" << __func__ << ":" << __LINE__ << "] ERROR:\033[0m " \
			          << neurusLogOs.str() << "\n"; \
		} else { \
			std::cerr << "[" << __func__ << ":" << __LINE__ << "] ERROR: " \
			          << neurusLogOs.str() << "\n"; \
		} \
	} while(0)
