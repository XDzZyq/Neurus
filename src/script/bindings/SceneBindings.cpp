/**
 * @file SceneBindings.cpp
 * @brief Read-only camera/light/mesh/environment proxies, the typed `neurus.data`
 *        collections, and `neurus.context`.
 *
 * Every proxy is constructed with the run's `ScriptView` and resolves its own
 * object from that view on each access; nothing is cached and no view or proxy
 * holds a run-scoped pointer. Collections are empty - never a null dereference -
 * when the run has no scene. No setter writes a C++ property (each routes the
 * exact typed event through the run dispatch, which refuses in this phase).
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
#include "script/ScriptContext.h"
#include "script/bindings/Bindings.h"
#include "script/Proxies.h"
#include "script/ProxyBase.h"

namespace py = pybind11;

namespace neurus
{
namespace script
{
namespace
{

/// Converts a scene vector to a Python 3-tuple.
py::tuple ToTuple(const glm::vec3& value)
{
	return py::make_tuple(value.x, value.y, value.z);
}

struct CameraProxy : ProxyBase
{
	/// @brief Resolves the live camera or raises a clear error.
	const Camera* Resolve() const
	{
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			throw std::runtime_error("neurus: camera proxy has no run context");
		}
		const Camera* cam = dynamic_cast<const Camera*>(scene->GetObjectID(uid));
		if (cam == nullptr)
		{
			throw std::runtime_error("neurus: camera proxy refers to a missing or mistyped object");
		}
		return cam;
	}
};

struct LightProxy : ProxyBase
{
	/// @brief Resolves the live light or raises a clear error.
	const Light* Resolve() const
	{
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			throw std::runtime_error("neurus: light proxy has no run context");
		}
		const Light* light = dynamic_cast<const Light*>(scene->GetObjectID(uid));
		if (light == nullptr)
		{
			throw std::runtime_error("neurus: light proxy refers to a missing or mistyped object");
		}
		return light;
	}
};

struct MeshProxy : ProxyBase
{
	/// @brief Resolves the live mesh or raises a clear error.
	const Mesh* Resolve() const
	{
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			throw std::runtime_error("neurus: mesh proxy has no run context");
		}
		const Mesh* mesh = dynamic_cast<const Mesh*>(scene->GetObjectID(uid));
		if (mesh == nullptr)
		{
			throw std::runtime_error("neurus: mesh proxy refers to a missing or mistyped object");
		}
		return mesh;
	}
};

struct EnvironmentProxy : ProxyBase
{
	/// @brief Resolves the live environment or raises a clear error.
	const Environment* Resolve() const
	{
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			throw std::runtime_error("neurus: environment proxy has no run context");
		}
		const Environment* env = dynamic_cast<const Environment*>(scene->GetObjectID(uid));
		if (env == nullptr)
		{
			throw std::runtime_error("neurus: environment proxy refers to a missing or mistyped object");
		}
		return env;
	}
};

/// @brief Stateless typed collection over one scene pool, keyed by UID.
template <typename Proxy, typename T>
struct TypedCollection
{
	using Pool = std::unordered_map<int, std::shared_ptr<T>>;

	const ScriptView* view = nullptr;
	Pool Scene::*     pool = nullptr;

	py::list Snapshot() const
	{
		py::list result;
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			return result;
		}
		const Pool& objects = scene->*(pool);
		for (const auto& [id, obj] : objects)
		{
			(void)obj;
			result.append(py::cast(Proxy{{id, view}}));
		}
		return result;
	}

	static py::object Lookup(Pool Scene::* pool, const ScriptView* view, int uid)
	{
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			return py::none();
		}
		const Pool& objects = scene->*(pool);
		if (objects.find(uid) == objects.end())
		{
			return py::none();
		}
		return py::cast(Proxy{{uid, view}});
	}
};

template <typename Proxy, typename T>
void RegisterCollection(py::module_& data, const char* name)
{
	using Collection = TypedCollection<Proxy, T>;
	py::class_<Collection>(data, (std::string("_") + name).c_str())
		.def("__iter__", [](const Collection& s) { return py::iter(s.Snapshot()); })
		.def("__len__", [](const Collection& s) { return py::len(s.Snapshot()); })
		.def("__getitem__",
		     [](const Collection& s, int uid) -> py::object
		     { return Collection::Lookup(s.pool, s.view, uid); });
}

/// @brief `neurus.context`: the run's active camera and selection.
struct ContextView
{
	const ScriptView* view = nullptr;
};

py::object ActiveCamera(const ContextView& self)
{
	const Scene* scene = (self.view != nullptr && self.view->ctx != nullptr) ? self.view->ctx->scene : nullptr;
	if (scene == nullptr)
	{
		return py::none();
	}
	// The scene owns the active camera; read it live (0 = none).
	const int activeId = scene->ActiveCameraID();
	if (activeId <= 0)
	{
		return py::none();
	}
	const ObjectID* obj = scene->GetObjectID(activeId);
	if (dynamic_cast<const Camera*>(obj) == nullptr)
	{
		return py::none();
	}
	return py::cast(CameraProxy{{activeId, self.view}});
}

py::list Selection(const ContextView& self)
{
	py::list result;
	const Scene* scene = (self.view != nullptr && self.view->ctx != nullptr) ? self.view->ctx->scene : nullptr;
	if (scene == nullptr)
	{
		return result;
	}
	// The scene owns the selection; snapshot its UIDs here (one source of truth).
	std::vector<int> selected;
	int activeUid = 0;
	SnapshotSelectionUids(scene->selections, selected, activeUid);
	for (const int uid : selected)
	{
		if (scene->GetObjectID(uid) != nullptr)
		{
			result.append(py::cast(ObjectProxy{{uid, self.view}}));
		}
	}
	return result;
}

}  // namespace

/// @brief Registers the scene proxy classes (once).
void RegisterSceneBindings(py::module_& root, py::module_& data)
{
	py::class_<CameraProxy>(data, "Camera", "Read-only proxy over a scene camera.")
		.def_property_readonly("uid", [](const CameraProxy& s) { return s.uid; })
		.def_property(
			"fov",
			[](const CameraProxy& s) { return s.Resolve()->cam_pers; },
			[](const CameraProxy& s, float value) { EmitEvent(s, CameraFovChanged{s.uid, value}); })
		.def_property_readonly("near", [](const CameraProxy& s) { return s.Resolve()->cam_near; })
		.def_property_readonly("far", [](const CameraProxy& s) { return s.Resolve()->cam_far; })
		.def_property(
			"position",
			[](const CameraProxy& s) { return ToTuple(s.Resolve()->GetPosition()); },
			[](const CameraProxy& s, std::tuple<float, float, float> value)
			{
				const Camera* cam = s.Resolve();
				const auto [x, y, z] = value;
				EmitEvent(s, CameraPoseChanged{s.uid, x, y, z,
				                              cam->cam_tar.x, cam->cam_tar.y, cam->cam_tar.z});
			})
		.def_property_readonly("rotation",
		     [](const CameraProxy& s) { return ToTuple(s.Resolve()->GetRotation()); })
		.def_property(
			"is_active",
			[](const CameraProxy& s)
			{
				const Scene* scene = (s.view != nullptr && s.view->ctx != nullptr) ? s.view->ctx->scene : nullptr;
				return scene != nullptr && scene->ActiveCameraID() == s.uid;
			},
			[](const CameraProxy& s, bool value) { EmitEvent(s, ActiveCameraChanged{value ? s.uid : 0}); });

	py::class_<LightProxy>(data, "Light", "Read-only proxy over a scene light.")
		.def_property_readonly("uid", [](const LightProxy& s) { return s.uid; })
		.def_property_readonly("type",
		     [](const LightProxy& s) { return static_cast<int>(s.Resolve()->light_type); })
		.def_property(
			"power",
			[](const LightProxy& s) { return s.Resolve()->light_power; },
			[](const LightProxy& s, float value) { EmitEvent(s, LightPowerChanged{s.uid, value}); })
		.def_property(
			"color",
			[](const LightProxy& s) { return ToTuple(s.Resolve()->light_color); },
			[](const LightProxy& s, std::tuple<float, float, float> value)
			{
				const auto [r, g, b] = value;
				EmitEvent(s, LightColorChanged{s.uid, r, g, b});
			})
		.def_property(
			"radius",
			[](const LightProxy& s) { return s.Resolve()->light_radius; },
			[](const LightProxy& s, float value) { EmitEvent(s, LightRadiusChanged{s.uid, value}); })
		.def_property(
			"shadow",
			[](const LightProxy& s) { return s.Resolve()->use_shadow; },
			[](const LightProxy& s, bool value) { EmitEvent(s, LightShadowChanged{s.uid, value}); })
		.def_property(
			"cutoff",
			[](const LightProxy& s) { return s.Resolve()->spot_cutoff; },
			[](const LightProxy& s, float value) { EmitEvent(s, LightCutoffChanged{s.uid, value}); })
		.def_property(
			"outer_cutoff",
			[](const LightProxy& s) { return s.Resolve()->spot_outer_cutoff; },
			[](const LightProxy& s, float value) { EmitEvent(s, LightOuterCutoffChanged{s.uid, value}); });

	py::class_<MeshProxy>(data, "Mesh", "Read-only proxy over a scene mesh.")
		.def_property_readonly("uid", [](const MeshProxy& s) { return s.uid; })
		.def_property(
			"shadow",
			[](const MeshProxy& s) { return s.Resolve()->using_shadow; },
			[](const MeshProxy& s, bool value) { EmitEvent(s, MeshShadowChanged{s.uid, value}); })
		.def_property(
			"using_material",
			[](const MeshProxy& s) { return s.Resolve()->using_material; },
			[](const MeshProxy& s, bool value) { EmitEvent(s, MeshMaterialChanged{s.uid, value}); })
		.def_property_readonly("material",
		     [](const MeshProxy& s) -> py::object
		     {
			     const std::shared_ptr<Material>& mat = s.Resolve()->o_material;
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
			[](const EnvironmentProxy& s) { return s.Resolve()->GetIntensity(); },
			[](const EnvironmentProxy& s, float value) { EmitEvent(s, EnvironmentIntensityChanged{s.uid, value}); })
		.def_property(
			"rotation",
			[](const EnvironmentProxy& s) { return s.Resolve()->GetRotation(); },
			[](const EnvironmentProxy& s, float value) { EmitEvent(s, EnvironmentRotationChanged{s.uid, value}); });

	RegisterCollection<CameraProxy, Camera>(data, "cameras");
	RegisterCollection<LightProxy, Light>(data, "lights");
	RegisterCollection<MeshProxy, Mesh>(data, "meshes");
	RegisterCollection<EnvironmentProxy, Environment>(data, "environments");

	py::class_<ContextView>(root, "_Context", "The run's active camera and selection.")
		.def_property_readonly("active_camera", [](const ContextView& s) { return ActiveCamera(s); })
		.def_property_readonly("selection", [](const ContextView& s) { return Selection(s); });
}

void InstallSceneViews(py::module_& root, py::module_& data, const ScriptView* view)
{
	data.attr("cameras") = py::cast(TypedCollection<CameraProxy, Camera>{view, &Scene::cam_list});
	data.attr("lights") = py::cast(TypedCollection<LightProxy, Light>{view, &Scene::light_list});
	data.attr("meshes") = py::cast(TypedCollection<MeshProxy, Mesh>{view, &Scene::mesh_list});
	data.attr("environments") = py::cast(TypedCollection<EnvironmentProxy, Environment>{view, &Scene::env_list});
	root.attr("context") = py::cast(ContextView{view});
}

}  // namespace script
}  // namespace neurus
