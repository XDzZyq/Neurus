# Logging

`core/Log.h` provides two logging macros and a thread-safe in-process ring buffer that every macro emission appends to alongside the console output.

## Macros

Both macros automatically inject `__func__` and `__LINE__` for traceability, and `msg` is streamed into an `ostringstream` exactly once, preserving single-evaluation semantics (`NEURUS_LOG(getCount())` calls `getCount()` once).

- `NEURUS_LOG(msg)` - debug-only info output to `std::cout` (TTY-colored cyan in Debug), and an `Info` `LogEntry` appended to the buffer. Compiled out entirely in Release (no append, no count, no cost).
- `NEURUS_ERR(msg)` - always-on error output to `std::cerr` (TTY-colored red in terminals), and an `Error` `LogEntry` appended to the buffer. Active in all build configurations so errors are never silently swallowed.

Debug detection accepts either MSVC's `_DEBUG` or the standard `NDEBUG`-absent signal, so the log is active in Debug on every supported compiler.

Usage:

```cpp
NEURUS_LOG("[Swapchain] " << extent.width << "x" << extent.height);
NEURUS_ERR("[Texture] failed: " << reason);
```

See [Conventions](../style/conventions.md) for the error-handling rules these macros support.

## LogLevel and LogEntry

- `LogLevel` is `Info` (`NEURUS_LOG`, debug builds only) or `Error` (`NEURUS_ERR`, all builds).
- `LogEntry` holds `level`, a `timestamp` captured at emission, `func` (a pointer to compiler-internal static storage from `__func__`, safe to store and read from any thread), `line`, the formatted `message`, and a monotonic `seq` assigned by `Append`, not by call sites.

## LogBuffer

`LogBuffer` is a header-only Meyers singleton (`instance()`) so `neurus_core` stays an INTERFACE library. Magic statics make `instance()` thread-safe, and an internal mutex guards all accessors.

- Fixed-capacity ring buffer: `kLogCapacity` is 10000 entries; when full, the oldest entry is evicted and the head advances, keeping the count constant.
- `Append(LogEntry)` assigns the entry's `seq` under the lock, so concurrent producers never race on a counter.
- `Clear()` empties the buffer and resets counts and the seq counter.
- Accessors: `Size()`, `InfoCount()`, `ErrorCount()` (with `InfoCount() + ErrorCount() == Size()`), `At(logicalIdx)` (returns a copy, since a concurrent `Append` could invalidate a reference), `HeadSeq()` (the monotonic seq of the newest entry, or 0 when empty/cleared), `Snapshot()` (seq and size under one lock) and `BySeq(seq, out)` (the entry with that seq, if still buffered; seqs in the buffer are contiguous).

## Consumers

A consumer polls `Snapshot()` each frame to detect growth (insert rows), eviction, or a `Clear` (seq drops, so the model resets). Any thread may log - the render thread does - so a consumer must not treat the live `Size()` as its own row count. In the UI layer, `LogModel` is a `QAbstractListModel` over the `LogBuffer`, `LogFilterProxy` adds level filtering and search, and `LogDelegate` draws severity-colored rows. See [Layers](../architecture/layers.md).
