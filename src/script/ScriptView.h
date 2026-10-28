/**
 * @file ScriptView.h
 * @brief The per-interpreter view a proxy is constructed with.
 *
 * A `ScriptView` is a plain pair of engine-owned pointers with no behaviour: it
 * deliberately defines no accessor, so each proxy decides for itself what it
 * needs and how to read it. The view's address is stable for the interpreter's
 * lifetime (it is a member of `ScriptEngine::Impl`), which is what lets a proxy
 * store a pointer to it without owning anything.
 */

#pragma once

namespace neurus
{
struct ScriptContext;
class ScriptDispatch;

namespace script
{

/**
 * @brief Engine-owned state handed to every view and proxy at construction.
 *
 * Non-owning. In the engine's own view `ctx` points at the document published
 * for the run, which is an empty context between runs, and `dispatch` points at
 * the mutation funnel, whose `Emit` raises until a queue is attached.
 */
struct ScriptView
{
	const ScriptContext*  ctx = nullptr;       ///< Document view: scene, config and resource lookup.
	const ScriptDispatch* dispatch = nullptr;  ///< Mutation funnel.
};

}  // namespace script
}  // namespace neurus
