/**
 * @file SceneBindings.cpp
 * @brief `neurus.scene`: the collections of the run's current scene, plus its
 *        live active camera and selection.
 *
 * Every collection is constructed with the run's `ScriptView` and resolves its
 * own object through the run's resource pool on each access; nothing is cached
 * and no view or collection holds a run-scoped pointer. Unlike the pool-wide
 * `neurus.data` collections, these enumerate the scene's own typed pools, so a
 * pooled object detached from the scene (undo history) is not listed. The typed
 * collections resolve names through the pool's per-type registry (authoritative
 * for names) and then check scene membership; the generic `objects` pool is
 * UID-only because one name spans several types. The scene is read only for
 * state it alone owns - camera activation, selection and scene membership.
 * Collections are empty - never a null dereference - when the state they read is
 * absent. `active_camera()` and `selection()` are functions rather than
 * properties so neither can be snapshotted: each call reads the live scene.
 */

#include <pybind11/pybind11.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/IResourceLookup.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/ObjectID.h"
#include "scene/Scene.h"
#include "script/ScriptContext.h"
#include "script/bindings/Bindings.h"
#include "script/proxies/ObjectProxy.h"
#include "script/proxies/ProxyBase.h"
#include "script/proxies/SceneProxies.h"
#include "script/proxies/Transform3DProxy.h"

namespace py = pybind11;

namespace neurus
{
namespace script
{
namespace
{

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
void RegisterSceneCollection(py::module_& scene, const char* name, bool resolvesNames)
{
	using Collection = SceneCollection<Proxy, T>;
	py::class_<Collection> cls(scene, (std::string("_Scene") + name).c_str(),
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

/**
 * @brief Internal helper behind `neurus.scene.active_camera()` and `.selection()`.
 *
 * Deliberately not a registered Python class: the scene module binds the two
 * functions to lambdas that construct one with the run's view, so neither value
 * can be captured and stale.
 */
struct ContextView
{
	const ScriptView* view = nullptr;  ///< Constructing run's view.

	/// @brief Live active camera proxy, or None when none is active.
	py::object ActiveCamera() const
	{
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			return py::none();
		}
		// The scene owns activation and validates the UID against its camera pool,
		// so a stale activation left by a removed camera reads as "none".
		const Camera* active = scene->GetActiveCamera();
		if (active == nullptr)
		{
			return py::none();
		}
		// The object itself resolves through the run's pool.
		const int activeId = active->GetObjectID();
		const IResourceLookup* pool = PoolOf(view);
		if (pool == nullptr || !pool->Get<Camera>(activeId))
		{
			return py::none();
		}
		return py::cast(CameraProxy{activeId, view});
	}

	/// @brief Live selection as a list of `Object` proxies, empty when there is none.
	py::list Selection() const
	{
		py::list result;
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			return result;
		}
		const IResourceLookup* pool = PoolOf(view);
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
				result.append(py::cast(ObjectProxy{uid, view}));
			}
		}
		return result;
	}
};

}  // namespace

/// @brief Registers the scene-scoped collection classes (once).
void RegisterSceneBindings(py::module_& scene)
{
	RegisterSceneCollection<MeshProxy, Mesh>(scene, "Meshes", true);
	RegisterSceneCollection<CameraProxy, Camera>(scene, "Cameras", true);
	RegisterSceneCollection<LightProxy, Light>(scene, "Lights", true);
	RegisterSceneCollection<EnvironmentProxy, Environment>(scene, "Environments", true);
	RegisterSceneCollection<ObjectProxy, ObjectID>(scene, "Objects", false);
}

/**
 * @brief Installs the scene collections and the two live scene functions.
 *
 * The functions are bound here, not in `RegisterSceneBindings`, because they
 * capture the interpreter's `ScriptView`; the view's address is stable for the
 * interpreter's lifetime, so the captured pointer never dangles.
 */
void InstallSceneViews(py::module_& scene, const ScriptView* view)
{
	scene.attr("objects") = py::cast(SceneCollection<ObjectProxy, ObjectID>{view, &Scene::obj_list});
	scene.attr("cameras") = py::cast(SceneCollection<CameraProxy, Camera>{view, &Scene::cam_list});
	scene.attr("lights") = py::cast(SceneCollection<LightProxy, Light>{view, &Scene::light_list});
	scene.attr("meshes") = py::cast(SceneCollection<MeshProxy, Mesh>{view, &Scene::mesh_list});
	scene.attr("environments") = py::cast(SceneCollection<EnvironmentProxy, Environment>{view, &Scene::env_list});

	scene.def("active_camera", [view]() -> py::object { return ContextView{view}.ActiveCamera(); },
	          "The scene's active camera as a Camera proxy, or None when none is active.");
	scene.def("selection", [view]() { return ContextView{view}.Selection(); },
	          "The scene's selected objects as a list of Object proxies.");
}

}  // namespace script
}  // namespace neurus
