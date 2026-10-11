/**
 * @file ShaderOperations.h
 * @brief Undoable operations for per-mesh shader content edits (delta-only).
 *
 * Shader content edits are per-object state, so these ops are UID-keyed like the
 * TransitionOp family. Rather than snapshotting the whole stage source, each op
 * carries only the delta for one edit dimension, keeping the serialized history
 * compact.
 *
 * Replay dispatches a dedicated restore event so mutation stays on the single
 * controller path and the version bumps only on undo/redo (forward live edits
 * skip the bump to avoid a panel reload mid-typing). Replay restores CPU-side
 * source only; the user presses Compile to push to the GPU.
 *
 * Edits are deliberately NOT mergeable: a code-edit burst is bracketed by the
 * controller's edit gesture and recorded once on focus-out.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "editor/events/ShaderEvents.h"
#include "editor/operations/Operation.h"
#include "render/shaders/ShaderStructSerialize.h"

namespace neurus {

/**
 * @brief Undoable edit of one shader stage's GLSL code text (before -> after).
 *
 * Carries only the two text endpoints; no parsed IR. Replays by dispatching
 * ShaderCodeRestored, which overwrites ShaderUnit::code and bumps the version.
 */
class SetShaderCodeOp : public Operation
{
public:
	SetShaderCodeOp() = default;

	SetShaderCodeOp(int uid, int stage, std::string beforeCode, std::string afterCode)
		: m_uid(uid)
		, m_stage(stage)
		, m_beforeCode(std::move(beforeCode))
		, m_afterCode(std::move(afterCode))
	{}

	void Apply(OperationContext& ctx) override
	{
		// Replayed event carries the mesh UID; ShaderController resolves it
		// against the current scene and no-ops stale ids.
		ctx.events.emitNow(ShaderCodeRestored{ m_uid, m_stage, m_afterCode });
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SetShaderCodeOp>(m_uid, m_stage, m_afterCode, m_beforeCode);
	}

	std::string Label() const override { return "Edit Shader Code"; }

	/** @brief Serializes {uid, stage, beforeCode, afterCode}. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("uid", m_uid),
		   cereal::make_nvp("stage", m_stage),
		   cereal::make_nvp("beforeCode", m_beforeCode),
		   cereal::make_nvp("afterCode", m_afterCode));
	}

private:
	int         m_uid = 0;     ///< Target mesh UID (serialized).
	int         m_stage = 0;   ///< ShaderType as int (serialized).
	std::string m_beforeCode;  ///< GLSL text before the edit (serialized).
	std::string m_afterCode;   ///< GLSL text after the edit (serialized).
};

/**
 * @brief Undoable edit of one ShaderStruct element (before -> after).
 *
 * Identifies the section + entry index and carries the whole addressed element
 * at both endpoints. Replays by dispatching ShaderFieldRestored, which assigns
 * the element back into the live IR and bumps the version. Storing the whole
 * element keeps inversion a plain endpoint swap.
 */
class SetShaderFieldOp : public Operation
{
public:
	SetShaderFieldOp() = default;

	SetShaderFieldOp(int uid, int stage, ShaderSection section, int fieldIndex,
	                 ShaderFieldValue before, ShaderFieldValue after)
		: m_uid(uid)
		, m_stage(stage)
		, m_section(section)
		, m_fieldIndex(fieldIndex)
		, m_before(std::move(before))
		, m_after(std::move(after))
	{}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.emitNow(ShaderFieldRestored{
			m_uid, m_stage, m_section, m_fieldIndex, m_after });
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SetShaderFieldOp>(
			m_uid, m_stage, m_section, m_fieldIndex, m_after, m_before);
	}

	std::string Label() const override { return "Edit Shader Field"; }

	/** @brief Serializes {uid, stage, section, fieldIndex, before, after}. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("uid", m_uid),
		   cereal::make_nvp("stage", m_stage),
		   cereal::make_nvp("section", m_section),
		   cereal::make_nvp("fieldIndex", m_fieldIndex),
		   cereal::make_nvp("before", m_before),
		   cereal::make_nvp("after", m_after));
	}

private:
	int              m_uid = 0;        ///< Target mesh UID (serialized).
	int              m_stage = 0;      ///< ShaderType as int (serialized).
	ShaderSection    m_section{};      ///< Which ShaderStruct container (serialized).
	int              m_fieldIndex = 0; ///< Index into the section's vector (serialized).
	ShaderFieldValue m_before;         ///< Whole element before the edit (serialized).
	ShaderFieldValue m_after;          ///< Whole element after the edit (serialized).
};

/**
 * @brief Undoable append of one default entry to a ShaderStruct container.
 *
 * Carries the section + sub-index and a direction flag. Because undo/redo is
 * strictly LIFO, the added entry is always the last element, so undo simply
 * removes the last entry and redo re-appends an identical default. Inverse
 * flips the flag, making the op its own logical inverse.
 */
class AddShaderFieldOp : public Operation
{
public:
	AddShaderFieldOp() = default;

	AddShaderFieldOp(int uid, int stage, ShaderSection section, int subFieldIndex, bool add)
		: m_uid(uid)
		, m_stage(stage)
		, m_section(section)
		, m_subFieldIndex(subFieldIndex)
		, m_add(add)
	{}

	void Apply(OperationContext& ctx) override
	{
		if (m_add)
			ctx.events.emitNow(ShaderFieldAddRestored{ m_uid, m_stage, m_section, m_subFieldIndex });
		else
			ctx.events.emitNow(ShaderFieldRemoved{ m_uid, m_stage, m_section, m_subFieldIndex });
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<AddShaderFieldOp>(m_uid, m_stage, m_section, m_subFieldIndex, !m_add);
	}

	std::string Label() const override { return "Add Shader Field"; }

	/** @brief Serializes {uid, stage, section, subFieldIndex, add}. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("uid", m_uid),
		   cereal::make_nvp("stage", m_stage),
		   cereal::make_nvp("section", m_section),
		   cereal::make_nvp("subFieldIndex", m_subFieldIndex),
		   cereal::make_nvp("add", m_add));
	}

private:
	int           m_uid = 0;             ///< Target mesh UID (serialized).
	int           m_stage = 0;           ///< ShaderType as int (serialized).
	ShaderSection m_section{};           ///< Which ShaderStruct container (serialized).
	int           m_subFieldIndex = -1;  ///< StructDefs target struct-def index (-1 = top-level) (serialized).
	bool          m_add = true;          ///< true = append default, false = remove last (serialized).
};

/**
 * @brief Undoable Create Shader - a pool-preserving membership toggle.
 *
 * Stores the target mesh UID + the pooled RenderShader UID + a direction flag.
 * Apply() dispatches a dedicated restore event so replay NEVER re-enters the
 * forward create handler (which would reload, re-parse, recompile and mint a NEW
 * pooled shader). The pooled shader is never removed - undo only drops the
 * mesh's reference, so redo relinks the SAME object.
 */
class ShaderLinkOp : public Operation
{
public:
	ShaderLinkOp() = default;

	/**
	 * @brief Constructs a link/unlink toggle over a mesh's pooled shader.
	 * @param uid Target mesh UID.
	 * @param shaderId Pooled RenderShader UID to link (unlink ignores it).
	 * @param add true = link (redo), false = unlink (undo).
	 */
	ShaderLinkOp(int uid, int shaderId, bool add)
		: m_uid(uid)
		, m_shaderId(shaderId)
		, m_add(add)
	{}

	void Apply(OperationContext& ctx) override
	{
		// Replayed events carry the mesh UID + pooled shader UID; the Editor
		// resolves them and no-ops stale ids.
		if (m_add)
			ctx.events.emitNow(ShaderLinkRestored{ m_uid, m_shaderId });
		else
			ctx.events.emitNow(ShaderUnlinkRestored{ m_uid });
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<ShaderLinkOp>(m_uid, m_shaderId, !m_add);
	}

	std::string Label() const override
	{
		return m_add ? "Create Shader" : "Remove Shader";
	}

	/** @brief Serializes {uid, shaderId, add}. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("uid", m_uid),
		   cereal::make_nvp("shaderId", m_shaderId),
		   cereal::make_nvp("add", m_add));
	}

private:
	int  m_uid = 0;      ///< Target mesh UID (serialized).
	int  m_shaderId = 0; ///< Pooled RenderShader UID to link/unlink (serialized).
	bool m_add = false;  ///< true = link; false = unlink (serialized).
};

} // namespace neurus
