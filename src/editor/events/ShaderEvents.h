/**
 * @file ShaderEvents.h
 * @brief Event structs for the shader editor pipeline (UI -> Editor -> ShaderController).
 *
 * All events carry an int objectUid (the target mesh's UID); the controller
 * resolves it against the current scene and operates on the shader data.
 * Version flow: only create and compile bump the shader version, and only on a
 * successful compile - code and struct edits need an explicit Compile.
 */

#pragma once

#include <string>
#include <variant>

#include "render/shaders/ShaderStruct.h"

namespace neurus {

/**
 * @brief One ShaderStruct element payload (the granularity of an undoable edit).
 *
 * An edit captures the whole addressed element before and after, so undo/redo
 * restores it by plain assignment. The ShaderSection still discriminates which
 * container the element belongs to (multiple sections share an element type).
 */
using ShaderFieldValue = std::variant<S_IO, S_Uniform, S_Func, S_PushConstant, S_StructDef>;

/**
 * @brief Identifies which ShaderStruct container is being edited.
 *
 * Maps 1:1 to the public vectors in ShaderStruct.
 */
enum class ShaderSection : int
{
	Attributes    = 0,  ///< AB_list          (S_IO)          - vertex attributes
	PassOutputs   = 1,  ///< pass_list        (S_IO)          - render-pass outputs
	Inputs        = 2,  ///< input_list       (S_Uniform)     - shader-stage inputs
	Outputs       = 3,  ///< output_list      (S_Uniform)     - shader-stage outputs
	Uniforms      = 4,  ///< uniform_list     (S_Uniform)     - single-value uniforms
	StructDefs    = 5,  ///< struct_def_list  (S_StructDef)   - bare struct definitions
	Functions     = 6,  ///< func_list        (S_Func)        - user-defined functions
	PushConstants = 7,  ///< push_constants   (S_PushConstant)- push-constant members
};

/**
 * @brief Emitted when the user clicks "Create Shader" for a mesh with no shader.
 *
 * Handled by the Editor (which owns the pool): it loads and links a pooled
 * RenderShader, compiles it, and records a ShaderLinkOp so the create is
 * undoable. The ShaderController does NOT handle this event.
 */
struct ShaderCreateRequested
{
	int objectUid = 0;
};

/**
 * @brief Emitted when the user edits the GLSL code text for a stage.
 *
 * Updates the unit's code in-place and does NOT bump the version.
 */
struct ShaderCodeEdited
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int (0=VERTEX, 1=FRAGMENT)
	std::string code;         ///< New GLSL source text
};

/**
 * @brief Emitted when the user edits a single field in a ShaderStruct container.
 *
 * Identifies the section, the entry index, the property name and the new value.
 * For StructDefs, subFieldIndex identifies the member field (-1 = the struct
 * definition itself). Does NOT bump the version.
 */
struct ShaderStructEdited
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
	ShaderSection section;    ///< Which ShaderStruct container
	int fieldIndex = 0;       ///< Index into the section's vector
	int subFieldIndex = -1;   ///< For StructDefs: which member field (-1 = struct itself)
	std::string field;        ///< Property name ("type", "name", "value", "body", ...)
	std::string value;        ///< New value as string
};

/**
 * @brief Emitted when the user clicks the "+" add button on a struct section.
 *
 * Appends a new default entry to the specified container; subFieldIndex
 * identifies which struct definition to add a member to (-1 = a new struct
 * definition). Does NOT bump the version.
 */
struct ShaderFieldAdded
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
	ShaderSection section;    ///< Which ShaderStruct container to append to
	int subFieldIndex = -1;   ///< For StructDefs: which struct def to add a member to
};

/**
 * @brief Emitted when the user clicks "Compile" to compile GLSL -> SPIR-V.
 *
 * Compiles the specified stage and bumps the shader version on success, so the
 * pipeline rebuilds on the next frame. unitType: 0 = compile from code
 * (re-parse then generate), 1 = compile from struct IR.
 */
struct ShaderCompileRequested
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
	int unitType = 0;         ///< 0 = Code path, 1 = Struct path
};

/**
 * @brief Emitted when a bounded shader code-edit gesture begins (editor focus-in).
 *
 * The controller captures the "before" source for the (object, stage) and
 * applies live keystrokes without recording, so the whole burst collapses to
 * one undo entry committed on ShaderEditEnd.
 */
struct ShaderEditBegin
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
};

/**
 * @brief Emitted when a bounded shader code-edit gesture ends (editor focus-out).
 *
 * The controller records a single SetShaderCodeOp spanning the code captured at
 * ShaderEditBegin through the current code, if it changed.
 */
struct ShaderEditEnd
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
};

// ---------------------------------------------------------------------------
// Undo/redo restore events (replayed by the shader operations)
//
// These mirror the forward edit events 1:1, but are distinct types so replay can
// bump the unit version (refreshing the panel) while forward live edits
// deliberately do not. All are CPU-only: they restore editable source without
// recompiling.
// ---------------------------------------------------------------------------

/**
 * @brief Restores a stage's GLSL code text (replayed by SetShaderCodeOp).
 *
 * Overwrites the unit's code and bumps the version so the editor refreshes.
 */
struct ShaderCodeRestored
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
	std::string code;         ///< Absolute GLSL text to restore
};

/**
 * @brief Restores one ShaderStruct element (replayed by SetShaderFieldOp).
 *
 * Carries the whole addressed element; the controller assigns it into the
 * section's vector and bumps the version. Whole-element assignment makes both
 * undo and redo a plain copy.
 */
struct ShaderFieldRestored
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
	ShaderSection section;    ///< Which ShaderStruct container
	int fieldIndex = 0;       ///< Index into the section's vector
	ShaderFieldValue value;   ///< Whole element to assign back
};

/**
 * @brief Re-appends a default entry to a ShaderStruct container (redo of an add).
 *
 * The controller appends the same default entry and bumps the version.
 */
struct ShaderFieldAddRestored
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
	ShaderSection section;    ///< Which ShaderStruct container to append to
	int subFieldIndex = -1;   ///< For StructDefs: which struct def to add a member to
};

/**
 * @brief Removes the last entry from a ShaderStruct container (undo of an add).
 *
 * Because undo/redo is strictly LIFO, the added entry is always the last one,
 * so dropping the last element is safe.
 */
struct ShaderFieldRemoved
{
	int objectUid = 0;
	int stage = 0;            ///< ShaderType as int
	ShaderSection section;    ///< Which ShaderStruct container to remove from
	int subFieldIndex = -1;   ///< For StructDefs: which struct def to remove a member from
};

/**
 * @brief Relinks a mesh to its pooled shader by UID (redo of Create Shader).
 *
 * The Editor resolves the pooled RenderShader by UID and links it - no reload,
 * no recompile, no new pooled object. The shader panel refresh is driven by the
 * version dirty check, and the per-mesh pipeline cache is keyed by
 * (objectId, shader version), so the relinked shader reuses its pipeline.
 */
struct ShaderLinkRestored
{
	int objectUid = 0;   ///< Mesh UID to relink to its pooled shader.
	int shaderId = 0;    ///< Pooled RenderShader UID to relink.
};

/**
 * @brief Drops a mesh's shader reference (undo of Create Shader).
 *
 * The pooled RenderShader stays in the pool for redo, and the mesh falls back
 * to the default pass shader.
 */
struct ShaderUnlinkRestored
{
	int objectUid = 0;   ///< Mesh UID whose shader reference is dropped.
};

} // namespace neurus