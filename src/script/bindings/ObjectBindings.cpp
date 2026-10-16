/**
 * @file ObjectBindings.cpp
 * @brief Read-only `Object` / `Transform` proxies and `neurus.data.objects`.
 *
 * The collection and every proxy are bound to the run's `ScriptExecutor`; every
 * property is resolved live through it on attribute access. No setter writes a
 * C++ property - each constructs the exact typed event and routes it through
 * the run dispatch, which refuses in this phase. Nothing is cached, so a proxy
 * never observes a stale scene.
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
#include "script/bindings/Proxies.h"
#include "script/bindings/ProxyBase.h"

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
 * An instance is bound to one run's executor and replaced at the start of the
 * next run, so it can never read a finished run. It is empty (never a null
 * dereference) when the run has no scene.
 */
struct ObjectCollection
{
	const ScriptExecutor* exec = nullptr;

	/// @brief Snapshot of every master-pool object as an `Object` proxy list.
	py::list Snapshot() const
	{
		py::list result;
		if (exec == nullptr || exec->context().scene == nullptr)
		{
			return result;
		}

		for (const auto& [id, obj] : exec->context().scene->obj_list)
		{
			(void)obj;
			result.append(py::cast(ObjectProxy{exec, id}));
		}
		return result;
	}
};

}  // namespace

/// @brief Registers the proxy classes (once) on the `data` submodule.
void RegisterObjectBindings(py::module_& data)
{
	py::class_<TransformProxy>(data, "Transform", "Read-only 3D transform of a scene object.")
		.def_property(
			"position",
			[](const TransformProxy& self) { return ToTuple(self.Resolve()->GetPosition()); },
			[](const TransformProxy& self, std::tuple<float, float, float> value)
			{
				const auto [x, y, z] = value;
				EmitEvent(self, PositionChanged{self.uid, x, y, z});
			},
			"World position as an (x, y, z) tuple.")
		.def_property(
			"rotation",
			[](const TransformProxy& self) { return ToTuple(self.Resolve()->GetRotation()); },
			[](const TransformProxy& self, std::tuple<float, float, float> value)
			{
				const auto [x, y, z] = value;
				EmitEvent(self, RotationChanged{self.uid, x, y, z});
			},
			"Euler rotation in degrees as an (x, y, z) tuple.")
		.def_property(
			"scale",
			[](const TransformProxy& self) { return ToTuple(self.Resolve()->GetScale()); },
			[](const TransformProxy& self, std::tuple<float, float, float> value)
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
				return py::cast(TransformProxy{self.exec, self.uid});
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
			     if (self.exec == nullptr || self.exec->context().scene == nullptr
			         || self.exec->context().scene->GetObjectID(uid) == nullptr)
			     {
				     return py::none();
			     }
			     return py::cast(ObjectProxy{self.exec, uid});
		     },
		     "Looks an object up by UID, or returns None when it is absent.")
		.def("__contains__",
		     [](const ObjectCollection& self, int uid)
		     {
			     return self.exec != nullptr && self.exec->context().scene != nullptr
			            && self.exec->context().scene->GetObjectID(uid) != nullptr;
		     },
		     "True when an object with the given UID exists.")
		.def("__repr__",
		     [](const ObjectCollection& self) { return "<neurus.data.objects len=" + std::to_string(py::len(self.Snapshot())) + ">"; });
}

void InstallObjectViews(py::module_& data, const ScriptExecutor* exec)
{
	data.attr("objects") = py::cast(ObjectCollection{exec});
}

}  // namespace script
}  // namespace neurus
