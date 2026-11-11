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

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/IResourceLookup.h"
#include "editor/Input.h"
#include "editor/events/SceneEvents.h"
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
 * @brief Extracts object UIDs from one proxy or any iterable of proxies.
 *
 * A single proxy is not iterable; a list, tuple or generator is. Casting each
 * item to ObjectProxy accepts every typed proxy through its registered base.
 */
std::vector<int> ObjectUids(const py::object& objects)
{
	std::vector<int> uids;
	if (objects.is_none())
		return uids;

	if (PyObject_HasAttrString(objects.ptr(), "__iter__"))
	{
		for (py::handle item : py::reinterpret_borrow<py::iterable>(objects))
			uids.push_back(py::cast<ObjectProxy>(item).uid);
	}
	else
	{
		uids.push_back(py::cast<ObjectProxy>(objects).uid);
	}
	return uids;
}

/**
 * @brief Wraps a newly created pooled object in its typed Python proxy.
 */
py::object ProxyForUid(const ScriptView* view, int uid)
{
	const IResourceLookup* pool = PoolOf(view);
	if (pool == nullptr)
		return py::cast(ObjectProxy{uid, view});
	if (pool->Get<Mesh>(uid))
		return py::cast(MeshProxy{uid, view});
	if (pool->Get<Camera>(uid))
		return py::cast(CameraProxy{uid, view});
	if (pool->Get<Light>(uid))
		return py::cast(LightProxy{uid, view});
	if (pool->Get<Environment>(uid))
		return py::cast(EnvironmentProxy{uid, view});
	return py::cast(ObjectProxy{uid, view});
}

/**
 * @brief Internal helper behind `neurus.scene.active_camera()` and `.selection()`.
 *
 * Deliberately not a registered Python class: the scene module binds the two
 * functions to lambdas that construct one with the run's view, so neither value
 * can be captured and stale.
 */
struct ContextProxy
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

	scene.def("active_camera", [view]() -> py::object { return ContextProxy{view}.ActiveCamera(); },
	          "The scene's active camera as a Camera proxy, or None when none is active.");
	scene.def("selection", [view]() { return ContextProxy{view}.Selection(); },
	          "The scene's selected objects as a list of Object proxies.");

	scene.def(
		"select",
		[view](const py::object& objects, bool additive)
		{
			const std::vector<int> uids = ObjectUids(objects);
			// Reuse the live click event instead of a separate set-write event:
			// the first UID replaces the selection (or adds, when additive), every
			// later one adds with Shift, so the last UID becomes active. That is
			// exactly the set a click gesture builds, and each event records its
			// own SetSelectionOp - all folded into the run's single undo entry.
			if (uids.empty())
			{
				if (!additive)
					EmitEvent(ProxyBase{0, view}, ObjectSelected{0, 0}); // clear
				return;
			}
			EmitEvent(ProxyBase{0, view},
			          ObjectSelected{uids.front(), additive ? Input::Mod_Shift : 0});
			for (std::size_t i = 1; i < uids.size(); ++i)
				EmitEvent(ProxyBase{0, view}, ObjectSelected{uids[i], Input::Mod_Shift});
		},
		py::arg("objects"), py::arg("additive") = false,
		"Select one object or an iterable of objects; additive appends to the current set.");

	scene.def(
		"clear_selection",
		[view]() { EmitEvent(ProxyBase{0, view}, ObjectSelected{0, 0}); },
		"Clear the scene selection.");

	scene.def(
		"delete",
		[view](const py::object& objects)
		{
			const std::vector<int> uids = ObjectUids(objects);
			if (uids.empty())
				return;
			EmitEvent(ProxyBase{0, view}, DeleteObjectsRequested{uids});
		},
		py::arg("objects"),
		"Remove one object or an iterable of objects from the scene as one undo entry.");

	scene.def(
		"import_mesh",
		[view](const std::string& path, const std::string& name) -> py::object
		{
			// The Editor owns the pool, so creation is requested through a typed
			// event; the reply slot is borrowed for the synchronous dispatch.
			ObjectCreateResult result;
			EmitEvent(ProxyBase{0, view}, MeshImportRequested{path, name, &result});
			if (result.uid == 0)
				throw std::runtime_error(result.error.empty() ? "neurus: mesh import failed" : result.error);
			return ProxyForUid(view, result.uid);
		},
		py::arg("path"), py::arg("name") = std::string{},
		"Load a mesh file, add it to the scene and return its Mesh proxy.");

	scene.def(
		"add_camera",
		[view](const std::string& name) -> py::object
		{
			ObjectCreateResult result;
			EmitEvent(ProxyBase{0, view}, CameraAddRequested{name, &result});
			if (result.uid == 0)
				throw std::runtime_error(result.error.empty() ? "neurus: camera creation failed" : result.error);
			return ProxyForUid(view, result.uid);
		},
		py::arg("name") = std::string{},
		"Create a camera, add it to the scene and return its Camera proxy.");

	scene.def(
		"duplicate",
		[view](const py::object& object, const std::string& name) -> py::object
		{
			const ObjectProxy source = py::cast<ObjectProxy>(object);
			ObjectCreateResult result;
			EmitEvent(ProxyBase{0, view}, ObjectDuplicateRequested{source.uid, name, &result});
			if (result.uid == 0)
				throw std::runtime_error(result.error.empty() ? "neurus: duplication failed" : result.error);
			return ProxyForUid(view, result.uid);
		},
		py::arg("object"), py::arg("name") = std::string{},
		"Duplicate a pooled mesh or camera, add it to the scene and return its proxy.");
}

}  // namespace script
}  // namespace neurus
