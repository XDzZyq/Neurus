/**
 * @file ProxyBase.h
 * @brief Shared state of every Python proxy class.
 *
 * A proxy is a thin, copyable handle: the read-only context of the run that
 * produced it plus the UID of the scene object it stands for. Values are read
 * live through the context on every attribute access and are never cached.
 */

#pragma once

#include "script/ScriptContext.h"

namespace neurus
{
namespace script
{

/**
 * @brief Common members of all scene proxies.
 *
 * A later task adds the write-dispatch pointer here; phase 1 only reads.
 */
struct ProxyBase
{
	const ScriptContext* ctx = nullptr;  ///< Context of the current run, or nullptr.
	int uid = 0;                         ///< Scene object UID; 0 when unused.
};

}  // namespace script
}  // namespace neurus
