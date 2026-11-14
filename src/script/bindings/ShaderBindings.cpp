/**
 * @file ShaderBindings.cpp
 * @brief `neurus.shader`, `neurus.data.shaders` and `mesh.shader`.
 *
 * The binding unit is a ShaderUnit view into a pooled ShaderData. Reads resolve
 * the live shader through the run's resource pool; every write goes through the
 * run's ScriptDispatch as a typed editor event, so undo and the panel refresh
 * keep working.
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl_bind.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "asset/data/shader/ShaderData.h"
#include "asset/data/shader/ShaderStage.h"
#include "asset/data/shader/ShaderStruct.h"
#include "asset/data/shader/ShaderUnit.h"
#include "core/IResourceLookup.h"
#include "editor/events/ShaderEvents.h"
#include "script/ScriptContext.h"
#include "script/ScriptView.h"
#include "script/bindings/Bindings.h"
#include "script/bindings/ShaderBindings.h"
#include "script/proxies/ProxyBase.h"
#include "script/proxies/ShaderProxies.h"

namespace py = pybind11;

// Opaque vectors so ShaderStruct properties return live references into the
// Python-owned ShaderStruct copy rather than converted list snapshots.
PYBIND11_MAKE_OPAQUE(std::vector<neurus::S_IO>);
PYBIND11_MAKE_OPAQUE(std::vector<neurus::S_Uniform>);
PYBIND11_MAKE_OPAQUE(std::vector<neurus::S_Func>);
PYBIND11_MAKE_OPAQUE(std::vector<neurus::S_StructDef>);
PYBIND11_MAKE_OPAQUE(std::vector<neurus::S_PushConstant>);
PYBIND11_MAKE_OPAQUE(std::vector<neurus::S_Glob>);
PYBIND11_MAKE_OPAQUE(std::vector<neurus::S_Var>);
PYBIND11_MAKE_OPAQUE(std::vector<neurus::S_SpecConstant>);

namespace neurus
{
namespace script
{
namespace
{

/// @brief Converts a ParaType to its GLSL type string.
std::string TypeString(ParaType type)
{
	return ShaderStruct::ParseType(type);
}

void SetIOType(S_IO& io, const std::string& value)
{
	io.type = ShaderStruct::ParseType(value);
	io.typeName = value;
}

std::string IOType(const S_IO& io)
{
	return io.typeName.empty() ? TypeString(io.type) : io.typeName;
}

void SetUniformType(S_Uniform& uniform, const std::string& value)
{
	uniform.type = ShaderStruct::ParseType(value);
	uniform.actualType = value;
}

std::string UniformType(const S_Uniform& uniform)
{
	return uniform.actualType.empty() ? TypeString(uniform.type) : uniform.actualType;
}

py::list ArgsToList(const Args& args)
{
	py::list result;
	for (const auto& [type, name, typeName] : args)
	{
		result.append(py::make_tuple(TypeString(type), name, typeName));
	}
	return result;
}

Args ListToArgs(const py::iterable& iterable)
{
	Args args;
	for (py::handle item : iterable)
	{
		py::sequence sequence = py::reinterpret_borrow<py::sequence>(item);
		const py::ssize_t size = py::len(sequence);
		if (size != 2 && size != 3)
		{
			throw py::value_error("function args entries must be (type, name[, type_name])");
		}
		const std::string type = py::cast<std::string>(sequence[0]);
		const std::string name = py::cast<std::string>(sequence[1]);
		const std::string typeName = size == 3 ? py::cast<std::string>(sequence[2]) : std::string{};
		args.emplace_back(ShaderStruct::ParseType(type), name, typeName);
	}
	return args;
}

/// @brief A ShaderUnit view or None when the pooled shader lacks the stage.
py::object UnitOrNone(const ShaderProxy& shader, ShaderType type)
{
	if (!shader.Resolve()->HasStage(type))
	{
		return py::none();
	}
	return py::cast(ShaderUnitProxy{shader.uid, shader.view, static_cast<int>(type)});
}

/// @brief Compiles a stage and raises the shaderc message on failure.
void CompileUnit(const ShaderUnitProxy& unit, bool fromCode)
{
	ShaderCompileResult result;
	EmitEvent(unit, ShaderUnitCompileRequested{
		unit.uid, unit.stage, fromCode ? 0 : 1, &result });
	if (!result.ok)
	{
		throw std::runtime_error(result.error.empty() ? "shader compile failed" : result.error);
	}
}

/**
 * @brief Parses mesh.shader.create(...) arguments and emits one create request.
 *
 * Forms: no args (defaults), one path (stage inferred), two positional
 * (vertex, fragment), and vertex=/fragment= keywords. Each side accepts a path
 * or a pooled Shader proxy whose matching stage is copied.
 */
ShaderSlotProxy CreateShaderFromPython(const ShaderSlotProxy& slot, const py::args& args,
                                       const py::kwargs& kwargs)
{
	if (args.size() > 2)
	{
		throw py::value_error("create accepts at most two positional arguments");
	}

	std::string vertexPath;
	std::string fragmentPath;
	int vertexShaderUid = 0;
	int fragmentShaderUid = 0;
	bool vertexSet = false;
	bool fragmentSet = false;

	auto applySide = [&](bool isVertex, const py::handle& value)
	{
		const ShaderType type = isVertex ? ShaderType::VERTEX : ShaderType::FRAGMENT;
		const char* sideName = isVertex ? "vertex" : "fragment";
		bool& alreadySet = isVertex ? vertexSet : fragmentSet;
		if (alreadySet)
		{
			throw py::value_error(std::string(sideName) + " given more than once");
		}
		alreadySet = true;

		if (py::isinstance<py::str>(value))
		{
			const std::string path = py::cast<std::string>(value);
			const auto inferred = ShaderTypeFromPath(path);
			if (!inferred || *inferred != type)
			{
				throw py::value_error(std::string("path is not a ") + sideName + " shader: " + path);
			}
			(isVertex ? vertexPath : fragmentPath) = path;
			return;
		}

		if (py::isinstance<ShaderProxy>(value))
		{
			const ShaderProxy source = py::cast<ShaderProxy>(value);
			if (!source.Resolve()->HasStage(type))
			{
				throw py::value_error(std::string("shader has no ") + sideName + " stage");
			}
			(isVertex ? vertexShaderUid : fragmentShaderUid) = source.uid;
			return;
		}

		throw py::type_error(std::string("expected a path or a Shader for ") + sideName);
	};

	if (args.size() == 1)
	{
		if (py::isinstance<ShaderProxy>(args[0]))
		{
			throw py::value_error("pass vertex= or fragment= for a pooled shader");
		}
		if (!py::isinstance<py::str>(args[0]))
		{
			throw py::type_error("expected a shader path or a pooled Shader");
		}
		const std::string path = py::cast<std::string>(args[0]);
		const auto inferred = ShaderTypeFromPath(path);
		if (!inferred || (*inferred != ShaderType::VERTEX && *inferred != ShaderType::FRAGMENT))
		{
			throw py::value_error("unsupported shader extension: " + path);
		}
		applySide(*inferred == ShaderType::VERTEX, args[0]);
	}
	else if (args.size() == 2)
	{
		applySide(true, args[0]);
		applySide(false, args[1]);
	}

	for (auto item : kwargs)
	{
		const std::string key = py::cast<std::string>(item.first);
		if (key == "vertex") applySide(true, item.second);
		else if (key == "fragment") applySide(false, item.second);
		else throw py::type_error("unexpected keyword argument: " + key);
	}

	ShaderRequestResult result;
	EmitEvent(slot, ShaderCreateRequested{
		slot.uid, vertexPath, fragmentPath, vertexShaderUid, fragmentShaderUid, &result });
	if (result.shaderUid == 0)
	{
		throw std::runtime_error(result.error.empty() ? "shader creation failed" : result.error);
	}
	return slot;
}

/**
 * @brief Pool-wide shader collection over the ShaderData base.
 *
 * Iteration and lookup go through ForEachUID because the per-type index keys
 * the exact dynamic type (RenderShader / ComputeShader), not the base. Names
 * are the ShaderData's registered name.
 */
struct ShaderCollection
{
	const ScriptView* view = nullptr;

	const IResourceLookup* Pool() const { return ShaderPool(view); }

	py::list Snapshot() const
	{
		py::list result;
		const IResourceLookup* pool = Pool();
		if (pool == nullptr) return result;

		pool->ForEachUID([&](const std::shared_ptr<UID>& resource)
		{
			if (auto shader = std::dynamic_pointer_cast<ShaderData>(resource))
			{
				result.append(py::cast(ShaderProxy{shader->GetObjectID(), view}));
			}
		});
		return result;
	}

	py::ssize_t Count() const
	{
		py::ssize_t count = 0;
		const IResourceLookup* pool = Pool();
		if (pool != nullptr)
		{
			pool->ForEachUID([&](const std::shared_ptr<UID>& resource)
			{
				if (std::dynamic_pointer_cast<ShaderData>(resource)) ++count;
			});
		}
		return count;
	}

	py::object Lookup(int uid) const
	{
		const IResourceLookup* pool = Pool();
		if (pool == nullptr || pool->Get<ShaderData>(uid) == nullptr)
		{
			return py::none();
		}
		return py::cast(ShaderProxy{uid, view});
	}

	bool Contains(int uid) const
	{
		const IResourceLookup* pool = Pool();
		return pool != nullptr && pool->Get<ShaderData>(uid) != nullptr;
	}

	py::object LookupName(const std::string& name) const
	{
		const IResourceLookup* pool = Pool();
		if (pool == nullptr) return py::none();

		py::object result = py::none();
		pool->ForEachUID([&](const std::shared_ptr<UID>& resource)
		{
			if (!result.is_none()) return;
			if (auto shader = std::dynamic_pointer_cast<ShaderData>(resource))
			{
				if (shader->GetName() == name)
				{
					result = py::cast(ShaderProxy{shader->GetObjectID(), view});
				}
			}
		});
		return result;
	}

	bool ContainsName(const std::string& name) const
	{
		return !LookupName(name).is_none();
	}
};

}  // namespace

/// @brief Registers the `neurus.shader` module and its proxy classes.
void RegisterShaderBindings(py::module_& shader)
{
	shader.attr("VERTEX") = static_cast<int>(ShaderType::VERTEX);
	shader.attr("FRAGMENT") = static_cast<int>(ShaderType::FRAGMENT);
	shader.attr("COMPUTE") = static_cast<int>(ShaderType::COMPUTE);
	shader.attr("GEOMETRY") = static_cast<int>(ShaderType::GEOMETRY);
	shader.attr("DEFAULT_VERTEX") = kDefaultVertexPath;
	shader.attr("DEFAULT_FRAGMENT") = kDefaultFragmentPath;

	py::enum_<Interp>(shader, "Interpolation", "GLSL interpolation qualifier.")
		.value("smooth", Interp::Smooth)
		.value("flat", Interp::Flat)
		.value("noperspective", Interp::Noperspective);

	// --- IR element classes ------------------------------------------------

	py::class_<S_IO>(shader, "Attribute", "Vertex attribute, pass output or struct member.")
		.def(py::init([](int location, const std::string& name, const std::string& type)
		{
			S_IO io;
			io.location = location;
			io.name = name;
			SetIOType(io, type);
			return io;
		}), py::arg("location") = 0, py::arg("name") = std::string{},
		    py::arg("type") = "float")
		.def_readwrite("location", &S_IO::location)
		.def_readwrite("name", &S_IO::name)
		.def_property("type", &IOType, &SetIOType)
		.def_readwrite("type_name", &S_IO::typeName)
		.def_readwrite("interpolation", &S_IO::interpolation);

	py::class_<S_Uniform>(shader, "Uniform", "Uniform, input or output variable.")
		.def(py::init([](const std::string& name, const std::string& type, int count, int binding)
		{
			S_Uniform uniform;
			uniform.name = name;
			SetUniformType(uniform, type);
			uniform.count = count;
			uniform.binding = binding;
			return uniform;
		}), py::arg("name") = std::string{}, py::arg("type") = "float",
		    py::arg("count") = 1, py::arg("binding") = -1)
		.def_readwrite("name", &S_Uniform::name)
		.def_property("type", &UniformType, &SetUniformType)
		.def_readwrite("count", &S_Uniform::count)
		.def_readwrite("binding", &S_Uniform::binding)
		.def_readwrite("qualifiers", &S_Uniform::qualifiers)
		.def_property("actual_type",
			[](const S_Uniform& u) { return u.actualType; },
			[](S_Uniform& u, const std::string& value)
			{
				u.actualType = value;
				u.type = ShaderStruct::ParseType(value);
			})
		.def_readwrite("image_format", &S_Uniform::imageFormat);

	py::class_<S_Func>(shader, "Function", "User-defined function or const.")
		.def(py::init([](const std::string& name, const std::string& returnType, const std::string& body)
		{
			S_Func function;
			function.name = name;
			function.returnType = ShaderStruct::ParseType(returnType);
			function.body = body;
			return function;
		}), py::arg("name") = std::string{}, py::arg("return_type") = "void",
		    py::arg("body") = std::string{})
		.def_readwrite("name", &S_Func::name)
		.def_property("return_type",
			[](const S_Func& f) { return TypeString(f.returnType); },
			[](S_Func& f, const std::string& value) { f.returnType = ShaderStruct::ParseType(value); })
		.def_readwrite("body", &S_Func::body)
		.def_property("args",
			[](const S_Func& f) { return ArgsToList(f.args); },
			[](S_Func& f, const py::iterable& args) { f.args = ListToArgs(args); });

	py::class_<S_StructDef>(shader, "StructDef", "Bare struct or buffer block definition.")
		.def(py::init([](const std::string& name, const std::string& varName, int binding)
		{
			S_StructDef def;
			def.name = name;
			def.varName = varName;
			def.binding = binding;
			return def;
		}), py::arg("name") = std::string{}, py::arg("var_name") = std::string{},
		    py::arg("binding") = 0)
		.def_readwrite("binding", &S_StructDef::binding)
		.def_readwrite("name", &S_StructDef::name)
		.def_readwrite("var_name", &S_StructDef::varName)
		.def_readwrite("fields", &S_StructDef::fields);

	py::class_<S_PushConstant>(shader, "PushConstant", "Push-constant block member.")
		.def(py::init([](const std::string& name, const std::string& typeName, uint32_t offset, uint32_t size)
		{
			S_PushConstant pc;
			pc.name = name;
			pc.typeName = typeName;
			pc.offset = offset;
			pc.size = size;
			return pc;
		}), py::arg("name") = std::string{}, py::arg("type_name") = "float",
		    py::arg("offset") = 0, py::arg("size") = 4)
		.def_readwrite("name", &S_PushConstant::name)
		.def_readwrite("type_name", &S_PushConstant::typeName)
		.def_readwrite("offset", &S_PushConstant::offset)
		.def_readwrite("size", &S_PushConstant::size);

	py::class_<S_Glob>(shader, "Global", "Global variable with a scalar default.")
		.def(py::init([](const std::string& name, const std::string& type, float defaultValue)
		{
			S_Glob global;
			global.name = name;
			global.type = ShaderStruct::ParseType(type);
			global.defaultVal = defaultValue;
			return global;
		}), py::arg("name") = std::string{}, py::arg("type") = "float",
		    py::arg("default_value") = 0.0f)
		.def_readwrite("name", &S_Glob::name)
		.def_property("type",
			[](const S_Glob& g) { return TypeString(g.type); },
			[](S_Glob& g, const std::string& value) { g.type = ShaderStruct::ParseType(value); })
		.def_readwrite("default_value", &S_Glob::defaultVal);

	py::class_<S_Var>(shader, "Variable", "Variable with a string type name.")
		.def(py::init([](const std::string& typeName, const std::string& name, int count)
		{
			S_Var var;
			var.typeName = typeName;
			var.name = name;
			var.count = count;
			return var;
		}), py::arg("type_name") = "float", py::arg("name") = std::string{},
		    py::arg("count") = 1)
		.def_readwrite("type_name", &S_Var::typeName)
		.def_readwrite("name", &S_Var::name)
		.def_readwrite("count", &S_Var::count);

	py::class_<S_SpecConstant>(shader, "SpecConstant", "Specialisation constant.")
		.def(py::init([](uint32_t binding, const std::string& name, const std::string& type, float defaultValue)
		{
			S_SpecConstant spec;
			spec.binding = binding;
			spec.name = name;
			spec.type = ShaderStruct::ParseType(type);
			spec.defaultVal = defaultValue;
			return spec;
		}), py::arg("binding") = 0, py::arg("name") = std::string{},
		    py::arg("type") = "float", py::arg("default_value") = 0.0f)
		.def_readwrite("binding", &S_SpecConstant::binding)
		.def_readwrite("name", &S_SpecConstant::name)
		.def_property("type",
			[](const S_SpecConstant& s) { return TypeString(s.type); },
			[](S_SpecConstant& s, const std::string& value) { s.type = ShaderStruct::ParseType(value); })
		.def_readwrite("default_value", &S_SpecConstant::defaultVal);

	// --- IR containers -----------------------------------------------------

	py::bind_vector<std::vector<S_IO>>(shader, "AttributeList");
	py::bind_vector<std::vector<S_Uniform>>(shader, "UniformList");
	py::bind_vector<std::vector<S_Func>>(shader, "FunctionList");
	py::bind_vector<std::vector<S_StructDef>>(shader, "StructDefList");
	py::bind_vector<std::vector<S_PushConstant>>(shader, "PushConstantList");
	py::bind_vector<std::vector<S_Glob>>(shader, "GlobalList");
	py::bind_vector<std::vector<S_Var>>(shader, "VariableList");
	py::bind_vector<std::vector<S_SpecConstant>>(shader, "SpecConstantList");

	py::class_<ShaderStruct>(shader, "ShaderStruct", "Parsed GLSL IR for one stage.")
		.def(py::init<>())
		.def_readwrite("attributes", &ShaderStruct::AB_list)
		.def_readwrite("pass_outputs", &ShaderStruct::pass_list)
		.def_readwrite("storage_buffers", &ShaderStruct::SB_list)
		.def_readwrite("uniform_buffers", &ShaderStruct::ubuffer_list)
		.def_readwrite("struct_defs", &ShaderStruct::struct_def_list)
		.def_readwrite("uniforms", &ShaderStruct::uniform_list)
		.def_readwrite("inputs", &ShaderStruct::input_list)
		.def_readwrite("outputs", &ShaderStruct::output_list)
		.def_readwrite("constants", &ShaderStruct::const_list)
		.def_readwrite("globals", &ShaderStruct::glob_list)
		.def_readwrite("variables", &ShaderStruct::vari_list)
		.def_readwrite("functions", &ShaderStruct::func_list)
		.def_readwrite("main", &ShaderStruct::Main)
		.def_readwrite("push_constants", &ShaderStruct::push_constants)
		.def_readwrite("push_constant_variable", &ShaderStruct::push_constants_var)
		.def_readwrite("spec_constants", &ShaderStruct::spec_constants)
		.def_property("local_size",
			[](const ShaderStruct& s)
			{
				return py::make_tuple(s.local_size_x, s.local_size_y, s.local_size_z);
			},
			[](ShaderStruct& s, const std::tuple<uint32_t, uint32_t, uint32_t>& size)
			{
				s.local_size_x = std::get<0>(size);
				s.local_size_y = std::get<1>(size);
				s.local_size_z = std::get<2>(size);
			})
		.def_property("extensions",
			[](const ShaderStruct& s) { return s.extensions; },
			[](ShaderStruct& s, const std::vector<std::string>& value) { s.extensions = value; })
		.def_property("defines",
			[](const ShaderStruct& s) { return s.define_directives; },
			[](ShaderStruct& s, const std::vector<std::string>& value) { s.define_directives = value; })
		.def_readwrite("version", &ShaderStruct::version);

	// --- Proxies -----------------------------------------------------------

	py::class_<ShaderUnitProxy>(shader, "ShaderUnit", "A view into one shader stage.")
		.def_property_readonly("stage", [](const ShaderUnitProxy& s) { return s.stage; })
		.def_property("code",
			[](const ShaderUnitProxy& s) { return s.ResolveUnit()->code; },
			[](const ShaderUnitProxy& s, const std::string& code)
			{
				EmitEvent(s, ShaderUnitCodeSet{s.uid, s.stage, code});
			})
		.def_property("parsed",
			[](const ShaderUnitProxy& s) { return s.ResolveUnit()->parsed; },
			[](const ShaderUnitProxy& s, const ShaderStruct& parsed)
			{
				EmitEvent(s, ShaderUnitStructSet{s.uid, s.stage, parsed});
			})
		.def("compile", &CompileUnit, py::arg("from_code") = true,
		     "Compile the stage; raises RuntimeError with the shaderc message on failure.");

	py::class_<ShaderProxy>(shader, "Shader", "A pooled RenderShader or ComputeShader bundle.")
		.def_property_readonly("name", [](const ShaderProxy& s) { return s.Resolve()->GetName(); })
		.def_property_readonly("stage",
			[](const ShaderProxy& s) { return static_cast<int>(s.Resolve()->GetType()); })
		.def_property_readonly("vertex",
			[](const ShaderProxy& s) { return UnitOrNone(s, ShaderType::VERTEX); })
		.def_property_readonly("fragment",
			[](const ShaderProxy& s) { return UnitOrNone(s, ShaderType::FRAGMENT); })
		.def_property_readonly("compute",
			[](const ShaderProxy& s) { return UnitOrNone(s, ShaderType::COMPUTE); });

	py::class_<ShaderSlotProxy>(shader, "ShaderSlot", "The shader slot of a mesh.")
		.def_property_readonly("attached", &ShaderSlotProxy::Attached)
		.def("create", &CreateShaderFromPython,
		     "Create or replace this mesh's shader from paths or pooled stage sources.")
		.def("remove",
			[](const ShaderSlotProxy& s) { EmitEvent(s, ShaderRemoveRequested{s.uid}); })
		.def_property_readonly("vertex",
			[](const ShaderSlotProxy& s) -> py::object
			{
				const int shaderUid = s.ShaderUid();
				if (shaderUid == 0) return py::none();
				const ShaderData* shader = ResolveShader(s.view, shaderUid, "shader");
				if (!shader->HasStage(ShaderType::VERTEX)) return py::none();
				return py::cast(ShaderUnitProxy{shaderUid, s.view, static_cast<int>(ShaderType::VERTEX)});
			})
		.def_property_readonly("fragment",
			[](const ShaderSlotProxy& s) -> py::object
			{
				const int shaderUid = s.ShaderUid();
				if (shaderUid == 0) return py::none();
				const ShaderData* shader = ResolveShader(s.view, shaderUid, "shader");
				if (!shader->HasStage(ShaderType::FRAGMENT)) return py::none();
				return py::cast(ShaderUnitProxy{shaderUid, s.view, static_cast<int>(ShaderType::FRAGMENT)});
			});

	// --- Asset entry points ------------------------------------------------

	// load_shader needs the engine's per-run view, so it is installed by
	// InstallShaderViews() together with the data.shaders collection.
}

/// @brief Adds the `shader` slot property to the Mesh proxy class.
void RegisterMeshShaderBindings(py::class_<MeshProxy, ObjectProxy, Transform3DProxy>& cls)
{
	cls.def_property_readonly(
		"shader",
		[](const MeshProxy& self) { return ShaderSlotProxy{self.uid, self.view}; },
		"The mesh's shader slot: create(), remove(), attached, vertex, fragment.");
}

/// @brief Installs the per-run views: load_shader and the shader collection.
void InstallShaderViews(py::module_& shader, py::module_& data, const ScriptView* view)
{
	shader.def(
		"load_shader",
		[view](const std::string& path, const std::string& name) -> ShaderProxy
		{
			const auto stage = ShaderTypeFromPath(path);
			if (!stage || *stage == ShaderType::GEOMETRY)
			{
				throw py::value_error("unsupported shader extension: " + path);
			}

			ShaderRequestResult result;
			EmitEvent(ProxyBase{0, view}, ShaderLoadRequested{path, name, &result});
			if (result.shaderUid == 0)
			{
				throw std::runtime_error(result.error.empty() ? "shader load failed" : result.error);
			}
			return ShaderProxy{result.shaderUid, view};
		},
		py::arg("path"), py::arg("name") = std::string{},
		"Load a .vert/.frag/.comp source into the pool; empty name uses the path.");

	py::class_<ShaderCollection>(data, "_Shaders", "Pool-wide shader collection.")
		.def("__iter__", [](const ShaderCollection& s) { return py::iter(s.Snapshot()); })
		.def("__len__", [](const ShaderCollection& s) { return s.Count(); })
		.def("__getitem__", [](const ShaderCollection& s, int uid) { return s.Lookup(uid); })
		.def("__contains__", [](const ShaderCollection& s, int uid) { return s.Contains(uid); })
		.def("__getitem__",
		     [](const ShaderCollection& s, const std::string& name) { return s.LookupName(name); })
		.def("__contains__",
		     [](const ShaderCollection& s, const std::string& name) { return s.ContainsName(name); });

	data.attr("shaders") = py::cast(ShaderCollection{view});
}

}  // namespace script
}  // namespace neurus
