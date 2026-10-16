/**
 * @file SceneBindings.cpp
 * @brief Read-only camera/light/mesh/environment proxies, the typed `neurus.data`
 *        collections, and `neurus.context` (design sections 4.5 and 5).
 *
 * Every value is resolved live through the run's `ScriptContext`; nothing is
 * cached and no setter is defined (write support is a later task). Collections
 * are empty - never a null dereference - outside a run or when the run has no
 * scene.
 */

#include <pybind11/pybind11.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "editor/events/SceneEvents.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Material.h"
#include "scene/Mesh.h"
#include "scene/ObjectID.h"
#include "scene/Scene.h"
#include "scene/Transform.h"
#include "script/bindings/Bindings.h"
#include "script/bindings/Proxies.h"
#include "script/bindings/ProxyBase.h"
#include "script/bindings/Runtime.h"

namespace py = pybind11;

namespace neurus
{
namespace script
{
namespace
{

/// Converts a scene vector to a Python 3-tuple (design section 5 mapping).
py::tuple ToTuple(const glm::vec3& value)
{
	return py::make_tuple(value.x, value.y, value.z);
}

/// @brief Resolves @p self to the live typed object, or raises a clear error.
template <typename T>
const T* Require(const ProxyBase& self)
{
	if (self.ctx == nullptr || self.ctx->scene == nullptr)
	{
		throw std::runtime_error("neurus: proxy has no scene context");
	}
	const ObjectID* obj = self.ctx->scene->GetObjectID(self.uid);
	const T* typed = dynamic_cast<const T*>(obj);
	if (typed == nullptr)
	{
		throw std::runtime_error("neurus: proxy refers to a missing or mistyped object");
	}
	return typed;
}

struct CameraProxy : ProxyBase {};
struct LightProxy : ProxyBase {};
struct MeshProxy : ProxyBase {};
struct EnvironmentProxy : ProxyBase {};

/// @brief Stateless typed collection over one scene pool, keyed by UID.
template <typename Proxy, typename T>
struct TypedCollection
{
	using Pool = std::unordered_map<int, std::shared_ptr<T>>;
	Pool Scene::* pool = nullptr;

	py::list Snapshot() const
	{
		py::list result;
		const ScriptContext* ctx = CurrentContext();
		if (ctx == nullptr || ctx->scene == nullptr)
		{
			return result;
		}
		for (const auto& [id, obj] : ctx->scene->*(pool))
		{
			(void)obj;
			result.append(py::cast(Proxy{ctx, id}));
		}
		return result;
	}

	static py::object Lookup(Pool Scene::* pool, int uid)
	{
		const ScriptContext* ctx = CurrentContext();
		if (ctx == nullptr || ctx->scene == nullptr)
		{
			return py::none();
		}
		const Pool& objects = ctx->scene->*(pool);
		if (objects.find(uid) == objects.end())
		{
			return py::none();
		}
		return py::cast(Proxy{ctx, uid});
	}

	static bool Contains(Pool Scene::* pool, int uid)
	{
		const ScriptContext* ctx = CurrentContext();
		if (ctx == nullptr || ctx->scene == nullptr)
		{
			return false;
		}
		const Pool& objects = ctx->scene->*(pool);
		return objects.find(uid) != objects.end();
	}
};

template <typename Proxy, typename T>
void RegisterCollection(py::module_& data, const char* name, typename TypedCollection<Proxy, T>::Pool Scene::* pool)
{
	using Collection = TypedCollection<Proxy, T>;
	py::class_<Collection>(data, (std::string("_") + name).c_str())
		.def("__iter__", [](const Collection& self) { return py::iter(self.Snapshot()); })
		.def("__len__", [](const Collection& self) { return py::len(self.Snapshot()); })
		.def("__getitem__",
		     [pool](const Collection&, int uid) -> py::object { return Collection::Lookup(pool, uid); })
		.def("__contains__", [pool](const Collection&, int uid) { return Collection::Contains(pool, uid); });

	Collection instance;
	instance.pool = pool;
	data.attr(name) = py::cast(instance);
}

/// @brief `neurus.context`: the run's active camera and selection.
struct ContextView {};

py::object ActiveCamera()
{
	const ScriptContext* ctx = CurrentContext();
	if (ctx == nullptr || ctx->scene == nullptr)
	{
		return py::none();
	}
	// The scene owns the active camera; read it live (0 = none).
	const int activeId = ctx->scene->ActiveCameraID();
	if (activeId <= 0)
	{
		return py::none();
	}
	const ObjectID* obj = ctx->scene->GetObjectID(activeId);
	if (dynamic_cast<const Camera*>(obj) == nullptr)
	{
		return py::none();
	}
	return py::cast(CameraProxy{ctx, activeId});
}

py::list Selection()
{
	py::list result;
	const ScriptContext* ctx = CurrentContext();
	if (ctx == nullptr || ctx->scene == nullptr)
	{
		return result;
	}
	// The scene owns the selection; snapshot its UIDs here (one source of truth).
	std::vector<int> selected;
	int activeUid = 0;
	SnapshotSelectionUids(ctx->scene->selections, selected, activeUid);
	for (const int uid : selected)
	{
		if (ctx->scene->GetObjectID(uid) != nullptr)
		{
			result.append(py::cast(ObjectProxy{ctx, uid}));
		}
	}
	return result;
}

}  // namespace

void RegisterSceneBindings(py::module_& root, py::module_& data)
{
	py::class_<CameraProxy>(data, "Camera", "Read-only proxy over a scene camera.")
		.def_property_readonly("uid", [](const CameraProxy& s) { return s.uid; })
		.def_property(
			"fov",
			[](const CameraProxy& s) { return Require<Camera>(s)->cam_pers; },
			[](const CameraProxy& s, float value) { EmitEvent(CameraFovChanged{s.uid, value}); })
		.def_property_readonly("near", [](const CameraProxy& s) { return Require<Camera>(s)->cam_near; })
		.def_property_readonly("far", [](const CameraProxy& s) { return Require<Camera>(s)->cam_far; })
		.def_property(
			"position",
			[](const CameraProxy& s) { return ToTuple(Require<Camera>(s)->GetPosition()); },
			[](const CameraProxy& s, std::tuple<float, float, float> value)
			{
				const Camera* cam = Require<Camera>(s);
				const auto [x, y, z] = value;
				EmitEvent(CameraPoseChanged{s.uid, x, y, z,
				                            cam->cam_tar.x, cam->cam_tar.y, cam->cam_tar.z});
			})
		.def_property_readonly("rotation",
		     [](const CameraProxy& s) { return ToTuple(Require<Camera>(s)->GetRotation()); })
		.def_property(
			"is_active",
			[](const CameraProxy& s)
			{
				return s.ctx != nullptr && s.ctx->scene != nullptr
				       && s.ctx->scene->ActiveCameraID() == s.uid;
			},
			[](const CameraProxy& s, bool value) { EmitEvent(ActiveCameraChanged{value ? s.uid : 0}); });

	py::class_<LightProxy>(data, "Light", "Read-only proxy over a scene light.")
		.def_property_readonly("uid", [](const LightProxy& s) { return s.uid; })
		.def_property_readonly("type",
		     [](const LightProxy& s) { return static_cast<int>(Require<Light>(s)->light_type); })
		.def_property(
			"power",
			[](const LightProxy& s) { return Require<Light>(s)->light_power; },
			[](const LightProxy& s, float value) { EmitEvent(LightPowerChanged{s.uid, value}); })
		.def_property(
			"color",
			[](const LightProxy& s) { return ToTuple(Require<Light>(s)->light_color); },
			[](const LightProxy& s, std::tuple<float, float, float> value)
			{
				const auto [r, g, b] = value;
				EmitEvent(LightColorChanged{s.uid, r, g, b});
			})
		.def_property(
			"radius",
			[](const LightProxy& s) { return Require<Light>(s)->light_radius; },
			[](const LightProxy& s, float value) { EmitEvent(LightRadiusChanged{s.uid, value}); })
		.def_property(
			"shadow",
			[](const LightProxy& s) { return Require<Light>(s)->use_shadow; },
			[](const LightProxy& s, bool value) { EmitEvent(LightShadowChanged{s.uid, value}); })
		.def_property(
			"cutoff",
			[](const LightProxy& s) { return Require<Light>(s)->spot_cutoff; },
			[](const LightProxy& s, float value) { EmitEvent(LightCutoffChanged{s.uid, value}); })
		.def_property(
			"outer_cutoff",
			[](const LightProxy& s) { return Require<Light>(s)->spot_outer_cutoff; },
			[](const LightProxy& s, float value) { EmitEvent(LightOuterCutoffChanged{s.uid, value}); });

	py::class_<MeshProxy>(data, "Mesh", "Read-only proxy over a scene mesh.")
		.def_property_readonly("uid", [](const MeshProxy& s) { return s.uid; })
		.def_property(
			"shadow",
			[](const MeshProxy& s) { return Require<Mesh>(s)->using_shadow; },
			[](const MeshProxy& s, bool value) { EmitEvent(MeshShadowChanged{s.uid, value}); })
		.def_property(
			"using_material",
			[](const MeshProxy& s) { return Require<Mesh>(s)->using_material; },
			[](const MeshProxy& s, bool value) { EmitEvent(MeshMaterialChanged{s.uid, value}); })
		.def_property_readonly("material",
		     [](const MeshProxy& s) -> py::object
		     {
			     const std::shared_ptr<Material>& mat = Require<Mesh>(s)->o_material;
			     if (!mat)
			     {
				     return py::none();
			     }
			     return py::str(mat->mat_name);
		     });

	py::class_<EnvironmentProxy>(data, "Environment", "Read-only proxy over the scene environment.")
		.def_property_readonly("uid", [](const EnvironmentProxy& s) { return s.uid; })
		.def_property(
			"intensity",
			[](const EnvironmentProxy& s) { return Require<Environment>(s)->GetIntensity(); },
			[](const EnvironmentProxy& s, float value) { EmitEvent(EnvironmentIntensityChanged{s.uid, value}); })
		.def_property(
			"rotation",
			[](const EnvironmentProxy& s) { return Require<Environment>(s)->GetRotation(); },
			[](const EnvironmentProxy& s, float value) { EmitEvent(EnvironmentRotationChanged{s.uid, value}); });

	RegisterCollection<CameraProxy, Camera>(data, "cameras", &Scene::cam_list);
	RegisterCollection<LightProxy, Light>(data, "lights", &Scene::light_list);
	RegisterCollection<MeshProxy, Mesh>(data, "meshes", &Scene::mesh_list);
	RegisterCollection<EnvironmentProxy, Environment>(data, "environments", &Scene::env_list);

	py::class_<ContextView>(root, "_Context", "The run's active camera and selection.")
		.def_property_readonly("active_camera", [](const ContextView&) { return ActiveCamera(); })
		.def_property_readonly("selection", [](const ContextView&) { return Selection(); });

	root.attr("context") = py::cast(ContextView{});
}

}  // namespace script
}  // namespace neurus
