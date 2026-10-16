/**
 * @file ShaderController.cpp
 * @brief Event-driven shader lifecycle: create, compile, code edit, struct edit.
 *
 * All handlers are free functions; each resolves an integer UID against the
 * current scene and mutates the shader data. Pure shader logic - no renderer or
 * upload dependency.
 */

#include "editor/controllers/ShaderController.h"
#include "editor/events/ShaderEvents.h"
#include "editor/events/EditorEvents.h"
#include "editor/events/EventBus.h"
#include "editor/operations/IOperationSink.h"
#include "editor/operations/ShaderOperations.h"

#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/ObjectID.h"

#include "render/shaders/Shader.h"
#include "render/shaders/ShaderUnit.h"
#include "render/shaders/ShaderLibrary.h"
#include "render/shaders/ShaderGenerator.h"
#include "render/shaders/ShaderParser.h"
#include "render/shaders/RenderShader.h"

#include "core/Log.h"

#include <algorithm>
#include <memory>
#include <string>
#include <variant>

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

/**
 * @brief Resolves an event's object UID to its live Mesh (via the scene).
 * @return Non-owning Mesh*, or nullptr if the id is stale or not a mesh.
 */
neurus::Mesh* ResolveMesh(const neurus::ControllerContext& ctx, int objectUid)
{
	neurus::Scene* scene = ctx.scene();
	if (!scene) return nullptr;
	auto it = scene->mesh_list.find(objectUid);
	return it == scene->mesh_list.end() ? nullptr : it->second.get();
}

/**
 * @brief Resolves a mesh's (stage) pair to its live ShaderUnit.
 * @return Pointer to the stage's ShaderUnit, or nullptr if the mesh has no
 *         shader or lacks the requested stage.
 */
neurus::ShaderUnit* GetStageUnit(neurus::Mesh* mesh, int stage)
{
	if (!mesh || !mesh->o_shader) return nullptr;
	auto type = static_cast<neurus::ShaderType>(stage);
	if (!mesh->o_shader->HasStage(type)) return nullptr;
	return &mesh->o_shader->GetStage(type);
}

/**
 * @brief Reads a stage's current GLSL code text.
 * @return true on success, false if the stage could not be resolved.
 */
bool SnapshotCode(neurus::Mesh* mesh, int stage, std::string& out)
{
	neurus::ShaderUnit* unit = GetStageUnit(mesh, stage);
	if (!unit) return false;
	out = unit->code;
	return true;
}

// ---------------------------------------------------------------------------
// ShaderStruct element access - shared by the forward edit path and undo replay
// ---------------------------------------------------------------------------

/**
 * @brief Applies a functor to the element addressed by (section, fieldIndex).
 *
 * Each section maps to exactly one ShaderStruct vector, and @p fn is invoked with
 * a reference to the element at @p fieldIndex (its concrete type deduced per
 * section). StructDefs addresses the struct entry itself; member-field edits go
 * through the sub-index.
 *
 * @return true if the element exists (fn was called), false if out of range.
 */
template<class Fn>
bool VisitElement(neurus::ShaderStruct& parsed, neurus::ShaderSection section,
                  int fieldIndex, Fn&& fn)
{
	auto visit = [&](auto& list) -> bool {
		if (fieldIndex < 0 || fieldIndex >= static_cast<int>(list.size())) return false;
		fn(list[fieldIndex]);
		return true;
	};

	switch (section)
	{
	case neurus::ShaderSection::Attributes:    return visit(parsed.AB_list);
	case neurus::ShaderSection::PassOutputs:   return visit(parsed.pass_list);
	case neurus::ShaderSection::Inputs:        return visit(parsed.input_list);
	case neurus::ShaderSection::Outputs:       return visit(parsed.output_list);
	case neurus::ShaderSection::Uniforms:      return visit(parsed.uniform_list);
	case neurus::ShaderSection::Functions:     return visit(parsed.func_list);
	case neurus::ShaderSection::PushConstants: return visit(parsed.push_constants);
	case neurus::ShaderSection::StructDefs:    return visit(parsed.struct_def_list);
	}
	return false;
}

// ---------------------------------------------------------------------------
// ApplyFieldEdit: write the UI's single {field, value} onto a live element.
//
// One small overload per element type translates a property name + string value
// onto its members. Unknown field names are ignored (the caller's before==after
// diff then skips the edit); type fields update the paired ParaType enum too.
// ---------------------------------------------------------------------------

/** @brief Edits an S_IO element (attribute / pass output / struct member). */
void ApplyFieldEdit(neurus::S_IO& io, int /*subFieldIndex*/,
                    const std::string& field, const std::string& value)
{
	if (field == "type")
	{
		io.type = neurus::ShaderStruct::ParseType(value);
		io.typeName = value;
	}
	else if (field == "name")
	{
		io.name = value;
	}
}

/** @brief Edits an S_Uniform element (uniform / input / output variable). */
void ApplyFieldEdit(neurus::S_Uniform& u, int /*subFieldIndex*/,
                    const std::string& field, const std::string& value)
{
	if (field == "type")
	{
		u.type = neurus::ShaderStruct::ParseType(value);
		u.actualType = value;
	}
	else if (field == "name")
	{
		u.name = value;
	}
}

/** @brief Edits an S_Func element (function / const definition). */
void ApplyFieldEdit(neurus::S_Func& f, int /*subFieldIndex*/,
                    const std::string& field, const std::string& value)
{
	if (field == "name") f.name = value;
	else if (field == "type") f.returnType = neurus::ShaderStruct::ParseType(value);
}

/** @brief Edits an S_PushConstant element (push-constant block member). */
void ApplyFieldEdit(neurus::S_PushConstant& pc, int /*subFieldIndex*/,
                    const std::string& field, const std::string& value)
{
	if (field == "name") pc.name = value;
	else if (field == "type") pc.typeName = value;
}

/**
 * @brief Edits an S_StructDef element (struct definition or one of its members).
 *
 * subFieldIndex < 0 edits the struct definition itself (name / varName);
 * subFieldIndex >= 0 edits member field [subFieldIndex].
 */
void ApplyFieldEdit(neurus::S_StructDef& sd, int subFieldIndex,
                    const std::string& field, const std::string& value)
{
	if (subFieldIndex < 0)
	{
		if (field == "name") sd.name = value;
		else if (field == "type") sd.varName = value;
	}
	else if (subFieldIndex < static_cast<int>(sd.fields.size()))
	{
		ApplyFieldEdit(sd.fields[subFieldIndex], -1, field, value);
	}
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

/**
 * @brief Handles ShaderCreateRequested: loads default gbuffer shaders for a mesh.
 *
 * Loads, parses/generates, assigns to the mesh, compiles both stages to SPIR-V
 * and bumps the version on success.
 */
void OnCreateShader(const neurus::ShaderCreateRequested& e, const neurus::ControllerContext& ctx)
{
	auto* mesh = ResolveMesh(ctx, e.objectUid);
	if (!mesh)
	{
		NEURUS_ERR("[ShaderController] OnCreateShader: not a mesh");
		return;
	}

	if (mesh->o_shader)
	{
		NEURUS_LOG("[ShaderController] Mesh already has a shader");
		return;
	}

	const int objectId = mesh->GetObjectID();
	const std::string shaderName = "MeshShader_" + std::to_string(objectId);
	const std::string vertPath = "res/shaders/render/gbuffer.vert";
	const std::string fragPath = "res/shaders/render/gbuffer.frag";

	try
	{
		auto shader = neurus::ShaderLibrary::LoadRenderShader(shaderName, vertPath, fragPath);
		if (!shader || !shader->ParseAndGenerate())
		{
			NEURUS_ERR("[ShaderController] Failed to load/parse default shader for mesh " << objectId);
			return;
		}

		mesh->o_shader = std::move(shader);

		// Compile both stages to SPIR-V and bump version on success
		auto& s = *mesh->o_shader;
		bool allOk = true;

		if (s.HasStage(neurus::ShaderType::VERTEX))
		{
			auto& unit = s.GetStage(neurus::ShaderType::VERTEX);
			unit.spv = neurus::ShaderLibrary::Compile(unit, neurus::ShaderType::VERTEX, s.GetName());
			if (unit.spv.empty()) { allOk = false; }
			else { unit.BumpVersion(); }
		}
		if (s.HasStage(neurus::ShaderType::FRAGMENT))
		{
			auto& unit = s.GetStage(neurus::ShaderType::FRAGMENT);
			unit.spv = neurus::ShaderLibrary::Compile(unit, neurus::ShaderType::FRAGMENT, s.GetName());
			if (unit.spv.empty()) { allOk = false; }
			else { unit.BumpVersion(); }
		}

		if (allOk)
		{
			s.BumpVersion();
			ctx.events.enqueue(neurus::ShaderCompiled{ s.GetObjectID() });
		}

		NEURUS_LOG("[ShaderController] Created shader for mesh " << objectId << ": " << shaderName);
	}
	catch (const std::exception& ex)
	{
		NEURUS_ERR("[ShaderController] Exception creating shader: " << ex.what());
	}
}

/**
 * @brief Handles ShaderCodeEdited: updates the stage's code in-place.
 *
 * Does NOT bump version: the user must press Compile to apply the change.
 */
void OnCodeEdited(const neurus::ShaderCodeEdited& e, const neurus::ControllerContext& ctx)
{
	auto* mesh = ResolveMesh(ctx, e.objectUid);
	if (!mesh || !mesh->o_shader) return;

	auto& shader = *mesh->o_shader;
	auto type = static_cast<neurus::ShaderType>(e.stage);

	if (!shader.HasStage(type)) return;

	auto& unit = shader.GetStage(type);
	if (unit.code == e.code) return;  // dirty-check

	unit.code = e.code;

	NEURUS_LOG("[ShaderController] Code updated for mesh " << mesh->GetObjectID()
	           << " (" << unit.code.size() << " bytes)");
}

/**
 * @brief Handles ShaderCompileRequested: compiles GLSL -> SPIR-V for a stage.
 *
 * unitType 0 parses the code into IR and regenerates GLSL first; unitType 1
 * regenerates from the existing IR. Bumps the unit and shader versions on success.
 */
void OnCompileShader(const neurus::ShaderCompileRequested& e, const neurus::ControllerContext& ctx)
{
	auto* mesh = ResolveMesh(ctx, e.objectUid);
	if (!mesh || !mesh->o_shader)
	{
		NEURUS_ERR("[ShaderController] OnCompileShader: no shader on mesh");
		return;
	}

	auto& shader = *mesh->o_shader;
	auto type = static_cast<neurus::ShaderType>(e.stage);

	if (!shader.HasStage(type)) return;
	auto& unit = shader.GetStage(type);

	try
	{
		if (e.unitType == 0)
		{
			// Code path: parse the code into IR, then regenerate GLSL
			unit.parsed = neurus::ShaderParser::ParseShaderCode(unit.code, type);
			unit.code = neurus::ShaderGenerator::Generate(unit.parsed);
		}
		else
		{
			// Struct path: regenerate GLSL from the existing IR
			unit.code = neurus::ShaderGenerator::Generate(unit.parsed);
		}

		// Compile to SPIR-V and store in the unit
		unit.spv = neurus::ShaderLibrary::Compile(unit, type, shader.GetName());
		if (unit.spv.empty())
		{
			NEURUS_ERR("[ShaderController] Compile failed for mesh " << mesh->GetObjectID());
			return;
		}
		unit.BumpVersion();
		shader.BumpVersion();

		// The pipeline cache is keyed on the shader, so hand the Editor its id to warm.
		ctx.events.enqueue(neurus::ShaderCompiled{ shader.GetObjectID() });

		NEURUS_LOG("[ShaderController] Compiled shader for mesh " << mesh->GetObjectID()
		           << " (unitType=" << e.unitType << ")");
	}
	catch (const std::exception& ex)
	{
		NEURUS_ERR("[ShaderController] Compile failed: " << ex.what());
	}
}

/**
 * @brief Generates a unique name by appending a counter if the base name exists.
 */
std::string UniqueName(const std::string& base, const std::vector<std::string>& existing)
{
	auto exists = [&](const std::string& n) {
		return std::find(existing.begin(), existing.end(), n) != existing.end();
	};
	if (!exists(base)) return base;
	for (int i = 2; ; ++i)
	{
		std::string candidate = base + "_" + std::to_string(i);
		if (!exists(candidate)) return candidate;
	}
}

/**
 * @brief Appends a new default entry to a ShaderStruct container.
 *
 * Dispatches on ShaderSection to append a default-constructed entry to the
 * correct vector; StructDefs with subFieldIndex >= 0 appends a member field.
 * Generated GLSL is refreshed only on the next compile.
 *
 * @return true if an entry was actually appended, false otherwise.
 */
bool AppendDefault(neurus::ShaderStruct& parsed, neurus::ShaderSection section, int subFieldIndex)
{
	bool changed = false;

	switch (section)
	{
	case neurus::ShaderSection::Attributes:
	{
		std::vector<std::string> names;
		for (const auto& io : parsed.AB_list) names.push_back(io.name);
		parsed.AB_list.push_back({static_cast<int>(parsed.AB_list.size()),
		                          UniqueName("new_attr", names), neurus::ParaType::FLOAT, "", neurus::Interp::Smooth});
		changed = true;
		break;
	}
	case neurus::ShaderSection::PassOutputs:
	{
		std::vector<std::string> names;
		for (const auto& io : parsed.pass_list) names.push_back(io.name);
		parsed.pass_list.push_back({static_cast<int>(parsed.pass_list.size()),
		                            UniqueName("new_output", names), neurus::ParaType::FLOAT, "", neurus::Interp::Smooth});
		changed = true;
		break;
	}
	case neurus::ShaderSection::Inputs:
	{
		std::vector<std::string> names;
		for (const auto& u : parsed.input_list) names.push_back(u.name);
		parsed.input_list.push_back({UniqueName("new_input", names), neurus::ParaType::FLOAT, 1, -1, "", ""});
		changed = true;
		break;
	}
	case neurus::ShaderSection::Outputs:
	{
		std::vector<std::string> names;
		for (const auto& u : parsed.output_list) names.push_back(u.name);
		parsed.output_list.push_back({UniqueName("new_output", names), neurus::ParaType::FLOAT, 1, -1, "", ""});
		changed = true;
		break;
	}
	case neurus::ShaderSection::Uniforms:
	{
		std::vector<std::string> names;
		for (const auto& u : parsed.uniform_list) names.push_back(u.name);
		parsed.uniform_list.push_back({UniqueName("new_uniform", names), neurus::ParaType::FLOAT, 1, -1, "", ""});
		changed = true;
		break;
	}
	case neurus::ShaderSection::Functions:
	{
		std::vector<std::string> names;
		for (const auto& f : parsed.func_list) names.push_back(f.name);
		parsed.func_list.push_back({neurus::ParaType::FLOAT, UniqueName("new_func", names), "", {}});
		changed = true;
		break;
	}
	case neurus::ShaderSection::PushConstants:
	{
		std::vector<std::string> names;
		for (const auto& pc : parsed.push_constants) names.push_back(pc.name);
		parsed.push_constants.push_back({UniqueName("new_pc", names), 0, 0, "float"});
		changed = true;
		break;
	}
	case neurus::ShaderSection::StructDefs:
		if (subFieldIndex < 0)
		{
			// Add a new struct definition
			std::vector<std::string> names;
			for (const auto& sd : parsed.struct_def_list) names.push_back(sd.name);
			parsed.struct_def_list.push_back({0, UniqueName("NewStruct", names), {}, ""});
			changed = true;
		}
		else if (subFieldIndex < static_cast<int>(parsed.struct_def_list.size()))
		{
			// Add a member field to the specified struct definition
			auto& fields = parsed.struct_def_list[subFieldIndex].fields;
			std::vector<std::string> names;
			for (const auto& mf : fields) names.push_back(mf.name);
			fields.push_back({0, UniqueName("new_field", names), neurus::ParaType::FLOAT, "", neurus::Interp::Smooth});
			changed = true;
		}
		break;
	}

	if (changed)
	{
		NEURUS_LOG("[ShaderController] Field added: section=" << static_cast<int>(section)
		           << " subIdx=" << subFieldIndex);
	}

	return changed;
}

/**
 * @brief Removes the last entry from a ShaderStruct container (undo of an add).
 *
 * Undo/redo is strictly LIFO, so the entry appended by AppendDefault is always
 * the last element and dropping the tail is a correct inverse.
 *
 * @return true if an entry was actually removed, false otherwise.
 */
bool RemoveLast(neurus::ShaderStruct& parsed, neurus::ShaderSection section, int subFieldIndex)
{
	auto popIfAny = [](auto& list) -> bool {
		if (list.empty()) return false;
		list.pop_back();
		return true;
	};

	bool changed = false;
	switch (section)
	{
	case neurus::ShaderSection::Attributes:   changed = popIfAny(parsed.AB_list); break;
	case neurus::ShaderSection::PassOutputs:  changed = popIfAny(parsed.pass_list); break;
	case neurus::ShaderSection::Inputs:       changed = popIfAny(parsed.input_list); break;
	case neurus::ShaderSection::Outputs:      changed = popIfAny(parsed.output_list); break;
	case neurus::ShaderSection::Uniforms:     changed = popIfAny(parsed.uniform_list); break;
	case neurus::ShaderSection::Functions:    changed = popIfAny(parsed.func_list); break;
	case neurus::ShaderSection::PushConstants: changed = popIfAny(parsed.push_constants); break;
	case neurus::ShaderSection::StructDefs:
		if (subFieldIndex < 0)
		{
			changed = popIfAny(parsed.struct_def_list);
		}
		else if (subFieldIndex < static_cast<int>(parsed.struct_def_list.size()))
		{
			changed = popIfAny(parsed.struct_def_list[subFieldIndex].fields);
		}
		break;
	}

	if (changed)
	{
		NEURUS_LOG("[ShaderController] Field removed: section=" << static_cast<int>(section)
		           << " subIdx=" << subFieldIndex);
	}

	return changed;
}

/**
 * @brief Restores a stage's GLSL code text (undo/redo replay).
 *
 * Bumps the unit version so the editor refreshes. CPU-only: no recompile.
 */
void OnCodeRestored(const neurus::ShaderCodeRestored& e, const neurus::ControllerContext& ctx)
{
	neurus::ShaderUnit* unit = GetStageUnit(ResolveMesh(ctx, e.objectUid), e.stage);
	if (!unit) return; // Stale identity: shader gone, safe no-op.
	unit->code = e.code;
	unit->BumpVersion();

	NEURUS_LOG("[ShaderController] Code restored (undo/redo) for stage " << e.stage
	           << " (" << unit->code.size() << " bytes)");
}

/**
 * @brief Restores one ShaderStruct element (undo/redo replay).
 *
 * Assigns the stored element back into the section's vector and bumps the unit
 * version so the editor refreshes. The variant's active alternative always
 * matches the section's element type, since the op captured it there.
 */
void OnFieldRestored(const neurus::ShaderFieldRestored& e, const neurus::ControllerContext& ctx)
{
	neurus::ShaderUnit* unit = GetStageUnit(ResolveMesh(ctx, e.objectUid), e.stage);
	if (!unit) return; // Stale identity: shader gone, safe no-op.

	bool applied = false;
	bool matched = VisitElement(unit->parsed, e.section, e.fieldIndex,
		[&](auto& elem) {
			using T = std::decay_t<decltype(elem)>;
			if (const T* v = std::get_if<T>(&e.value))
			{
				elem = *v;
				applied = true;
			}
			else
			{
				NEURUS_ERR("[ShaderController] Field restore skipped: variant type mismatch"
				           " section=" << static_cast<int>(e.section)
				           << " idx=" << e.fieldIndex);
			}
		});
	if (!matched)
	{
		NEURUS_ERR("[ShaderController] Field restore skipped: index out of range"
		           " section=" << static_cast<int>(e.section) << " idx=" << e.fieldIndex);
		return;
	}
	if (!applied) return;
	unit->BumpVersion();

	NEURUS_LOG("[ShaderController] Field restored (undo/redo): section="
	           << static_cast<int>(e.section) << " idx=" << e.fieldIndex);
}

/**
 * @brief Re-appends a default entry to a container (redo of an add).
 *
 * Bumps the ShaderUnit version so the editor panel refreshes.
 */
void OnFieldAddRestored(const neurus::ShaderFieldAddRestored& e, const neurus::ControllerContext& ctx)
{
	neurus::ShaderUnit* unit = GetStageUnit(ResolveMesh(ctx, e.objectUid), e.stage);
	if (!unit) return; // Stale identity: shader gone, safe no-op.
	if (!AppendDefault(unit->parsed, e.section, e.subFieldIndex))
	{
		NEURUS_ERR("[ShaderController] Field re-add skipped: section="
		           << static_cast<int>(e.section) << " subIdx=" << e.subFieldIndex);
		return;
	}
	unit->BumpVersion();
}

/**
 * @brief Removes the last entry from a container (undo of an add).
 *
 * Bumps the ShaderUnit version so the editor panel refreshes.
 */
void OnFieldRemoved(const neurus::ShaderFieldRemoved& e, const neurus::ControllerContext& ctx)
{
	neurus::ShaderUnit* unit = GetStageUnit(ResolveMesh(ctx, e.objectUid), e.stage);
	if (!unit) return; // Stale identity: shader gone, safe no-op.
	if (!RemoveLast(unit->parsed, e.section, e.subFieldIndex))
	{
		NEURUS_ERR("[ShaderController] Field remove skipped: container empty or out of range"
		           " section=" << static_cast<int>(e.section)
		           << " subIdx=" << e.subFieldIndex);
		return;
	}
	unit->BumpVersion();
}

} // anonymous namespace

namespace neurus {

// ---------------------------------------------------------------------------
// Init - subscribe to shader events
// ---------------------------------------------------------------------------

void ShaderController::Init(ControllerContext& ctx)
{
	// Shader CREATE is handled by the Editor (it constructs the pooled
	// RenderShader); this controller keeps only pool-free lifecycle handlers.
	// Compile bumps the shader version, so the pipeline rebuilds and temporal
	// accumulation must reset. These stay non-undoable.
	ctx.events.subscribe<ShaderCompileRequested>(
		[ctx](const ShaderCompileRequested& e) {
			OnCompileShader(e, ctx);
			ctx.events.enqueue(RenderResetEvent{});
		});

	// Code edits apply live; recording is bracketed by the ShaderEditBegin/End
	// gesture so a keystroke burst collapses to one SetShaderCodeOp on focus-out.
	ctx.events.subscribe<ShaderCodeEdited>(
		[ctx](const ShaderCodeEdited& e) {
			OnCodeEdited(e, ctx);
		});
	ctx.events.subscribe<ShaderEditBegin>(
		[this, ctx](const ShaderEditBegin& e) {
			std::string before;
			if (!SnapshotCode(ResolveMesh(ctx, e.objectUid), e.stage, before)) return;
			m_codeEditing = true;
			m_editObjectId = e.objectUid;
			m_editStage = e.stage;
			m_beforeCode = std::move(before);
		});
	ctx.events.subscribe<ShaderEditEnd>(
		[this, ctx](const ShaderEditEnd&) {
			if (!m_codeEditing) return;
			m_codeEditing = false;

			const int objectUid = m_editObjectId;
			const int stage = m_editStage;
			m_editObjectId = 0;

			neurus::Mesh* mesh = ResolveMesh(ctx, objectUid);
			std::string after;
			if (!SnapshotCode(mesh, stage, after)) return;
			if (after == m_beforeCode) return; // no net change -> no op

			ctx.ops.Submit(std::make_unique<SetShaderCodeOp>(
				mesh->GetObjectID(), stage, std::move(m_beforeCode), std::move(after)));
		});

	// Discrete struct edits: snapshot the element, apply the edit, diff, record.
	ctx.events.subscribe<ShaderStructEdited>(
		[ctx](const ShaderStructEdited& e) {
			ShaderUnit* unit = GetStageUnit(ResolveMesh(ctx, e.objectUid), e.stage);
			if (!unit) return;

			ShaderFieldValue before;
			ShaderFieldValue after;
			bool captured = VisitElement(unit->parsed, e.section, e.fieldIndex,
				[&](auto& elem) {
					before = elem;
					ApplyFieldEdit(elem, e.subFieldIndex, e.field, e.value);
					after = elem;
				});
			if (!captured) return;
			if (before == after) return; // no net change -> no op

			NEURUS_LOG("[ShaderController] Struct edited: section=" << static_cast<int>(e.section)
			           << " idx=" << e.fieldIndex << " field=" << e.field << " value=" << e.value);

			neurus::Mesh* mesh = ResolveMesh(ctx, e.objectUid);
			if (!mesh) return;
			ctx.ops.Submit(std::make_unique<SetShaderFieldOp>(
				mesh->GetObjectID(), e.stage, e.section, e.fieldIndex,
				std::move(before), std::move(after)));
		});
	ctx.events.subscribe<ShaderFieldAdded>(
		[ctx](const ShaderFieldAdded& e) {
			ShaderUnit* unit = GetStageUnit(ResolveMesh(ctx, e.objectUid), e.stage);
			if (!unit) return;
			if (!AppendDefault(unit->parsed, e.section, e.subFieldIndex)) return;

			neurus::Mesh* mesh = ResolveMesh(ctx, e.objectUid);
			if (!mesh) return;
			ctx.ops.Submit(std::make_unique<AddShaderFieldOp>(
				mesh->GetObjectID(), e.stage, e.section, e.subFieldIndex, /*add=*/true));
		});

	// Replay: re-apply one edit dimension + bump version so the panel refreshes.
	// CPU-only, no recompile.
	ctx.events.subscribe<ShaderCodeRestored>(
		[ctx](const ShaderCodeRestored& e) { OnCodeRestored(e, ctx); });
	ctx.events.subscribe<ShaderFieldRestored>(
		[ctx](const ShaderFieldRestored& e) { OnFieldRestored(e, ctx); });
	ctx.events.subscribe<ShaderFieldAddRestored>(
		[ctx](const ShaderFieldAddRestored& e) { OnFieldAddRestored(e, ctx); });
	ctx.events.subscribe<ShaderFieldRemoved>(
		[ctx](const ShaderFieldRemoved& e) { OnFieldRemoved(e, ctx); });
}

} // namespace neurus