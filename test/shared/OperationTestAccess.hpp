#pragma once

#include "editor/operations/OperationContext.hpp"

#include <type_traits>

namespace neurus {

// Production callers replay through OperationManager. These tests exercise the
// event emitted by a single operation independently of the history stacks.
struct OperationTestAccess
{
	static OperationContext Context(IEventQueue& events) { return OperationContext(events); }
};

static_assert(!std::is_constructible_v<OperationContext, IEventQueue&>);
static_assert(!std::is_constructible_v<OperationContext, IEventQueue&, bool>);
static_assert(!std::is_copy_constructible_v<OperationContext>);

} // namespace neurus
