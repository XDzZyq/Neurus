/**
 * @file Operation.h
 * @brief Abstract undoable operation (ID-based inverse descriptor).
 *
 * An Operation is a value-only description of a scene edit: Apply() dispatches
 * this operation's absolute-set event synchronously, so the existing controller
 * handler performs the actual mutation. Operations carry only integer UIDs plus
 * absolute before/after values - no raw pointers - so they survive scene
 * mutations, serialize trivially, and no-op when the object is gone.
 *
 * Undo/redo is group-theoretic: the undo stack holds the forward operation `g`;
 * undo emits `g.Inverse()` and pushes the inverse to redo; redo emits its
 * inverse. Inverse() must therefore be an involution.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/vector.hpp>

#include "editor/operations/OperationContext.h"

namespace neurus {

/**
 * @brief Base class for all undoable operations.
 */
class Operation
{
public:
	virtual ~Operation() = default;

	/**
	 * @brief Dispatches this operation's absolute-set event synchronously.
	 * @param ctx Replay context (event queue for synchronous dispatch).
	 * @note Emits an event carrying integer object UIDs; the receiving
	 *       controller handler resolves them and no-ops stale ids.
	 */
	virtual void Apply(OperationContext& ctx) = 0;

	/**
	 * @brief Returns a fresh operation with before/after values swapped.
	 * @return Owning pointer to the inverse operation.
	 * @note Involution: Inverse()->Inverse() must equal the original edit.
	 */
	virtual std::unique_ptr<Operation> Inverse() const = 0;

	/**
	 * @brief Human-readable label (for Edit menu / debugging).
	 */
	virtual std::string Label() const = 0;

	/**
	 * @brief Coalescing key for merging consecutive edits into one history entry.
	 * @return Non-empty key for mergeable ops, empty (default) for standalone ops.
	 * @note Two ops merge only when their keys are equal and non-empty; encode the
	 *       target UID in the key so edits to different objects never coalesce.
	 */
	virtual std::string MergeKey() const { return {}; }

	/**
	 * @brief Folds a newer same-key op's end state into this op (extends the range).
	 * @param newer The just-submitted op that shares this op's MergeKey().
	 */
	virtual void MergeFrom(const Operation& newer) { (void)newer; }

	/**
	 * @brief Whether recording this op should keep the redo stack intact.
	 * @return false (default) for edits that branch history and clear redo;
	 *         true for "transparent" ops that append to undo without discarding
	 *         a pending redo chain.
	 *
	 * Safe only because operations are absolute state-sets, not deltas: a
	 * preserved redo op replays to its stored end state regardless of any
	 * transparent ops recorded in between. Selection changes use this.
	 */
	virtual bool PreservesRedo() const { return false; }
};

/**
 * @brief Generic base for reversible before/after value edits (a "transition").
 *
 * Stores a target UID plus *absolute* before/after values (endpoints, not
 * deltas), and replays by dispatching the scene event the UI would emit. A
 * concrete subclass only declares how a stored value becomes its event via
 * `MakeEvent()` and a static `kLabel`.
 *
 * CRTP lets Inverse() reconstruct the *concrete* subclass with swapped values,
 * so an operation on the redo stack has the same type as a freshly recorded one.
 *
 * @tparam Derived Concrete subclass (CRTP); must provide
 *                 `TEvent MakeEvent(int objectUid, const Value&) const` and
 *                 `static constexpr const char* kLabel`.
 * @tparam TEvent  Scene event struct dispatched on replay.
 * @tparam Value   Stored value type (float, bool, glm::vec3, ...).
 */
template<typename Derived, typename TEvent, typename Value>
class TransitionOp : public Operation
{
public:
	TransitionOp() = default;

	TransitionOp(int uid, Value before, Value after)
		: m_uid(uid)
		, m_before(std::move(before))
		, m_after(std::move(after))
	{}

	void Apply(OperationContext& ctx) override
	{
		// The replayed event carries the target UID; the controller handler
		// resolves it and no-ops stale ids.
		ctx.events.Emit(static_cast<const Derived*>(this)->MakeEvent(m_uid, m_after));
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		// Swap before/after and rebuild the concrete type - involution holds.
		return std::make_unique<Derived>(m_uid, m_after, m_before);
	}

	std::string Label() const override { return Derived::kLabel; }

	/**
	 * @brief Adopts @p newer's "after" value, keeping this op's original "before".
	 */
	void MergeFrom(const Operation& newer) override
	{
		m_after = static_cast<const TransitionOp&>(newer).m_after;
	}

	/**
	 * @brief Serializes {uid, before, after}; Value must be cereal-serializable.
	 * @note Templated so cereal's polymorphic machinery can dispatch to the
	 *       registered concrete type; Derived must be default-constructible.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("uid", m_uid),
		   cereal::make_nvp("before", m_before),
		   cereal::make_nvp("after", m_after));
	}

protected:
	int m_uid = 0;      ///< Target object UID (serialized).
	Value m_before{};   ///< Value before the edit (serialized).
	Value m_after{};    ///< Value after the edit (serialized).
};

/**
 * @brief Composes a sequence of operations into a single undo entry.
 *
 * Apply() replays the sequence in forward order; Inverse() returns a composite
 * of the reversed, individually-inverted operations, so inversion is an
 * involution. Used wherever one user gesture spans several primitive edits that
 * must collapse to one history entry (add + select, delete + clear).
 */
class CompositeOp : public Operation
{
public:
	CompositeOp() = default;

	/**
	 * @brief Constructs a composite from a forward-order sequence.
	 * @param ops Primitive operations, applied in order on redo.
	 */
	explicit CompositeOp(std::vector<std::unique_ptr<Operation>> ops)
		: m_ops(std::move(ops))
	{}

	void Apply(OperationContext& ctx) override
	{
		for (const auto& op : m_ops)
			op->Apply(ctx);
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<CompositeOp>(InvertedSequence());
	}

	std::string Label() const override
	{
		return m_ops.empty() ? "Composite" : m_ops.front()->Label();
	}

	/**
	 * @brief Serializes the contained operation sequence polymorphically.
	 * @note All contained concrete types must be registered; every contained type
	 *       must be default-constructible.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(m_ops));
	}

protected:
	/**
	 * @brief Reversed, individually-inverted copy of the sequence.
	 *
	 * The shared inversion step for every composite: a plain CompositeOp hands it
	 * back as another CompositeOp, while a ScriptRunOp hands it to another
	 * ScriptRunOp, so each type keeps its own label across undo and redo.
	 */
	std::vector<std::unique_ptr<Operation>> InvertedSequence() const
	{
		std::vector<std::unique_ptr<Operation>> inv;
		inv.reserve(m_ops.size());
		for (auto it = m_ops.rbegin(); it != m_ops.rend(); ++it)
			inv.push_back((*it)->Inverse());
		return inv;
	}

private:
	std::vector<std::unique_ptr<Operation>> m_ops; ///< Forward-order primitive sequence.
};

/**
 * @brief One script run's edits, labelled as a single "Script Run" entry.
 *
 * Same replay semantics as CompositeOp: Apply() runs the edits in forward order
 * and Inverse() returns the reversed, individually-inverted sequence. The distinct
 * type carries the distinct label, so the History panel names the run instead of
 * its first child (for example "Move"). Registered with cereal, so the label
 * survives a project save and reload.
 */
class ScriptRunOp : public CompositeOp
{
public:
	ScriptRunOp() = default;
	explicit ScriptRunOp(std::vector<std::unique_ptr<Operation>> ops);

	std::unique_ptr<Operation> Inverse() const override;
	std::string Label() const override { return "Script Run"; }

	template<class Archive>
	void serialize(Archive& ar)
	{
		CompositeOp::serialize(ar);
	}
};

/// @brief Constructs a script run from its forward-order edits.
inline ScriptRunOp::ScriptRunOp(std::vector<std::unique_ptr<Operation>> ops)
	: CompositeOp(std::move(ops))
{}

/**
 * @brief Inverts as another ScriptRunOp, so the redo entry keeps the label.
 */
inline std::unique_ptr<Operation> ScriptRunOp::Inverse() const
{
	return std::make_unique<ScriptRunOp>(InvertedSequence());
}

} // namespace neurus
