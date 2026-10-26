/**
 * @file SceneBindings.cpp
 * @brief Read-only camera/light/mesh/environment proxies, the pool-wide
 *        `neurus.data` collections, and `neurus.context`.
 *
 * Every proxy is constructed with the run's `ScriptView` and resolves its own
 * object through the run's resource pool on each access; nothing is cached and
 * no view or proxy holds a run-scoped pointer. Two kinds of collection coexist:
 * the typed `neurus.data` collections enumerate the whole pool, so a pooled
 * object detached from the scene (undo history) stays reachable, while
 * `neurus.data.scene.*` enumerate the scene's own pools and exclude detached
 * objects. The typed collections resolve names through the pool's per-type
 * registry; the generic `objects` pools are UID-only. The scene is read only for
 * state it alone owns - camera activation, selection and scene membership. The
 * pool-wide collections need only the resource lookup; the scene-scoped ones
 * need only the scene (a name lookup additionally needs the pool). Collections
 * are empty - never a null dereference - when the state they read is absent. No
 * setter writes a C++ property (each routes the exact typed event through the
 * run dispatch, which refuses in this phase).
 */

#include <pybind11/pybind11.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/IResourceLookup.h"
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
#include "script/proxies/ObjectProxy.h"
#include "script/proxies/ProxyBase.h"
#include "script/proxies/Transform3DProxy.h"

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

/// @brief The run's resource pool, or nullptr when the run has no document.
const IResourceLookup* PoolOf(const ScriptView* view)
{
	return (view != nullptr && view->ctx != nullptr) ? view->ctx->resources : nullptr;
}

/**
 * @brief Resolves a pooled object of dynamic type @p T for a typed proxy.
 *
 * The pool owns the object, so the returned raw pointer stays valid after the
 * temporary shared_ptr is destroyed. Throws a per-proxy error when the run has
 * no pool, or the UID is absent from it or of another type.
 *
 * @tparam T Concrete pooled type (Camera, Light, Mesh, Environment).
 * @param view The proxy's run view.
 * @param uid Pooled object UID.
 * @param what Lower-case proxy type name used in the error message.
 */
template <typename T>
const T* ResolvePooled(const ScriptView* view, int uid, const char* what)
{
	const IResourceLookup* pool = PoolOf(view);
	if (pool == nullptr)
	{
		throw std::runtime_error(std::string("neurus: ") + what + " proxy has no resource lookup");
	}

	const std::shared_ptr<T> obj = pool->Get<T>(uid);
	if (!obj)
	{
		throw std::runtime_error(std::string("neurus: ") + what + " proxy refers to a missing or mistyped object");
	}
	return obj.get();
}

struct CameraProxy : ObjectProxy, Transform3DProxy
{
	CameraProxy() = default;

	CameraProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/// @brief Resolves the live camera or raises a clear error.
	const Camera* Resolve() const
	{
		return ResolvePooled<Camera>(view, uid, "camera");
	}
};

struct LightProxy : ObjectProxy, Transform3DProxy
{
	LightProxy() = default;

	LightProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/// @brief Resolves the live light or raises a clear error.
	const Light* Resolve() const
	{
		return ResolvePooled<Light>(view, uid, "light");
	}
};

struct MeshProxy : ObjectProxy, Transform3DProxy
{
	MeshProxy() = default;

	MeshProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/// @brief Resolves the live mesh or raises a clear error.
	const Mesh* Resolve() const
	{
		return ResolvePooled<Mesh>(view, uid, "mesh");
	}
};

struct EnvironmentProxy : ObjectProxy, Transform3DProxy
{
	EnvironmentProxy() = default;

	EnvironmentProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/// @brief Resolves the live environment or raises a clear error.
	const Environment* Resolve() const
	{
		return ResolvePooled<Environment>(view, uid, "environment");
	}
};

/**
 * @brief Stateless typed collection over one scene pool, keyed by UID (and, for
 *        the typed pools, by registered name).
 *
 * Iteration and UID lookup read the scene's own typed pool, so a pooled object
 * detached from the scene is not visible. A typed pool also accepts a registered
 * name: it resolves through the pool's per-type registry (authoritative for
 * names) and is then checked against the scene pool, so a name that is pooled but
 * not part of the scene, absent, or owned by another type yields None. The
 * generic `objects` pool is UID-only because one name spans several types.
 * Iteration and UID lookup need only the scene; a name lookup also needs the
 * pool, so it yields None without one. Empty - never a null dereference - when
 * the run has no scene.
 *
 * @tparam Proxy Typed proxy handed out for each scene object.
 * @tparam T Scene object type (Mesh, Camera, ... or ObjectID for `objects`).
 */
template <typename Proxy, typename T>
struct SceneCollection
{
	using Pool = std::unordered_map<int, std::shared_ptr<T>>;

	const ScriptView* view = nullptr;  ///< Constructing run's view.
	Pool Scene::*     pool = nullptr;  ///< Scene pool this collection reads.

	/// @brief The run's scene, or nullptr when the run has no document.
	const Scene* SceneOf() const
	{
		return (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
	}

	/// @brief Snapshot of every object in this scene pool as a proxy list.
	py::list Snapshot() const
	{
		py::list result;
		const Scene* scene = SceneOf();
		if (scene == nullptr)
		{
			return result;
		}
		const Pool& objects = scene->*(pool);
		for (const auto& [id, obj] : objects)
		{
			(void)obj;
			result.append(py::cast(Proxy{id, view}));
		}
		return result;
	}

	/// @brief Number of objects in this scene pool.
	py::ssize_t Count() const
	{
		const Scene* scene = SceneOf();
		return scene != nullptr ? static_cast<py::ssize_t>((scene->*(pool)).size()) : 0;
	}

	/// @brief Proxy for a UID in this scene pool, or None when it is not.
	py::object Lookup(int uid) const
	{
		const Scene* scene = SceneOf();
		if (scene == nullptr)
		{
			return py::none();
		}
		const Pool& objects = scene->*(pool);
		if (objects.find(uid) == objects.end())
		{
			return py::none();
		}
		return py::cast(Proxy{uid, view});
	}

	/// @brief True when this scene pool holds the given UID.
	bool Contains(int uid) const
	{
		const Scene* scene = SceneOf();
		return scene != nullptr && (scene->*(pool)).count(uid) != 0;
	}

	/**
	 * @brief Proxy for a registered name that is also in this scene pool.
	 *
	 * The registry, not the scene, is authoritative for names: a name unknown to
	 * the pool is None, and so is a pooled object that the scene does not use or
	 * whose name belongs to another object type.
	 */
	py::object LookupName(const std::string& name) const
	{
		const Scene* scene = SceneOf();
		const IResourceLookup* lookup = PoolOf(view);
		if (scene == nullptr || lookup == nullptr)
		{
			return py::none();
		}
		const std::shared_ptr<T> obj = lookup->GetByName<T>(name);
		if (!obj)
		{
			return py::none();
		}
		const Pool& objects = scene->*(pool);
		if (objects.find(obj->GetObjectID()) == objects.end())
		{
			return py::none(); // pooled, but the scene does not use it
		}
		return py::cast(Proxy{obj->GetObjectID(), view});
	}
};

template <typename Proxy, typename T>
void RegisterSceneCollection(py::module_& data, const char* name, bool resolvesNames)
{
	using Collection = SceneCollection<Proxy, T>;
	py::class_<Collection> cls(data, (std::string("_Scene") + name).c_str(),
	                           resolvesNames
	                               ? "A scene pool of a script run, keyed by UID or registered name."
	                               : "A scene pool of a script run, keyed by UID only.");
	cls.def("__iter__", [](const Collection& s) { return py::iter(s.Snapshot()); })
		.def("__len__", [](const Collection& s) { return s.Count(); })
		.def("__getitem__", [](const Collection& s, int uid) -> py::object { return s.Lookup(uid); })
		.def("__contains__", [](const Collection& s, int uid) { return s.Contains(uid); },
		     "True when this scene pool holds the given UID.");
	if (resolvesNames)
	{
		cls.def("__getitem__",
		        [](const Collection& s, const std::string& name) -> py::object { return s.LookupName(name); })
			.def("__contains__",
			     [](const Collection& s, const std::string& name) { return !s.LookupName(name).is_none(); },
			     "True when a registered name resolves to a scene object.");
	}
	else
	{
		// D2: `objects` is UID-only - a name spans several object types and is
		// ambiguous, so the typed scene collections resolve names instead.
		cls.def("__getitem__",
		        [](const Collection&, const std::string&) -> py::object { return py::none(); })
			.def("__contains__",
			     [](const Collection&, const std::string&) { return false; },
			     "Always False: this collection is UID-only.");
	}
}

/// @brief `neurus.data.scene`: the collections of the run's scene, not the pool.
struct SceneProxy : ProxyBase
{
	/// @brief Meshes the scene uses.
	py::object Meshes() const { return py::cast(SceneCollection<MeshProxy, Mesh>{view, &Scene::mesh_list}); }

	/// @brief Cameras the scene uses.
	py::object Cameras() const { return py::cast(SceneCollection<CameraProxy, Camera>{view, &Scene::cam_list}); }

	/// @brief Lights the scene uses.
	py::object Lights() const { return py::cast(SceneCollection<LightProxy, Light>{view, &Scene::light_list}); }

	/// @brief Environments the scene uses.
	py::object Environments() const
	{
		return py::cast(SceneCollection<EnvironmentProxy, Environment>{view, &Scene::env_list});
	}

	/// @brief Every scene object, keyed by UID only (D2).
	py::object Objects() const { return py::cast(SceneCollection<ObjectProxy, ObjectID>{view, &Scene::obj_list}); }
};

/**
 * @brief Stateless typed collection over the run's whole resource pool.
 *
 * Iteration and lookups go through `ScriptContext::resources`, not the scene, so
 * a pooled object detached from the scene (undo history) is still reachable. A
 * name resolves through the pool's per-type registry, which is authoritative for
 * lookup; a proxy's display `name` still reads `ObjectID::o_name`. Empty - never
 * a null dereference - when the run has no resource lookup.
 *
 * @tparam Proxy Typed proxy handed out for each pooled object.
 * @tparam T Pooled type to enumerate and look up (Mesh, Camera, ...).
 */
template <typename Proxy, typename T>
struct ResourceCollection
{
	const ScriptView* view = nullptr;

	/// @brief The run's resource pool, or nullptr when the run has no document.
	const IResourceLookup* Pool() const
	{
		return PoolOf(view);
	}

	/// @brief Snapshot of every pooled object of type @p T as a proxy list.
	py::list Snapshot() const
	{
		py::list result;
		const IResourceLookup* pool = Pool();
		if (pool == nullptr)
		{
			return result;
		}
		pool->ForEach<T>([this, &result](const std::shared_ptr<T>& obj)
		{
			result.append(py::cast(Proxy{obj->GetObjectID(), view}));
		});
		return result;
	}

	/// @brief Number of pooled objects of type @p T.
	py::ssize_t Count() const
	{
		py::ssize_t count = 0;
		const IResourceLookup* pool = Pool();
		if (pool != nullptr)
		{
			pool->ForEach<T>([&count](const std::shared_ptr<T>&) { ++count; });
		}
		return count;
	}

	/// @brief Proxy for a pooled UID, or None when absent or of another type.
	py::object Lookup(int uid) const
	{
		const IResourceLookup* pool = Pool();
		if (pool == nullptr || pool->Get<T>(uid) == nullptr)
		{
			return py::none();
		}
		return py::cast(Proxy{uid, view});
	}

	/// @brief True when a pooled object of type @p T has this UID.
	bool Contains(int uid) const
	{
		const IResourceLookup* pool = Pool();
		return pool != nullptr && pool->Get<T>(uid) != nullptr;
	}

	/// @brief Proxy for a registered name, or None when the name is absent.
	py::object LookupName(const std::string& name) const
	{
		const IResourceLookup* pool = Pool();
		if (pool == nullptr)
		{
			return py::none();
		}
		const std::shared_ptr<T> obj = pool->GetByName<T>(name);
		if (!obj)
		{
			return py::none();
		}
		return py::cast(Proxy{obj->GetObjectID(), view});
	}
};

template <typename Proxy, typename T>
void RegisterResourceCollection(py::module_& data, const char* name)
{
	using Collection = ResourceCollection<Proxy, T>;
	py::class_<Collection>(data, (std::string("_") + name).c_str())
		.def("__iter__", [](const Collection& s) { return py::iter(s.Snapshot()); })
		.def("__len__", [](const Collection& s) { return s.Count(); })
		.def("__getitem__", [](const Collection& s, int uid) -> py::object { return s.Lookup(uid); })
		.def("__contains__", [](const Collection& s, int uid) { return s.Contains(uid); },
		     "True when a pooled object of this type has the given UID.")
		.def("__getitem__",
		     [](const Collection& s, const std::string& key) -> py::object { return s.LookupName(key); })
		.def("__contains__",
		     [](const Collection& s, const std::string& name) { return !s.LookupName(name).is_none(); },
		     "True when a registered name resolves to a pooled object of this type.");
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
	// The scene owns activation and validates the UID against its camera pool, so
	// a stale activation left by a removed camera reads as "none".
	const Camera* active = scene->GetActiveCamera();
	if (active == nullptr)
	{
		return py::none();
	}
	// The object itself resolves through the run's pool.
	const int activeId = active->GetObjectID();
	const IResourceLookup* pool = PoolOf(self.view);
	if (pool == nullptr || !pool->Get<Camera>(activeId))
	{
		return py::none();
	}
	return py::cast(CameraProxy{activeId, self.view});
}

py::list Selection(const ContextView& self)
{
	py::list result;
	const Scene* scene = (self.view != nullptr && self.view->ctx != nullptr) ? self.view->ctx->scene : nullptr;
	if (scene == nullptr)
	{
		return result;
	}
	const IResourceLookup* pool = PoolOf(self.view);
	if (pool == nullptr)
	{
		return result;
	}
	// The scene owns the selection; snapshot its UIDs here (one source of truth).
	std::vector<int> selected;
	int activeUid = 0;
	SnapshotSelectionUids(scene->selections, selected, activeUid);
	for (const int uid : selected)
	{
		if (pool->Get<ObjectID>(uid) != nullptr)
		{
			result.append(py::cast(ObjectProxy{uid, self.view}));
		}
	}
	return result;
}

}  // namespace

/// @brief Registers the scene proxy classes (once).
void RegisterSceneBindings(py::module_& root, py::module_& data)
{
	py::class_<CameraProxy, ObjectProxy, Transform3DProxy>(data, "Camera", "Read-only proxy over a scene camera.")
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

	py::class_<LightProxy, ObjectProxy, Transform3DProxy>(data, "Light", "Read-only proxy over a scene light.")
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

	py::class_<MeshProxy, ObjectProxy, Transform3DProxy>(data, "Mesh", "Read-only proxy over a scene mesh.")
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

	py::class_<EnvironmentProxy, ObjectProxy, Transform3DProxy>(data, "Environment", "Read-only proxy over the scene environment.")
		.def_property(
			"intensity",
			[](const EnvironmentProxy& s) { return s.Resolve()->GetIntensity(); },
			[](const EnvironmentProxy& s, float value) { EmitEvent(s, EnvironmentIntensityChanged{s.uid, value}); })
		.def_property(
			"rotation",
			[](const EnvironmentProxy& s) { return s.Resolve()->GetRotation(); },
			[](const EnvironmentProxy& s, float value) { EmitEvent(s, EnvironmentRotationChanged{s.uid, value}); });

	RegisterResourceCollection<CameraProxy, Camera>(data, "cameras");
	RegisterResourceCollection<LightProxy, Light>(data, "lights");
	RegisterResourceCollection<MeshProxy, Mesh>(data, "meshes");
	RegisterResourceCollection<EnvironmentProxy, Environment>(data, "environments");

	// Scene-scoped collections: registered before any `_Scene` instance exists.
	RegisterSceneCollection<MeshProxy, Mesh>(data, "Meshes", true);
	RegisterSceneCollection<CameraProxy, Camera>(data, "Cameras", true);
	RegisterSceneCollection<LightProxy, Light>(data, "Lights", true);
	RegisterSceneCollection<EnvironmentProxy, Environment>(data, "Environments", true);
	RegisterSceneCollection<ObjectProxy, ObjectID>(data, "Objects", false);

	py::class_<SceneProxy>(data, "_Scene", "The run's scene collections.")
		.def_property_readonly("meshes", [](const SceneProxy& s) { return s.Meshes(); },
		                       "Meshes the scene uses.")
		.def_property_readonly("cameras", [](const SceneProxy& s) { return s.Cameras(); },
		                       "Cameras the scene uses.")
		.def_property_readonly("lights", [](const SceneProxy& s) { return s.Lights(); },
		                       "Lights the scene uses.")
		.def_property_readonly("environments", [](const SceneProxy& s) { return s.Environments(); },
		                       "Environments the scene uses.")
		.def_property_readonly("objects", [](const SceneProxy& s) { return s.Objects(); },
		                       "Every object the scene uses, keyed by UID only.");

	py::class_<ContextView>(root, "_Context", "The run's active camera and selection.")
		.def_property_readonly("active_camera", [](const ContextView& s) { return ActiveCamera(s); })
		.def_property_readonly("selection", [](const ContextView& s) { return Selection(s); });
}

void InstallSceneViews(py::module_& root, py::module_& data, const ScriptView* view)
{
	data.attr("cameras") = py::cast(ResourceCollection<CameraProxy, Camera>{view});
	data.attr("lights") = py::cast(ResourceCollection<LightProxy, Light>{view});
	data.attr("meshes") = py::cast(ResourceCollection<MeshProxy, Mesh>{view});
	data.attr("environments") = py::cast(ResourceCollection<EnvironmentProxy, Environment>{view});
	data.attr("scene") = py::cast(SceneProxy{{0, view}});
	root.attr("context") = py::cast(ContextView{view});
}

}  // namespace script
}  // namespace neurus
