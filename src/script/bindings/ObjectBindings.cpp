/**
 * @file ObjectBindings.cpp
 * @brief Read-only `Object` / `Transform3D` proxies and `neurus.data.objects`.
 *
 * The collection is constructed once per run with the run's `ScriptView`; the
 * proxies it hands out copy that view, so each one resolves its own object and
 * nothing is cached. No setter writes a C++ property - each constructs the
 * exact typed event and routes it through the run dispatch, which refuses in
 * this phase.
 */

#include <pybind11/pybind11.h>

#include <stdexcept>
#include <string>
#include <tuple>

#include "editor/events/SceneEvents.h"
#include "scene/ObjectID.h"
#include "scene/Scene.h"
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
 * @brief `neurus.data.objects`: all master-pool objects, iterable and keyed by
 *        UID.
 *
 * Constructed once per run with that run's view. It is empty (never a null
 * dereference) when the run has no scene, and it cannot observe a finished run
 * because the view it holds is only populated for the run that built it.
 */
struct ObjectCollection
{
	const ScriptView* view = nullptr;

	/// @brief Snapshot of every master-pool object as an `Object` proxy list.
	py::list Snapshot() const
	{
		py::list result;
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			return result;
		}

		for (const auto& [id, obj] : scene->obj_list)
		{
			(void)obj;
			result.append(py::cast(ObjectProxy{id, view}));
		}
		return result;
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

	py::class_<ObjectCollection>(data, "_Objects", "The master scene object pool.")
		.def("__iter__",
		     [](const ObjectCollection& self) { return py::iter(self.Snapshot()); },
		     "Iterates the objects, newest registration order unspecified.")
		.def("__len__", [](const ObjectCollection& self) { return py::len(self.Snapshot()); })
		.def("__getitem__",
		     [](const ObjectCollection& self, int uid) -> py::object
		     {
			     const Scene* scene =
			         (self.view != nullptr && self.view->ctx != nullptr) ? self.view->ctx->scene : nullptr;
			     if (scene == nullptr || scene->GetObjectID(uid) == nullptr)
			     {
				     return py::none();
			     }
			     return py::cast(ObjectProxy{uid, self.view});
		     },
		     "Looks an object up by UID, or returns None when it is absent.")
		.def("__getitem__",
		     [](const ObjectCollection& self, const std::string& name) -> py::object
		     {
			     const Scene* scene =
			         (self.view != nullptr && self.view->ctx != nullptr) ? self.view->ctx->scene : nullptr;
			     if (scene == nullptr)
			     {
				     return py::none();
			     }
			     const ObjectID* obj = scene->FindObjectByName(name);
			     if (obj == nullptr)
			     {
				     return py::none();
			     }
			     return py::cast(ObjectProxy{obj->GetObjectID(), self.view});
		     },
		     "Looks an object up by display name, or returns None when it is absent.")
		.def("__contains__",
		     [](const ObjectCollection& self, int uid)
		     {
			     const Scene* scene =
			         (self.view != nullptr && self.view->ctx != nullptr) ? self.view->ctx->scene : nullptr;
			     return scene != nullptr && scene->GetObjectID(uid) != nullptr;
		     },
		     "True when an object with the given UID exists.")
		.def("__repr__",
		     [](const ObjectCollection& self) { return "<neurus.data.objects len=" + std::to_string(py::len(self.Snapshot())) + ">"; });
}

void InstallObjectViews(py::module_& data, const ScriptView* view)
{
	data.attr("objects") = py::cast(ObjectCollection{view});
}

}  // namespace script
}  // namespace neurus
