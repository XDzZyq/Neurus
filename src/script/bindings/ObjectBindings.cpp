/**
 * @file ObjectBindings.cpp
 * @brief Read-only `Object` / `Transform3D` proxies and `neurus.data.objects`.
 *
 * The collection is constructed once with the engine's `ScriptView`; the proxies
 * it hands out copy that view, so each one resolves its own object through the
 * run's resource pool on every access and nothing is cached. Iteration and UID
 * lookup go through the pool too, so a pooled object detached from the scene is
 * still reachable. No setter writes a C++ property - each constructs the exact
 * typed event and routes it through the run dispatch, which refuses in this
 * phase.
 */

#include <pybind11/pybind11.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

#include "core/IResourceLookup.h"
#include "editor/events/SceneEvents.h"
#include "scene/ObjectID.h"
#include "scene/Transform.h"
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

/**
 * @brief `neurus.data.objects`: every pooled object, iterable and keyed by UID.
 *
 * Constructed once with the engine's view. Iteration and UID lookup go through
 * the resource pool, not the scene, so a pooled object detached from the scene
 * (undo history) is still reachable. It is empty (never a null dereference) when
 * the run has no resource lookup, and it cannot observe a finished run because
 * the view it holds is only populated for the run that built it.
 *
 * Deliberately UID-only (D2): one name can span several object types, so a
 * string key would be ambiguous and returns None; the typed collections resolve
 * names through the registry.
 */
struct ObjectCollection
{
	const ScriptView* view = nullptr;

	/// @brief The run's resource pool, or nullptr when the run has no document.
	const IResourceLookup* Pool() const
	{
		return (view != nullptr && view->ctx != nullptr) ? view->ctx->resources : nullptr;
	}

	/// @brief Snapshot of every pooled object as an `Object` proxy list.
	py::list Snapshot() const
	{
		py::list result;
		const IResourceLookup* pool = Pool();
		if (pool == nullptr)
		{
			return result;
		}

		pool->ForEach<ObjectID>([this, &result](const std::shared_ptr<ObjectID>& obj)
		{
			result.append(py::cast(ObjectProxy{obj->GetObjectID(), view}));
		});
		return result;
	}

	/// @brief Number of pooled objects.
	py::ssize_t Count() const
	{
		py::ssize_t count = 0;
		const IResourceLookup* pool = Pool();
		if (pool != nullptr)
		{
			pool->ForEach<ObjectID>([&count](const std::shared_ptr<ObjectID>&) { ++count; });
		}
		return count;
	}
};

}  // namespace

/// @brief Registers the proxy classes (once) on the `data` submodule.
void RegisterObjectBindings(py::module_& data)
{
	py::class_<Transform3DProxy>(data, "Transform3D",
	                             "Read-only 3D transform shared by every spatial scene object.")
		.def_property(
			"position",
			[](const Transform3DProxy& self) { return ToTuple(self.Resolve()->GetPosition()); },
			[](const Transform3DProxy& self, std::tuple<float, float, float> value)
			{
				const auto [x, y, z] = value;
				EmitEvent(self, PositionChanged{self.uid, x, y, z});
			},
			"World position as an (x, y, z) tuple.")
		.def_property(
			"rotation",
			[](const Transform3DProxy& self) { return ToTuple(self.Resolve()->GetRotation()); },
			[](const Transform3DProxy& self, std::tuple<float, float, float> value)
			{
				const auto [x, y, z] = value;
				EmitEvent(self, RotationChanged{self.uid, x, y, z});
			},
			"Euler rotation in degrees as an (x, y, z) tuple.")
		.def_property(
			"scale",
			[](const Transform3DProxy& self) { return ToTuple(self.Resolve()->GetScale()); },
			[](const Transform3DProxy& self, std::tuple<float, float, float> value)
			{
				const auto [x, y, z] = value;
				EmitEvent(self, ScaleChanged{self.uid, x, y, z});
			},
			"Per-axis scale as an (x, y, z) tuple.");

	py::class_<ObjectProxy>(data, "Object", "Read-only proxy over a scene object.")
		.def_property_readonly(
			"uid",
			[](const ObjectProxy& self) { return self.uid; },
			"The object's unique ID.")
		.def_property_readonly(
			"name",
			[](const ObjectProxy& self) { return self.Resolve()->o_name; },
			"The object's display name.")
		.def_property_readonly(
			"type",
			[](const ObjectProxy& self) { return static_cast<int>(self.Resolve()->o_type); },
			"The object's `ObjectID::GOType` value.")
		.def_property(
			"visible",
			[](const ObjectProxy& self) { return self.Resolve()->is_viewport; },
			[](const ObjectProxy& self, bool value)
			{
				EmitEvent(self, VisibilityChanged{self.uid, value, value});
			},
			"Whether the object is visible in the viewport.")
		.def_property_readonly(
			"transform",
			[](const ObjectProxy& self) -> py::object
			{
				ObjectID* obj = const_cast<ObjectID*>(self.Resolve());
				if (obj->GetTransform() == nullptr)
				{
					return py::none();
				}
				return py::cast(Transform3DProxy{self.uid, self.view});
			},
			"The object's transform, or None when it has none.")
		.def("__repr__",
		     [](const ObjectProxy& self) { return "<neurus.Object uid=" + std::to_string(self.uid) + ">"; });

	py::class_<ObjectCollection>(data, "_Objects", "The pooled objects a script run may read.")
		.def("__iter__",
		     [](const ObjectCollection& self) { return py::iter(self.Snapshot()); },
		     "Iterates every pooled object, in unspecified order.")
		.def("__len__", [](const ObjectCollection& self) { return self.Count(); })
		.def("__getitem__",
		     [](const ObjectCollection& self, int uid) -> py::object
		     {
			     const IResourceLookup* pool = self.Pool();
			     if (pool == nullptr || pool->Get<ObjectID>(uid) == nullptr)
			     {
				     return py::none();
			     }
			     return py::cast(ObjectProxy{uid, self.view});
		     },
		     "Looks an object up by UID, or returns None when it is absent.")
		.def("__getitem__",
		     [](const ObjectCollection&, const std::string&) -> py::object
		     {
			     // D2: a name spans several object types and is ambiguous, so a
			     // string key never resolves here; typed collections do names.
			     return py::none();
		     },
		     "Always None: use a UID, or a typed collection for a name.")
		.def("__contains__",
		     [](const ObjectCollection& self, int uid)
		     {
			     const IResourceLookup* pool = self.Pool();
			     return pool != nullptr && pool->Get<ObjectID>(uid) != nullptr;
		     },
		     "True when a pooled object with the given UID exists.")
		.def("__repr__",
		     [](const ObjectCollection& self)
		     { return "<neurus.data.objects len=" + std::to_string(self.Count()) + ">"; });
}

void InstallObjectViews(py::module_& data, const ScriptView* view)
{
	data.attr("objects") = py::cast(ObjectCollection{view});
}

}  // namespace script
}  // namespace neurus
