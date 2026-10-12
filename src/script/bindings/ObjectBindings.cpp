/**
 * @file ObjectBindings.cpp
 * @brief Read-only `Object` / `Transform` proxies and `neurus.data.objects`.
 *
 * Phase 1 exposes the master scene pool for reading only: every property is
 * resolved live through the run's `ScriptContext` on attribute access. No
 * setter is defined here (write support, gated through `ScriptDispatch`, is a
 * later task), and no value is cached, so a proxy can never observe a stale
 * scene (design sections 4.3 and 5).
 */

#include <pybind11/pybind11.h>

#include <stdexcept>
#include <string>

#include "scene/ObjectID.h"
#include "scene/Scene.h"
#include "scene/Transform.h"
#include "script/bindings/Bindings.h"
#include "script/bindings/ProxyBase.h"
#include "script/bindings/Proxies.h"
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

/**
 * @brief `neurus.data.objects`: all master-pool objects, iterable and keyed by
 *        UID.
 *
 * The collection itself is stateless; it reads `CurrentContext()` on every
 * access, so it is empty (never a null dereference) outside a run or when the
 * run has no scene.
 */
struct ObjectCollection
{
	/// @brief Snapshot of every master-pool object as an `Object` proxy list.
	py::list Snapshot() const
	{
		py::list result;
		const ScriptContext* ctx = CurrentContext();
		if (ctx == nullptr || ctx->scene == nullptr)
		{
			return result;
		}

		for (const auto& [id, obj] : ctx->scene->obj_list)
		{
			(void)obj;
			result.append(py::cast(ObjectProxy{ctx, id}));
		}
		return result;
	}
};

/// @brief Registers the proxies and the `objects` collection on `data`.
void RegisterObjectBindings(py::module_& data)
{
	py::class_<TransformProxy>(data, "Transform", "Read-only 3D transform of a scene object.")
		.def_property_readonly(
			"position",
			[](const TransformProxy& self) { return ToTuple(self.Resolve()->GetPosition()); },
			"World position as an (x, y, z) tuple.")
		.def_property_readonly(
			"rotation",
			[](const TransformProxy& self) { return ToTuple(self.Resolve()->GetRotation()); },
			"Euler rotation in degrees as an (x, y, z) tuple.")
		.def_property_readonly(
			"scale",
			[](const TransformProxy& self) { return ToTuple(self.Resolve()->GetScale()); },
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
		.def_property_readonly(
			"visible",
			[](const ObjectProxy& self) { return self.Resolve()->is_viewport; },
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
				return py::cast(TransformProxy{self.ctx, self.uid});
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
		     [](const ObjectCollection&, int uid) -> py::object
		     {
			     const ScriptContext* ctx = CurrentContext();
			     if (ctx == nullptr || ctx->scene == nullptr || ctx->scene->GetObjectID(uid) == nullptr)
			     {
				     return py::none();
			     }
			     return py::cast(ObjectProxy{ctx, uid});
		     },
		     "Looks an object up by UID, or returns None when it is absent.")
		.def("__contains__",
		     [](const ObjectCollection&, int uid)
		     {
			     const ScriptContext* ctx = CurrentContext();
			     return ctx != nullptr && ctx->scene != nullptr
			            && ctx->scene->GetObjectID(uid) != nullptr;
		     },
		     "True when an object with the given UID exists.")
		.def("__repr__",
		     [](const ObjectCollection& self) { return "<neurus.data.objects len=" + std::to_string(py::len(self.Snapshot())) + ">"; });

	// The collection is a stateless view; one instance serves the whole module.
	data.attr("objects") = py::cast(ObjectCollection{});
}

}  // namespace

void RegisterNeurusModule(py::module_& m)
{
	m.doc() = "Neurus scripting API (read-only phase 1).";

	py::module_ data = m.def_submodule("data", "Read-only scene data collections.");
	RegisterObjectBindings(data);
	RegisterSceneBindings(m, data);
	RegisterConfigBindings(data);
}

}  // namespace script
}  // namespace neurus
