/**
 * @file ShaderStruct.h
 * @brief Shader Intermediate Representation (IR) data model for dynamic GLSL
 *        shader generation, extended with Vulkan-specific constructs.
 *
 * ShaderStruct is the central IR that decouples shader parsing (Task 7) from
 * GLSL code generation (Task 8). A parser populates the ShaderStruct
 * containers; GenerateShader() reads them to emit a valid Vulkan GLSL string.
 *
 * Architecture:
 * - Independent of Shader base class (Task 6) - no inheritance relationship.
 * - Uses ParaType from core/Parameters.h (Task 2) for type metadata.
 * - Named structs replace OpenGL's std::tuple patterns for clarity.
 * - Vulkan extensions: push constants, specialization constants, local size,
 *   GLSL extensions, version control.
 *
 * @note This is a pure data model. No OpenGL headers, no GPU handles.
 */

#pragma once

#include "core/Parameters.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neurus {

// --------------------------------------
// Interpolation qualifier for inter-stage variables
// --------------------------------------

/** @brief GLSL interpolation qualifier for layout(location=N) declarations. */
enum class Interp : uint8_t
{
	Smooth        = 0,  ///< Default perspective-correct interpolation
	Flat          = 1,  ///< No interpolation (required for integer types)
	Noperspective = 2,  ///< Linear interpolation in screen space
};

// --------------------------------------
// Named type aliases - replace OpenGL std::tuple patterns
// --------------------------------------

/** @brief Input/Output binding descriptor for layout(location=X) declarations. */
struct S_IO
{
	int location;             ///< GLSL layout location qualifier
	std::string name;         ///< Variable name
	ParaType type;            ///< GLSL type of the variable
	std::string typeName;     ///< Original type name string for custom types (empty -> use ParseType(type))
	Interp interpolation = Interp::Smooth;  ///< Interpolation qualifier

	/** @brief Member-wise equality (used by undo/redo no-net-change diffing). */
	bool operator==(const S_IO&) const = default;
};

/** @brief Ordered parameter list: (type, name, originalTypeName) triples. */
using Args = std::vector<std::tuple<ParaType, std::string, std::string>>;

/** @brief Struct or buffer-block definition (bare struct, storage buffer or uniform block). */
struct S_StructDef
{
	int binding;              ///< Descriptor-set binding point (0 for bare structs)
	std::string name;         ///< Struct / block type name
	std::vector<S_IO> fields; ///< Member fields (location unused for struct members)
	std::string varName;      ///< Variable / instance name (used for ubuffer_list; empty for others)

	/** @brief Member-wise equality (used by undo/redo no-net-change diffing). */
	bool operator==(const S_StructDef&) const = default;
};

/** @brief Variable declaration (uniform, shader-stage input or output). */
struct S_Uniform
{
	std::string name;       ///< Variable name
	ParaType type;          ///< GLSL type (may be NONE when qualifiers/actualType are used)
	int count;              ///< Array count (1 = scalar)
	int binding = -1;       ///< Descriptor-set binding point (-1 = no layout qualifier)
	std::string qualifiers; ///< Qualifiers (e.g. "writeonly", "readonly"; empty = none)
	std::string actualType; ///< Original GLSL type string (e.g. "image2D"; empty -> use ParseType(type))
	std::string imageFormat; ///< Image format qualifier (e.g. "r8", "rgba16f"; empty = none)

	/** @brief Member-wise equality (used by undo/redo no-net-change diffing). */
	bool operator==(const S_Uniform&) const = default;
};

/** @brief Function or const definition. */
struct S_Func
{
	ParaType returnType;                                 ///< Return type (or const type)
	std::string name;                                    ///< Function / const name
	std::string body;                                    ///< Function body or const value
	Args args;  ///< Parameter list

	/** @brief Member-wise equality (used by undo/redo no-net-change diffing). */
	bool operator==(const S_Func&) const = default;
};

/** @brief Alias: const values share the same structure as functions. */
using S_Const = S_Func;

/** @brief Global variable with a default scalar value. */
struct S_Glob
{
	std::string name;  ///< Variable name
	ParaType type;     ///< GLSL type
	float defaultVal;  ///< Default scalar value
};

/** @brief Variable declaration with a string type name (supports user-defined types). */
struct S_Var
{
	std::string typeName; ///< GLSL type name (supports custom types)
	std::string name;     ///< Variable name
	int count;            ///< Array count (1 = scalar)
};

/**
 * @brief Push-constant range descriptor.
 *
 * Mirrors one member of a `layout(push_constant) uniform` GLSL block.
 */
struct S_PushConstant
{
	std::string name;     ///< Member name within the push-constant block
	uint32_t offset;      ///< Byte offset from the start of the push-constant block
	uint32_t size;        ///< Byte size of this member
	std::string typeName; ///< GLSL type of this member (e.g. "mat4", "vec4", "float")

	/** @brief Member-wise equality (used by undo/redo no-net-change diffing). */
	bool operator==(const S_PushConstant&) const = default;
};

/**
 * @brief Specialisation-constant descriptor.
 *
 * Maps to GLSL `layout(constant_id = binding) const type name = defaultVal;`.
 */
struct S_SpecConstant
{
	uint32_t binding;    ///< constant_id value
	std::string name;    ///< Variable name
	ParaType type;       ///< GLSL type
	float defaultVal;    ///< Default value (float for simplicity)
};

// --------------------------------------
// Convenience alias - matches OpenGL ShaderLib.h
// --------------------------------------

// --------------------------------------
// ShaderStruct - the IR data model
// --------------------------------------

/**
 * @brief Intermediate Representation for a single GLSL shader stage.
 *
 * Containers are populated by the parser and read by the generator; every
 * setter marks is_struct_changed = true, and the generator resets it after
 * successful code generation.
 */
class ShaderStruct
{
public:
	// clang-format off

	// ----------------------------------
	// Ported from OpenGL ShaderLib.h
	// ----------------------------------

	std::vector<S_IO>        AB_list;           ///< Vertex-attribute locations  (layout(location=X) in)
	std::vector<S_IO>        pass_list;         ///< Render-pass output locations (layout(location=X) out)
	std::vector<S_StructDef> SB_list;           ///< Storage-buffer blocks       (layout(binding=X) buffer)
	std::vector<S_StructDef> ubuffer_list;      ///< Uniform-buffer blocks       (layout(binding=X) uniform)
	std::vector<S_StructDef> struct_def_list;   ///< Bare struct definitions
	std::vector<S_Uniform>   uniform_list;      ///< Single-value uniforms       (uniform type name;)
	std::vector<S_Uniform>   input_list;        ///< Shader-stage inputs         (in type name;)
	std::vector<S_Uniform>   output_list;       ///< Shader-stage outputs        (out type name;)
	std::vector<S_Const>     const_list;        ///< Const declarations          (const type name = val;)
	std::vector<S_Glob>      glob_list;         ///< Global variables with defaults
	std::vector<S_Var>       vari_list;         ///< Local / global variables    (type name[count];)
	std::vector<S_Func>      func_list;         ///< User-defined functions

	std::string              Main;              ///< Body of the main() entry point

	// ----------------------------------
	// Vulkan-specific extensions
	// ----------------------------------

	std::vector<S_PushConstant> push_constants; ///< Push-constant block members
	std::string                push_constants_var; ///< Variable name after closing '}' (empty = none)
	std::vector<S_SpecConstant> spec_constants; ///< Specialisation constants
	uint32_t                    local_size_x = 0; ///< Compute workgroup X dimension (0 = not a compute shader)
	uint32_t                    local_size_y = 0; ///< Compute workgroup Y dimension
	uint32_t                    local_size_z = 0; ///< Compute workgroup Z dimension
	std::vector<std::string>    extensions;     ///< Required GLSL extensions (e.g. "GL_GOOGLE_include_directive")
	std::vector<std::string>    define_directives; ///< Raw #define directives (e.g. "#define NUM_SAMPLES 64")
	int                         version = 0;    ///< GLSL #version (0 = unset; the generator defaults to 450)

	// ----------------------------------
	// State flag
	// ----------------------------------

	/**
	 * @brief Set to true by every setter; cleared by the generator on success.
	 *
	 * Used to detect whether regeneration is needed after the last compile.
	 */
	bool is_struct_changed = true;

	// clang-format on

	// ----------------------------------
	// Static type-system helpers (ported from OpenGL, backed by Parameters.h)
	// ----------------------------------

	/** @brief Dynamic type table - maps ParaType values and custom types to GLSL strings. */
	static std::vector<std::string> type_table;

	/** @brief Converts a ParaType to its GLSL type string (delegates to neurus::ToString()). */
	static std::string ParseType(ParaType type);
	/**
	 * @brief Parses a GLSL type string into its ParaType value.
	 *
	 * Unknown types are registered in type_table and reported as CUSTOM.
	 */
	static ParaType ParseType(const std::string& type);
	/** @brief Returns an array suffix string ("[N]") or empty if count <= 1. */
	static std::string ParseCount(int count);
	/** @brief Formats a parameter list as a GLSL function-argument string: "(type0 n0, type1 n1, ...)". */
	static std::string ParseArgs(const Args& args);
	/** @brief Parses a GLSL function-argument string back into an Args list. */
	static Args ParseArgs(const std::string& args);
	/** @brief Returns true if the string is a recognised GLSL type (including type_table). */
	static bool IsAvailType(const std::string& type);
	/** @brief Registers a custom type name in the type table. */
	static void ADD_TYPE(const std::string& name);

	// ----------------------------------
	// Setters - each marks is_struct_changed = true (ported from OpenGL)
	// ----------------------------------

	/** @brief Register a vertex-attribute / input-location binding. */
	void SetAB(int loc, ParaType type, const std::string& name,
	           Interp interp = Interp::Smooth);
	/** @brief Register a render-pass output-location binding. */
	void SetPass(int loc, ParaType type, const std::string& name,
	             Interp interp = Interp::Smooth);
	/** @brief Register a storage-buffer block declaration. */
	void SetSB(int loc, const std::string& name, const Args& args);
	/** @brief Register a uniform-buffer block declaration. */
	void SetUB(std::string type, std::string name, const Args& args, int binding = 0);
	/** @brief Register a bare uniform variable. */
	void SetUni(ParaType type, int count, const std::string& name, int binding = -1,
	            const std::string& qualifiers = "", const std::string& actualType = "",
	            const std::string& imageFormat = "");
	/** @brief Register a shader-stage input variable. */
	void SetInp(ParaType type, int count, const std::string& name);
	/** @brief Register a shader-stage output variable. */
	void SetOut(ParaType type, int count, const std::string& name);
	/** @brief Register a global variable with a default scalar value. */
	void SetGlob(ParaType type, float defult, const std::string& name);
	/** @brief Define a bare struct type. */
	void DefStruct(const std::string& name, const Args& args);
	/** @brief Define a user function. */
	void DefFunc(ParaType type, std::string name, const std::string& content, const Args& args);
	/** @brief Register a const variable. */
	void SetConst(ParaType type, const std::string& name, const std::string& content);
	/** @brief Register a variable with a string type name (supports custom types). */
	void SetVar(const std::string& typeName, const std::string& name, int count);

	// ----------------------------------
	// Vulkan-specific setters
	// ----------------------------------

	/** @brief Register a push-constant block member (offset, size, and GLSL type). */
	void SetPushConstant(const std::string& name, uint32_t offset, uint32_t size,
	                     const std::string& typeName);
	/** @brief Set the push-constant block variable name (e.g. "pc" in "} pc;"). */
	void SetPushConstantVar(const std::string& var);
	/** @brief Set the compute-shader workgroup size. */
	void SetLocalSize(uint32_t x, uint32_t y, uint32_t z);
	/** @brief Set the GLSL #version value. */
	void SetVersion(int v);
	/** @brief Add a required GLSL extension string. */
	void AddExtension(const std::string& ext);
	/** @brief Add a raw #define directive (stored as-is, emitted before declarations). */
	void AddDefine(const std::string& directive);

	// ----------------------------------
	// Utility
	// ----------------------------------

	/**
	 * @brief Resets ALL containers and Vulkan fields to default/empty state.
	 *
	 * Version resets to 0 (unset; the generator defaults to 450).
	 */
	void Reset();

	/** @brief Returns true when every container is empty and Main is empty. */
	bool IsEmpty() const;

	/** @brief Reset the static type registration table. Call between independent parse sessions. */
	static void ResetTypeTable();
};

} // namespace neurus
