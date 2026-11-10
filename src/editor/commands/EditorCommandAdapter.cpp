#include "editor/commands/EditorCommandAdapter.h"

#include "asset/data/MeshData.h"
#include "command/Commands.h"
#include "core/RenderConfig.h"
#include "core/ResourceManager.h"
#include "editor/Editor.h"
#include "editor/controllers/MutationNotify.h"
#include "editor/events/ConfigEvents.h"
#include "editor/events/SceneEvents.h"
#include "editor/operations/HistoryView.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/ObjectID.h"
#include "scene/Scene.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neurus
{
namespace
{

using command::CommandErrorCode;
using command::CommandResult;
using command::Json;

// --- JSON helpers ---

Json Vec3Json(const glm::vec3& value)
{
	return Json::array({value.x, value.y, value.z});
}

bool ReadVec3(const Json& args, const char* key, glm::vec3& value, bool& present,
              std::string& error)
{
	present = args.contains(key);
	if (!present)
		return true;
	const Json& entry = args[key];
	if (!entry.is_array() || entry.size() != 3)
	{
		error = std::string("'") + key + "' must be an array of three numbers.";
		return false;
	}
	for (const auto& component : entry)
	{
		if (!component.is_number() || !std::isfinite(component.get<float>()))
		{
			error = std::string("'") + key + "' must contain finite numbers.";
			return false;
		}
	}
	value = glm::vec3(entry[0].get<float>(), entry[1].get<float>(), entry[2].get<float>());
	return true;
}

bool ReadFloat(const Json& args, const char* key, float& value, bool& present,
               std::string& error)
{
	present = args.contains(key);
	if (!present)
		return true;
	if (!args[key].is_number() || !std::isfinite(args[key].get<float>()))
	{
		error = std::string("'") + key + "' must be a finite number.";
		return false;
	}
	value = args[key].get<float>();
	return true;
}

bool ReadBool(const Json& args, const char* key, bool& value, bool& present,
              std::string& error)
{
	present = args.contains(key);
	if (!present)
		return true;
	if (!args[key].is_boolean())
	{
		error = std::string("'") + key + "' must be a boolean.";
		return false;
	}
	value = args[key].get<bool>();
	return true;
}

bool ReadString(const Json& args, const char* key, std::string& value, bool& present,
                std::string& error)
{
	present = args.contains(key);
	if (!present)
		return true;
	if (!args[key].is_string())
	{
		error = std::string("'") + key + "' must be a string.";
		return false;
	}
	value = args[key].get<std::string>();
	return true;
}

bool ReadInt(const Json& args, const char* key, int& value, bool& present,
             std::string& error)
{
	present = args.contains(key);
	if (!present)
		return true;
	if (!args[key].is_number_integer())
	{
		error = std::string("'") + key + "' must be an integer.";
		return false;
	}
	value = args[key].get<int>();
	return true;
}

// --- Type helpers ---

std::string TypeName(const ObjectID& object)
{
	switch (object.o_type)
	{
	case ObjectID::GOType::GO_CAM: return "camera";
	case ObjectID::GOType::GO_MESH: return "mesh";
	case ObjectID::GOType::GO_LIGHT:
	case ObjectID::GOType::GO_POLYLIGHT: return "light";
	case ObjectID::GOType::GO_ENVIR: return "environment";
	case ObjectID::GOType::GO_SPRITE: return "sprite";
	case ObjectID::GOType::GO_DL: return "debug_line";
	case ObjectID::GOType::GO_DP: return "debug_points";
	case ObjectID::GOType::GO_DM: return "debug_mesh";
	case ObjectID::GOType::GO_SDFFIELD: return "sdf_field";
	case ObjectID::GOType::NONE_GO: break;
	}
	return "object";
}

std::string LightTypeName(LightType type)
{
	switch (type)
	{
	case LightType::POINTLIGHT: return "point";
	case LightType::SUNLIGHT: return "sun";
	case LightType::SPOTLIGHT: return "spot";
	case LightType::AREALIGHT: return "area";
	case LightType::NONELIGHT: break;
	}
	return "none";
}

bool IsValidFilter(const std::string& filter)
{
	return filter.empty() || filter == "all" || filter == "object" || filter == "mesh"
		|| filter == "light" || filter == "lights" || filter == "camera"
		|| filter == "cameras" || filter == "environment" || filter == "env"
		|| filter == "sprite" || filter == "debug";
}

bool MatchesFilter(const ObjectID& object, const std::string& filter)
{
	if (filter.empty() || filter == "all" || filter == "object")
		return true;
	if (filter == "mesh")
		return object.o_type == ObjectID::GOType::GO_MESH;
	if (filter == "light" || filter == "lights")
		return object.IsLight();
	if (filter == "camera" || filter == "cameras")
		return object.o_type == ObjectID::GOType::GO_CAM;
	if (filter == "environment" || filter == "env")
		return object.o_type == ObjectID::GOType::GO_ENVIR;
	if (filter == "sprite")
		return object.o_type == ObjectID::GOType::GO_SPRITE;
	if (filter == "debug")
	{
		return object.o_type == ObjectID::GOType::GO_DL
			|| object.o_type == ObjectID::GOType::GO_DP
			|| object.o_type == ObjectID::GOType::GO_DM;
	}
	return false;
}

// --- Object description ---

Json ObjectJson(Scene& scene, ObjectID& object)
{
	Json json{
		{"uid", object.GetObjectID()},
		{"name", object.o_name},
		{"type", TypeName(object)},
		{"viewport_visible", object.is_viewport},
		{"render_visible", object.is_rendered},
	};

	if (void* transformPtr = object.GetTransform())
	{
		auto* transform = static_cast<Transform3D*>(transformPtr);
		json["position"] = Vec3Json(transform->GetPosition());
		json["rotation"] = Vec3Json(transform->GetRotation());
		json["scale"] = Vec3Json(transform->GetScale());
	}

	if (auto* mesh = dynamic_cast<Mesh*>(&object))
	{
		json["shadow"] = mesh->using_shadow;
		json["material"] = mesh->using_material;
		json["mesh_path"] = mesh->o_mesh ? mesh->o_mesh->GetPath() : std::string{};
	}
	else if (auto* light = dynamic_cast<Light*>(&object))
	{
		json["light_type"] = LightTypeName(light->light_type);
		json["power"] = light->light_power;
		json["color"] = Vec3Json(light->light_color);
		json["radius"] = light->light_radius;
		json["cutoff"] = light->spot_cutoff;
		json["outer_cutoff"] = light->spot_outer_cutoff;
		json["shadow"] = light->use_shadow;
	}
	else if (auto* camera = dynamic_cast<Camera*>(&object))
	{
		json["fov"] = camera->cam_pers;
		json["target"] = Vec3Json(camera->cam_tar);
		json["active"] = scene.ActiveCameraID() == camera->GetObjectID();
	}
	else if (auto* environment = dynamic_cast<Environment*>(&object))
	{
		json["intensity"] = environment->GetIntensity();
		json["environment_rotation"] = environment->GetRotation();
	}

	return json;
}

Json ObjectEnvelope(Scene& scene, ObjectID& object)
{
	return Json{{"object", ObjectJson(scene, object)}};
}

// --- Target resolution ---

ObjectID* ResolveTarget(Scene& scene, const Json& args, std::string& error,
                        CommandErrorCode& code)
{
	const bool hasName = args.contains("name");
	const bool hasUid = args.contains("uid");
	if (hasName && hasUid)
	{
		error = "Pass either 'name' or 'uid', not both.";
		code = CommandErrorCode::InvalidArgument;
		return nullptr;
	}

	std::optional<std::string> filter;
	if (args.contains("type"))
	{
		if (!args["type"].is_string())
		{
			error = "'type' must be a string.";
			code = CommandErrorCode::InvalidArgument;
			return nullptr;
		}
		filter = args["type"].get<std::string>();
		if (!IsValidFilter(*filter))
		{
			error = "Unknown object type: " + *filter;
			code = CommandErrorCode::InvalidArgument;
			return nullptr;
		}
	}

	if (hasUid)
	{
		if (!args["uid"].is_number_integer())
		{
			error = "'uid' must be an integer.";
			code = CommandErrorCode::InvalidArgument;
			return nullptr;
		}
		const int uid = args["uid"].get<int>();
		ObjectID* object = scene.GetObjectID(uid);
		if (object == nullptr)
		{
			error = "No scene object with uid " + std::to_string(uid) + ".";
			code = CommandErrorCode::ObjectNotFound;
			return nullptr;
		}
		if (filter && !MatchesFilter(*object, *filter))
		{
			error = "Object " + std::to_string(uid) + " is not a " + *filter + ".";
			code = CommandErrorCode::ObjectNotFound;
			return nullptr;
		}
		return object;
	}

	if (!hasName || !args["name"].is_string() || args["name"].get<std::string>().empty())
	{
		error = "Pass 'name' or 'uid' to identify the object.";
		code = CommandErrorCode::InvalidArgument;
		return nullptr;
	}

	const std::string name = args["name"].get<std::string>();
	std::vector<ObjectID*> matches;
	for (auto& [id, object] : scene.obj_list)
	{
		(void)id;
		if (object->o_name == name && (!filter || MatchesFilter(*object, *filter)))
			matches.push_back(object.get());
	}

	if (matches.empty())
	{
		error = "No scene object named '" + name + "'.";
		code = CommandErrorCode::ObjectNotFound;
		return nullptr;
	}
	if (matches.size() > 1)
	{
		Json uids = Json::array();
		for (ObjectID* match : matches)
			uids.push_back(match->GetObjectID());
		error = "Several scene objects are named '" + name + "'; pass 'type' or 'uid'.";
		code = CommandErrorCode::AmbiguousObject;
		return nullptr;
	}
	return matches.front();
}

bool ResolveTargets(Scene& scene, const Json& args, std::vector<int>& uids,
                    std::string& error, CommandErrorCode& code)
{
	if (args.contains("uids"))
	{
		if (args.contains("name") || args.contains("uid"))
		{
			error = "Pass either 'uids' or a single 'name'/'uid'.";
			code = CommandErrorCode::InvalidArgument;
			return false;
		}
		if (!args["uids"].is_array() || args["uids"].empty())
		{
			error = "'uids' must be a non-empty array of integers.";
			code = CommandErrorCode::InvalidArgument;
			return false;
		}
		for (const auto& entry : args["uids"])
		{
			if (!entry.is_number_integer())
			{
				error = "'uids' must contain only integers.";
				code = CommandErrorCode::InvalidArgument;
				return false;
			}
			const int uid = entry.get<int>();
			if (scene.GetObjectID(uid) == nullptr)
			{
				error = "No scene object with uid " + std::to_string(uid) + ".";
				code = CommandErrorCode::ObjectNotFound;
				return false;
			}
			if (std::find(uids.begin(), uids.end(), uid) == uids.end())
				uids.push_back(uid);
		}
		return true;
	}

	ObjectID* object = ResolveTarget(scene, args, error, code);
	if (object == nullptr)
		return false;
	uids.push_back(object->GetObjectID());
	return true;
}

// --- Object edit ---

struct ObjectEdit
{
	bool        hasRename = false;
	std::string rename;

	bool      hasPosition = false;
	glm::vec3 position{0.0f};
	bool      hasRotation = false;
	glm::vec3 rotation{0.0f};
	bool      hasScale = false;
	glm::vec3 scale{1.0f};

	bool hasVisible = false;
	bool visible = true;
	bool hasViewportVisible = false;
	bool viewportVisible = true;
	bool hasRenderVisible = false;
	bool renderVisible = true;

	bool hasActive = false;
	bool active = false;

	bool  hasFov = false;
	float fov = 60.0f;
	bool      hasTarget = false;
	glm::vec3 target{0.0f};

	bool  hasPower = false;
	float power = 10.0f;
	bool      hasColor = false;
	glm::vec3 color{1.0f};
	bool  hasRadius = false;
	float radius = 0.01f;
	bool  hasCutoff = false;
	float cutoff = 0.9f;
	bool  hasOuterCutoff = false;
	float outerCutoff = 0.8f;

	bool hasShadow = false;
	bool shadow = true;
	bool hasMaterial = false;
	bool material = true;

	bool  hasIntensity = false;
	float intensity = 1.0f;
	bool  hasEnvironmentRotation = false;
	float environmentRotation = 0.0f;

	bool Any() const
	{
		return hasRename || hasPosition || hasRotation || hasScale || hasVisible
			|| hasViewportVisible || hasRenderVisible || hasActive || hasFov || hasTarget
			|| hasPower || hasColor || hasRadius || hasCutoff || hasOuterCutoff
			|| hasShadow || hasMaterial || hasIntensity || hasEnvironmentRotation;
	}
};

bool ReadEdit(const Json& args, ObjectEdit& edit, std::string& error)
{
	if (!ReadString(args, "rename", edit.rename, edit.hasRename, error))
		return false;
	if (edit.hasRename && edit.rename.empty())
	{
		error = "'rename' cannot be empty.";
		return false;
	}
	if (!ReadVec3(args, "position", edit.position, edit.hasPosition, error)
	    || !ReadVec3(args, "rotation", edit.rotation, edit.hasRotation, error)
	    || !ReadVec3(args, "scale", edit.scale, edit.hasScale, error)
	    || !ReadBool(args, "visible", edit.visible, edit.hasVisible, error)
	    || !ReadBool(args, "viewport_visible", edit.viewportVisible,
	                 edit.hasViewportVisible, error)
	    || !ReadBool(args, "render_visible", edit.renderVisible, edit.hasRenderVisible, error)
	    || !ReadBool(args, "active", edit.active, edit.hasActive, error)
	    || !ReadFloat(args, "fov", edit.fov, edit.hasFov, error)
	    || !ReadVec3(args, "target", edit.target, edit.hasTarget, error)
	    || !ReadFloat(args, "power", edit.power, edit.hasPower, error)
	    || !ReadVec3(args, "color", edit.color, edit.hasColor, error)
	    || !ReadFloat(args, "radius", edit.radius, edit.hasRadius, error)
	    || !ReadFloat(args, "cutoff", edit.cutoff, edit.hasCutoff, error)
	    || !ReadFloat(args, "outer_cutoff", edit.outerCutoff, edit.hasOuterCutoff, error)
	    || !ReadBool(args, "shadow", edit.shadow, edit.hasShadow, error)
	    || !ReadBool(args, "material", edit.material, edit.hasMaterial, error)
	    || !ReadFloat(args, "intensity", edit.intensity, edit.hasIntensity, error)
	    || !ReadFloat(args, "environment_rotation", edit.environmentRotation,
	                  edit.hasEnvironmentRotation, error))
	{
		return false;
	}

	if (edit.hasFov && (edit.fov <= 0.0f || edit.fov >= 180.0f))
	{
		error = "'fov' must be between 0 and 180 degrees.";
		return false;
	}
	if ((edit.hasPower && edit.power < 0.0f) || (edit.hasRadius && edit.radius < 0.0f)
	    || (edit.hasIntensity && edit.intensity < 0.0f))
	{
		error = "power, radius and intensity cannot be negative.";
		return false;
	}
	return true;
}

bool ValidateEdit(ObjectID& object, const ObjectEdit& edit, std::string& error)
{
	const bool hasTransformProps = edit.hasPosition || edit.hasRotation || edit.hasScale;
	const bool isCamera = dynamic_cast<const Camera*>(&object) != nullptr;
	const bool isLight = object.IsLight();
	const bool isMesh = dynamic_cast<const Mesh*>(&object) != nullptr;
	const bool isEnvironment = dynamic_cast<const Environment*>(&object) != nullptr;

	if (hasTransformProps && object.GetTransform() == nullptr)
	{
		error = "Object '" + object.o_name + "' has no transform.";
		return false;
	}
	if ((edit.hasActive || edit.hasFov || edit.hasTarget) && !isCamera)
	{
		error = "Object '" + object.o_name + "' is not a camera.";
		return false;
	}
	if ((edit.hasPower || edit.hasColor || edit.hasRadius || edit.hasCutoff
	     || edit.hasOuterCutoff) && !isLight)
	{
		error = "Object '" + object.o_name + "' is not a light.";
		return false;
	}
	if (edit.hasMaterial && !isMesh)
	{
		error = "Object '" + object.o_name + "' is not a mesh.";
		return false;
	}
	if ((edit.hasIntensity || edit.hasEnvironmentRotation) && !isEnvironment)
	{
		error = "Object '" + object.o_name + "' is not an environment.";
		return false;
	}
	if (edit.hasShadow && !isLight && !isMesh)
	{
		error = "Object '" + object.o_name + "' is not a light or mesh.";
		return false;
	}
	return true;
}

/**
 * @brief RAII command edit scope: one command commits one undo entry.
 *
 * The guard closes the scope on an exception path too, so a failed write cannot
 * leave later commands buffering into this one.
 */
class CommandEditScope
{
public:
	CommandEditScope(OperationManager& operations, std::string label)
		: m_operations(operations)
	{
		m_operations.BeginCommandScope(std::move(label));
	}

	~CommandEditScope()
	{
		if (m_active)
			m_operations.EndCommandScope();
	}

	void Close()
	{
		if (m_active)
		{
			m_operations.EndCommandScope();
			m_active = false;
		}
	}

	CommandEditScope(const CommandEditScope&) = delete;
	CommandEditScope& operator=(const CommandEditScope&) = delete;

private:
	OperationManager& m_operations;
	bool              m_active = true;
};

void ApplyEditProperties(Editor& editor, Scene& scene, ObjectID& object,
                         const ObjectEdit& edit)
{
	const int uid = object.GetObjectID();
	IEventQueue& events = editor.GetEventQueue();

	if (edit.hasRename)
	{
		// Routed through the controller so the rename is recorded and undoable.
		events.Emit(RenameObjectRequested{uid, edit.rename});
	}
	if (edit.hasPosition)
		events.Emit(PositionChanged{uid, edit.position.x, edit.position.y, edit.position.z});
	if (edit.hasRotation)
		events.Emit(RotationChanged{uid, edit.rotation.x, edit.rotation.y, edit.rotation.z});
	if (edit.hasScale)
		events.Emit(ScaleChanged{uid, edit.scale.x, edit.scale.y, edit.scale.z});

	bool viewportVisible = object.is_viewport;
	bool renderVisible = object.is_rendered;
	bool visibilityChanged = false;
	if (edit.hasVisible)
	{
		viewportVisible = edit.visible;
		renderVisible = edit.visible;
		visibilityChanged = true;
	}
	if (edit.hasViewportVisible)
	{
		viewportVisible = edit.viewportVisible;
		visibilityChanged = true;
	}
	if (edit.hasRenderVisible)
	{
		renderVisible = edit.renderVisible;
		visibilityChanged = true;
	}
	if (visibilityChanged)
		events.Emit(VisibilityChanged{uid, viewportVisible, renderVisible});

	if (edit.hasActive)
	{
		if (edit.active)
			events.Emit(ActiveCameraChanged{uid});
		else if (scene.ActiveCameraID() == uid)
			events.Emit(ActiveCameraChanged{0});
	}
	if (edit.hasFov)
		events.Emit(CameraFovChanged{uid, edit.fov});
	if (edit.hasTarget)
		events.Emit(CameraTargetChanged{uid, edit.target.x, edit.target.y, edit.target.z});

	if (edit.hasPower)
		events.Emit(LightPowerChanged{uid, edit.power});
	if (edit.hasColor)
		events.Emit(LightColorChanged{uid, edit.color.r, edit.color.g, edit.color.b});
	if (edit.hasRadius)
		events.Emit(LightRadiusChanged{uid, edit.radius});
	if (edit.hasCutoff)
		events.Emit(LightCutoffChanged{uid, edit.cutoff});
	if (edit.hasOuterCutoff)
		events.Emit(LightOuterCutoffChanged{uid, edit.outerCutoff});
	if (edit.hasShadow)
	{
		if (object.IsLight())
			events.Emit(LightShadowChanged{uid, edit.shadow});
		else
			events.Emit(MeshShadowChanged{uid, edit.shadow});
	}
	if (edit.hasMaterial)
		events.Emit(MeshMaterialChanged{uid, edit.material});
	if (edit.hasIntensity)
		events.Emit(EnvironmentIntensityChanged{uid, edit.intensity});
	if (edit.hasEnvironmentRotation)
		events.Emit(EnvironmentRotationChanged{uid, edit.environmentRotation});

}

void ApplyEdit(Editor& editor, Scene& scene, ObjectID& object, const ObjectEdit& edit)
{
	CommandEditScope scope(editor.GetOperations(), "Set Object");
	ApplyEditProperties(editor, scene, object, edit);
	scope.Close();
}

// --- Render configuration ---

std::string PipelineName(RenderPipeLine value)
{
	switch (value)
	{
	case RenderPipeLine::Forward: return "forward";
	case RenderPipeLine::Deferred: return "deferred";
	case RenderPipeLine::Custom0: return "custom0";
	}
	return "deferred";
}

std::string AaName(AAAlg value)
{
	switch (value)
	{
	case AAAlg::None: return "none";
	case AAAlg::MSAA: return "msaa";
	case AAAlg::FXAA: return "fxaa";
	}
	return "none";
}

std::string AoName(AOAlg value)
{
	switch (value)
	{
	case AOAlg::None: return "none";
	case AOAlg::SSAO: return "ssao";
	}
	return "none";
}

std::string ShadowName(ShadowAlg value)
{
	switch (value)
	{
	case ShadowAlg::None: return "none";
	case ShadowAlg::ShadowMapping: return "shadow_mapping";
	case ShadowAlg::SDFSoftShadow: return "sdf_soft";
	case ShadowAlg::VSSM: return "vssm";
	}
	return "none";
}

std::string SsrName(SSRAlg value)
{
	switch (value)
	{
	case SSRAlg::None: return "none";
	case SSRAlg::RayMarching: return "ray_marching";
	case SSRAlg::SDFRayMarching: return "sdf_ray_marching";
	case SSRAlg::SDFResolvedRayMarching: return "sdf_resolved";
	}
	return "none";
}

Json RenderConfigJson(const RenderConfig& config)
{
	return Json{
		{"pipeline", PipelineName(config.r_pipeline)},
		{"aa", AaName(config.r_aa)},
		{"ao", AoName(config.r_ao)},
		{"shadow", ShadowName(config.r_shadow)},
		{"ssr", SsrName(config.r_ssr)},
		{"gamma", config.r_gamma},
		{"shadow_bias", config.r_shadow_bias},
		{"ao_ksize", config.r_ao_ksize},
		{"ao_radius", config.r_ao_radius},
		{"sample_pf", config.r_sample_pf},
		{"sampling_count", config.r_sampling_count},
		{"sampling_mode", config.r_sampling_mode},
		{"transparent", config.r_transparent},
		{"debug_draw", config.r_debug_draw},
		{"fxaa_subpix", config.r_fxaa_subpix},
		{"fxaa_edge_threshold", config.r_fxaa_edge_threshold},
		{"fxaa_edge_threshold_min", config.r_fxaa_edge_threshold_min},
	};
}

bool ParsePipeline(const std::string& text, RenderPipeLine& value)
{
	if (text == "forward")
	{
		value = RenderPipeLine::Forward;
		return true;
	}
	if (text == "deferred")
	{
		value = RenderPipeLine::Deferred;
		return true;
	}
	if (text == "custom0")
	{
		value = RenderPipeLine::Custom0;
		return true;
	}
	return false;
}

bool ParseAa(const std::string& text, AAAlg& value)
{
	if (text == "none")
	{
		value = AAAlg::None;
		return true;
	}
	if (text == "msaa")
	{
		value = AAAlg::MSAA;
		return true;
	}
	if (text == "fxaa")
	{
		value = AAAlg::FXAA;
		return true;
	}
	return false;
}

bool ParseAo(const std::string& text, AOAlg& value)
{
	if (text == "none")
	{
		value = AOAlg::None;
		return true;
	}
	if (text == "ssao")
	{
		value = AOAlg::SSAO;
		return true;
	}
	return false;
}

bool ParseShadow(const std::string& text, ShadowAlg& value)
{
	if (text == "none")
	{
		value = ShadowAlg::None;
		return true;
	}
	if (text == "shadow_mapping")
	{
		value = ShadowAlg::ShadowMapping;
		return true;
	}
	if (text == "sdf_soft")
	{
		value = ShadowAlg::SDFSoftShadow;
		return true;
	}
	if (text == "vssm")
	{
		value = ShadowAlg::VSSM;
		return true;
	}
	return false;
}

bool ParseSsr(const std::string& text, SSRAlg& value)
{
	if (text == "none")
	{
		value = SSRAlg::None;
		return true;
	}
	if (text == "ray_marching")
	{
		value = SSRAlg::RayMarching;
		return true;
	}
	if (text == "sdf_ray_marching")
	{
		value = SSRAlg::SDFRayMarching;
		return true;
	}
	if (text == "sdf_resolved")
	{
		value = SSRAlg::SDFResolvedRayMarching;
		return true;
	}
	return false;
}

} // namespace

EditorCommandAdapter::EditorCommandAdapter(Editor& editor)
	: m_editor(editor)
{
}

command::CommandResult EditorCommandAdapter::Execute(const command::CommandRequest& request)
{
	if (request.name == command::kSceneList)
		return ExecuteSceneList(request.args);
	if (request.name == command::kSceneInfo)
		return ExecuteSceneInfo();
	if (request.name == command::kObjectGet)
		return ExecuteObjectGet(request.args);
	if (request.name == command::kObjectSet)
		return ExecuteObjectSet(request.args);
	if (request.name == command::kObjectLoad)
		return ExecuteObjectLoad(request.args);
	if (request.name == command::kObjectAdd)
		return ExecuteObjectAdd(request.args);
	if (request.name == command::kObjectDuplicate)
		return ExecuteObjectDuplicate(request.args);
	if (request.name == command::kObjectDelete)
		return ExecuteObjectDelete(request.args);
	if (request.name == command::kRenderGet)
		return ExecuteRenderGet();
	if (request.name == command::kRenderSet)
		return ExecuteRenderSet(request.args);
	if (request.name == command::kHistoryGet)
		return ExecuteHistoryGet();
	if (request.name == command::kHistoryUndo)
		return ExecuteHistoryUndo();
	if (request.name == command::kHistoryRedo)
		return ExecuteHistoryRedo();

	return CommandResult::Failure(CommandErrorCode::InvalidArgument,
	                              "Unknown command: " + request.name);
}

command::CommandResult EditorCommandAdapter::ExecuteSceneList(const command::Json& args)
{
	std::string type = "all";
	if (args.contains("type"))
	{
		if (!args["type"].is_string())
		{
			return CommandResult::Failure(CommandErrorCode::InvalidArgument,
			                              "'type' must be a string.");
		}
		type = args["type"].get<std::string>();
	}
	if (!IsValidFilter(type))
	{
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "Unknown object type: " + type);
	}

	Scene& scene = m_editor.GetScene();
	std::vector<ObjectID*> objects;
	for (auto& [id, object] : scene.obj_list)
	{
		(void)id;
		if (MatchesFilter(*object, type))
			objects.push_back(object.get());
	}
	std::sort(objects.begin(), objects.end(), [](const ObjectID* left, const ObjectID* right)
	{
		return left->GetObjectID() < right->GetObjectID();
	});

	Json entries = Json::array();
	for (ObjectID* object : objects)
		entries.push_back(ObjectJson(scene, *object));

	return CommandResult::Success({
		{"type", type},
		{"count", entries.size()},
		{"objects", entries},
	});
}

command::CommandResult EditorCommandAdapter::ExecuteSceneInfo()
{
	Scene& scene = m_editor.GetScene();
	int meshes = 0;
	int lights = 0;
	int cameras = 0;
	int environments = 0;
	int sprites = 0;
	int debug = 0;
	for (auto& [id, object] : scene.obj_list)
	{
		(void)id;
		switch (object->o_type)
		{
		case ObjectID::GOType::GO_MESH: ++meshes; break;
		case ObjectID::GOType::GO_LIGHT:
		case ObjectID::GOType::GO_POLYLIGHT: ++lights; break;
		case ObjectID::GOType::GO_CAM: ++cameras; break;
		case ObjectID::GOType::GO_ENVIR: ++environments; break;
		case ObjectID::GOType::GO_SPRITE: ++sprites; break;
		case ObjectID::GOType::GO_DL:
		case ObjectID::GOType::GO_DP:
		case ObjectID::GOType::GO_DM: ++debug; break;
		case ObjectID::GOType::GO_SDFFIELD:
		case ObjectID::GOType::NONE_GO: break;
		}
	}

	std::vector<int> selected;
	int activeUid = 0;
	SnapshotSelectionUids(scene.selections, selected, activeUid);
	Json selectedJson = Json::array();
	for (int uid : selected)
		selectedJson.push_back(uid);

	return CommandResult::Success({
		{"objects", scene.obj_list.size()},
		{"meshes", meshes},
		{"lights", lights},
		{"cameras", cameras},
		{"environments", environments},
		{"sprites", sprites},
		{"debug", debug},
		{"selected", selectedJson},
		{"active_uid", activeUid},
		{"active_camera", scene.ActiveCameraID()},
		{"dirty", m_editor.IsDirty()},
	});
}

command::CommandResult EditorCommandAdapter::ExecuteObjectGet(const command::Json& args)
{
	Scene& scene = m_editor.GetScene();
	std::string error;
	CommandErrorCode code = CommandErrorCode::InvalidArgument;
	ObjectID* object = ResolveTarget(scene, args, error, code);
	if (object == nullptr)
		return CommandResult::Failure(code, error);

	return CommandResult::Success(ObjectEnvelope(scene, *object));
}

command::CommandResult EditorCommandAdapter::ExecuteObjectSet(const command::Json& args)
{
	Scene& scene = m_editor.GetScene();
	std::string error;
	CommandErrorCode code = CommandErrorCode::InvalidArgument;
	ObjectID* object = ResolveTarget(scene, args, error, code);
	if (object == nullptr)
		return CommandResult::Failure(code, error);

	ObjectEdit edit;
	if (!ReadEdit(args, edit, error))
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);
	if (!edit.Any())
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "No property to set was given.");
	if (!ValidateEdit(*object, edit, error))
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);

	ApplyEdit(m_editor, scene, *object, edit);
	return CommandResult::Success(ObjectEnvelope(scene, *object));
}

command::CommandResult EditorCommandAdapter::ExecuteObjectLoad(const command::Json& args)
{
	if (!args.contains("path") || !args["path"].is_string()
	    || args["path"].get<std::string>().empty())
	{
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'path' must be a non-empty string.");
	}
	std::string name;
	bool hasName = false;
	std::string error;
	if (!ReadString(args, "name", name, hasName, error))
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);

	std::string loadError;
	const int uid = m_editor.ImportMeshObject(args["path"].get<std::string>(), name, loadError);
	if (uid == 0)
	{
		return CommandResult::Failure(
			CommandErrorCode::IoError,
			loadError.empty() ? "Loading the mesh failed." : loadError,
			{{"path", args["path"]}});
	}

	Scene& scene = m_editor.GetScene();
	ObjectID* object = scene.GetObjectID(uid);
	if (object == nullptr)
	{
		return CommandResult::Failure(CommandErrorCode::Internal,
		                              "The imported mesh did not enter the scene.");
	}
	return CommandResult::Success({
		{"loaded", true},
		{"object", ObjectJson(scene, *object)},
	});
}

command::CommandResult EditorCommandAdapter::ExecuteObjectAdd(const command::Json& args)
{
	std::string error;
	std::string type;
	bool hasType = false;
	if (!ReadString(args, "type", type, hasType, error) || !hasType || type.empty())
	{
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'type' is required.");
	}

	if (type == "light" || type == "point-light" || type == "point_light"
	    || type == "sun-light" || type == "sun_light" || type == "spot-light"
	    || type == "spot_light")
	{
		return CommandResult::Failure(
			CommandErrorCode::InvalidArgument,
			"Adding lights is disabled while issue #60 is open.");
	}
	if (type != "camera")
	{
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "Unsupported object type: " + type
		                                  + ". Only camera can be added.");
	}

	std::string name;
	bool hasName = false;
	if (!ReadString(args, "name", name, hasName, error))
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);

	ObjectEdit edit;
	if (!ReadEdit(args, edit, error))
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);

	// Validate before creating anything, so a rejected property cannot leave a
	// half-configured object behind.
	Camera probe;
	if (!ValidateEdit(probe, edit, error))
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);

	CommandEditScope scope(m_editor.GetOperations(), "Add Object");
	std::string addError;
	const int uid = m_editor.AddCameraObject(name, addError);
	if (uid == 0)
	{
		return CommandResult::Failure(
			CommandErrorCode::IoError,
			addError.empty() ? "Creating the camera failed." : addError);
	}

	Scene& scene = m_editor.GetScene();
	ObjectID* object = scene.GetObjectID(uid);
	if (object == nullptr)
	{
		return CommandResult::Failure(CommandErrorCode::Internal,
		                              "The new camera did not enter the scene.");
	}
	if (edit.Any())
		ApplyEditProperties(m_editor, scene, *object, edit);
	scope.Close();

	return CommandResult::Success(ObjectEnvelope(scene, *object));
}

command::CommandResult EditorCommandAdapter::ExecuteObjectDuplicate(const command::Json& args)
{
	Scene& scene = m_editor.GetScene();
	std::string error;
	CommandErrorCode code = CommandErrorCode::InvalidArgument;
	ObjectID* source = ResolveTarget(scene, args, error, code);
	if (source == nullptr)
		return CommandResult::Failure(code, error);

	std::string rename;
	bool hasRename = false;
	if (!ReadString(args, "rename", rename, hasRename, error))
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);
	if (hasRename && rename.empty())
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'rename' cannot be empty.");

	if (source->IsLight())
	{
		return CommandResult::Failure(
			CommandErrorCode::InvalidArgument,
			"Duplicating lights is disabled while issue #60 is open.");
	}

	auto* camera = dynamic_cast<Camera*>(source);
	auto* mesh = dynamic_cast<Mesh*>(source);
	if (camera == nullptr && mesh == nullptr)
	{
		return CommandResult::Failure(
			CommandErrorCode::InvalidArgument,
			"Duplicating a " + TypeName(*source) + " is not supported.");
	}

	CommandEditScope scope(m_editor.GetOperations(), "Duplicate Object");
	ResourceManager& resources = m_editor.GetResourceManager();
	int uid = 0;
	if (camera != nullptr)
	{
		auto copy = resources.Load<Camera>(
			camera->cam_w, camera->cam_h, camera->cam_pers, camera->cam_near, camera->cam_far);
		copy->SetPosition(camera->GetPosition());
		copy->SetRotation(camera->GetRotation());
		copy->SetScale(camera->GetScale());
		copy->SetTarPos(camera->cam_tar);
		uid = copy->GetObjectID();
	}
	else
	{
		auto copy = resources.Load<Mesh>(mesh->o_mesh);
		copy->SetPosition(mesh->GetPosition());
		copy->SetRotation(mesh->GetRotation());
		copy->SetScale(mesh->GetScale());
		copy->using_shadow = mesh->using_shadow;
		copy->using_material = mesh->using_material;
		copy->using_sdf = mesh->using_sdf;
		copy->is_closure = mesh->is_closure;
		copy->o_material = mesh->o_material;
		if (mesh->o_shader)
			copy->SetObjShader(mesh->o_shader);
		uid = copy->GetObjectID();
	}

	auto pooled = resources.Get<ObjectID>(uid);
	if (pooled == nullptr)
	{
		return CommandResult::Failure(CommandErrorCode::Internal,
		                              "The duplicate resource is missing.");
	}
	const std::string name = hasRename ? rename : source->o_name + " Copy";
	const std::string unique = resources.SetName(uid, name);
	pooled->o_name = unique.empty() ? name : unique;
	m_editor.GetEventQueue().Emit(SceneObjectAddRequested{uid});

	ObjectID* object = scene.GetObjectID(uid);
	if (object == nullptr)
	{
		return CommandResult::Failure(CommandErrorCode::Internal,
		                              "The duplicate did not enter the scene.");
	}
	scope.Close();

	return CommandResult::Success(ObjectEnvelope(scene, *object));
}

command::CommandResult EditorCommandAdapter::ExecuteObjectDelete(const command::Json& args)
{
	Scene& scene = m_editor.GetScene();
	std::vector<int> uids;
	std::string error;
	CommandErrorCode code = CommandErrorCode::InvalidArgument;
	if (!ResolveTargets(scene, args, uids, error, code))
		return CommandResult::Failure(code, error);

	Json deleted = Json::array();
	for (int uid : uids)
	{
		ObjectID* object = scene.GetObjectID(uid);
		if (object == nullptr)
			continue;
		deleted.push_back({{"uid", uid}, {"name", object->o_name}, {"type", TypeName(*object)}});
	}

	m_editor.GetEventQueue().Emit(DeleteObjectsRequested{uids});
	return CommandResult::Success({
		{"deleted", deleted},
		{"count", deleted.size()},
	});
}

command::CommandResult EditorCommandAdapter::ExecuteRenderGet()
{
	return CommandResult::Success({{"config", RenderConfigJson(m_editor.GetRenderConfig())}});
}

command::CommandResult EditorCommandAdapter::ExecuteRenderSet(const command::Json& args)
{
	RenderConfig config = m_editor.GetRenderConfig();
	bool any = false;
	std::string error;

	auto setEnum = [&](const char* key, auto& value, auto parser) -> bool
	{
		std::string text;
		bool present = false;
		if (!ReadString(args, key, text, present, error))
			return false;
		if (!present)
			return true;
		if (!parser(text, value))
		{
			error = std::string("Unknown '") + key + "' value: " + text;
			return false;
		}
		any = true;
		return true;
	};
	auto setFloat = [&](const char* key, float& value) -> bool
	{
		bool present = false;
		if (!ReadFloat(args, key, value, present, error))
			return false;
		any = any || present;
		return true;
	};
	auto setInt = [&](const char* key, int& value) -> bool
	{
		bool present = false;
		if (!ReadInt(args, key, value, present, error))
			return false;
		any = any || present;
		return true;
	};
	auto setBool = [&](const char* key, bool& value) -> bool
	{
		bool present = false;
		if (!ReadBool(args, key, value, present, error))
			return false;
		any = any || present;
		return true;
	};

	int samplingMode = static_cast<int>(config.r_sampling_mode);
	bool samplingModePresent = false;
	if (!ReadInt(args, "sampling_mode", samplingMode, samplingModePresent, error))
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);
	if (samplingModePresent)
	{
		config.r_sampling_mode = static_cast<std::uint32_t>(samplingMode);
		any = true;
	}

	if (!setEnum("pipeline", config.r_pipeline, ParsePipeline)
	    || !setEnum("aa", config.r_aa, ParseAa)
	    || !setEnum("ao", config.r_ao, ParseAo)
	    || !setEnum("shadow", config.r_shadow, ParseShadow)
	    || !setEnum("ssr", config.r_ssr, ParseSsr)
	    || !setFloat("gamma", config.r_gamma)
	    || !setFloat("shadow_bias", config.r_shadow_bias)
	    || !setFloat("ao_radius", config.r_ao_radius)
	    || !setFloat("fxaa_subpix", config.r_fxaa_subpix)
	    || !setFloat("fxaa_edge_threshold", config.r_fxaa_edge_threshold)
	    || !setFloat("fxaa_edge_threshold_min", config.r_fxaa_edge_threshold_min)
	    || !setInt("ao_ksize", config.r_ao_ksize)
	    || !setInt("sample_pf", config.r_sample_pf)
	    || !setInt("sampling_count", config.r_sampling_count)
	    || !setBool("transparent", config.r_transparent)
	    || !setBool("debug_draw", config.r_debug_draw))
	{
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, error);
	}

	if (!any)
	{
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "No render setting to change was given.");
	}
	if (config.r_gamma <= 0.0f)
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'gamma' must be positive.");
	if (config.r_shadow_bias < 0.0f || config.r_ao_radius < 0.0f)
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'shadow_bias' and 'ao_radius' cannot be negative.");
	if (config.r_ao_ksize < 1 || config.r_sample_pf < 1 || config.r_sampling_count < 1)
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "ao_ksize, sample_pf and sampling_count must be at least 1.");
	if (config.r_sampling_mode > 1)
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'sampling_mode' must be 0 or 1.");
	if (config.r_fxaa_subpix < 0.0f || config.r_fxaa_subpix > 1.0f)
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'fxaa_subpix' must be between 0 and 1.");
	if (config.r_fxaa_edge_threshold < 0.063f || config.r_fxaa_edge_threshold > 0.333f)
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'fxaa_edge_threshold' must be between 0.063 and 0.333.");
	if (config.r_fxaa_edge_threshold_min < 0.0312f
	    || config.r_fxaa_edge_threshold_min > 0.0833f)
	{
		return CommandResult::Failure(CommandErrorCode::InvalidArgument,
		                              "'fxaa_edge_threshold_min' must be between 0.0312 and 0.0833.");
	}

	m_editor.GetEventQueue().Emit(RenderConfigChangedEvent{config});
	return CommandResult::Success({{"config", RenderConfigJson(m_editor.GetRenderConfig())}});
}

command::CommandResult EditorCommandAdapter::ExecuteHistoryGet()
{
	const HistoryView history = m_editor.GetHistory();
	Json undo = Json::array();
	Json redo = Json::array();
	for (const auto& label : history.undo)
		undo.push_back(label);
	for (const auto& label : history.redo)
		redo.push_back(label);
	return CommandResult::Success({
		{"undo", undo},
		{"redo", redo},
		{"can_undo", !history.undo.empty()},
		{"can_redo", !history.redo.empty()},
		{"revision", history.revision},
	});
}

command::CommandResult EditorCommandAdapter::ExecuteHistoryUndo()
{
	if (!m_editor.GetOperations().CanUndo())
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, "Nothing to undo.");
	m_editor.GetOperations().Undo();
	return ExecuteHistoryGet();
}

command::CommandResult EditorCommandAdapter::ExecuteHistoryRedo()
{
	if (!m_editor.GetOperations().CanRedo())
		return CommandResult::Failure(CommandErrorCode::InvalidArgument, "Nothing to redo.");
	m_editor.GetOperations().Redo();
	return ExecuteHistoryGet();
}

} // namespace neurus
