/**
 * @file ShaderInterface.cpp
 * @brief Reflects SPIR-V stage locations without GPU or renderer dependencies.
 */
#include "asset/data/shader/ShaderInterface.hpp"
#include "asset/data/ShaderData.hpp"

#include <glslang/SPIRV/spirv.hpp11>

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neurus {
namespace {

struct Decoration
{
	std::optional<uint32_t> location;
	uint32_t component = 0;
	bool builtin = false;
	std::optional<uint32_t> binding;
	uint32_t descriptorSet = 0;
	uint32_t arrayStride = 0;
	uint32_t matrixStride = 0;
	uint32_t offset = 0;
	bool rowMajor = false;
};

struct Type
{
	spv::Op op;
	std::vector<uint32_t> operands;
};

// Names and compiler-assigned type IDs can change between compilations. The
// byte layout behind a descriptor or a push-constant pointer must still match.
struct BufferLayout
{
	spv::Op type;
	std::vector<uint32_t> operands;
	std::vector<BufferLayout> members;
	uint32_t arrayStride = 0;
	uint32_t offset = 0;
	uint32_t matrixStride = 0;
	bool rowMajor = false;

	bool operator==(const BufferLayout&) const = default;
};

using Binding = std::tuple<spv::StorageClass, uint32_t, uint32_t>;

// A 64-bit scalar occupies two component slots. Keeping scalar type information
// per slot also permits legal packing of independent variables with Component.
using Slot = std::pair<uint32_t, uint32_t>;
using Interface = std::map<Slot, std::pair<spv::Op, std::vector<uint32_t>>>;

class StageInterface
{
public:
	StageInterface(std::span<const uint32_t> words, spv::ExecutionModel model)
	{
		if (words.size() < 5 || words[0] != spv::MagicNumber)
			throw std::runtime_error("invalid SPIR-V header");
		for (size_t offset = 5; offset < words.size();)
		{
			const auto count = words[offset] >> spv::WordCountShift;
			const auto op = static_cast<spv::Op>(words[offset] & spv::OpCodeMask);
			if (count == 0 || count > words.size() - offset)
				throw std::runtime_error("invalid SPIR-V instruction");
			const auto args = words.subspan(offset + 1, count - 1);
			switch (op)
			{
			case spv::Op::OpEntryPoint:
				if (args.size() >= 3 && args[0] == static_cast<uint32_t>(model))
				{
					m_entry = args[1];
					size_t end = 2;
					for (; end < args.size(); ++end)
					{
						const uint32_t word = args[end];
						if ((word & 0xffu) == 0 || (word & 0xff00u) == 0 ||
						    (word & 0xff0000u) == 0 || (word & 0xff000000u) == 0)
							break;
					}
					if (end == args.size()) throw std::runtime_error("unterminated SPIR-V entry name");
					m_interfaces.assign(args.begin() + end + 1, args.end());
				}
				break;
			case spv::Op::OpExecutionMode:
			case spv::Op::OpExecutionModeId:
				if (args.size() == 5 && args[0] == m_entry &&
				    (args[1] == static_cast<uint32_t>(spv::ExecutionMode::LocalSize) ||
				     args[1] == static_cast<uint32_t>(spv::ExecutionMode::LocalSizeId)))
				{
					m_localSize = {args[2], args[3], args[4]};
					m_sizeUsesIds = op == spv::Op::OpExecutionModeId;
				}
				break;
			case spv::Op::OpTypeRuntimeArray:
			case spv::Op::OpTypeImage:
			case spv::Op::OpTypeSampledImage:
			case spv::Op::OpTypeSampler:
			case spv::Op::OpTypeBool:
			case spv::Op::OpTypeInt:
			case spv::Op::OpTypeFloat:
			case spv::Op::OpTypeVector:
			case spv::Op::OpTypeMatrix:
			case spv::Op::OpTypeArray:
			case spv::Op::OpTypeStruct:
			case spv::Op::OpTypePointer:
				m_types.emplace(args[0], Type{op, {args.begin() + 1, args.end()}});
				break;
			case spv::Op::OpConstant:
			case spv::Op::OpSpecConstant:
				m_constants.emplace(args[1], args[2]);
				break;
			case spv::Op::OpVariable:
				m_variables.emplace(args[1], std::pair{args[0], static_cast<spv::StorageClass>(args[2])});
				break;
			case spv::Op::OpDecorate:
				Decorate(m_decorations[args[0]], args.subspan(1));
				break;
			case spv::Op::OpMemberDecorate:
				Decorate(m_members[{args[0], args[1]}], args.subspan(2));
				break;
			default:
				break;
			}
			offset += count;
		}
	}

	std::array<uint32_t, 3> WorkgroupSize() const
	{
		auto size = m_localSize;
		if (m_sizeUsesIds)
			for (auto& dimension : size)
				dimension = m_constants.at(dimension);
		return size;
	}

	Interface Read(spv::StorageClass storage) const
	{
		Interface result;
		for (const auto id : m_interfaces)
		{
			const auto& [pointer, variableStorage] = m_variables.at(id);
			if (variableStorage != storage) continue;
			const auto found = m_decorations.find(id);
			const Decoration decoration = found == m_decorations.end() ? Decoration{} : found->second;
			Flatten(m_types.at(pointer).operands.at(1), decoration, result);
		}
		return result;
	}

	std::map<Binding, BufferLayout> Resources() const
	{
		std::map<Binding, BufferLayout> result;
		for (const auto& [id, variable] : m_variables)
		{
			const auto [pointer, storage] = variable;
			const auto& d = Decorations(id);
			if (!d.binding && storage != spv::StorageClass::PushConstant)
				continue;
			result.emplace(Binding{storage, d.descriptorSet, d.binding.value_or(0)},
			               Layout(m_types.at(pointer).operands.at(1)));
		}
		return result;
	}

private:
	const Decoration& Decorations(uint32_t id) const
	{
		static const Decoration empty;
		const auto it = m_decorations.find(id);
		return it == m_decorations.end() ? empty : it->second;
	}

	BufferLayout Layout(uint32_t id) const
	{
		const auto& [type, args] = m_types.at(id);
		BufferLayout layout{type, {}, {}};
		layout.arrayStride = Decorations(id).arrayStride;
		if (type == spv::Op::OpTypeStruct)
		{
			for (uint32_t i = 0; i < args.size(); ++i)
			{
				auto member = Layout(args[i]);
				const auto it = m_members.find({id, i});
				if (it != m_members.end())
				{
					member.offset = it->second.offset;
					member.matrixStride = it->second.matrixStride;
					member.rowMajor = it->second.rowMajor;
				}
				layout.members.push_back(std::move(member));
			}
		}
		else if (type == spv::Op::OpTypeArray || type == spv::Op::OpTypeRuntimeArray ||
		         type == spv::Op::OpTypeMatrix || type == spv::Op::OpTypeVector ||
		         type == spv::Op::OpTypeImage || type == spv::Op::OpTypeSampledImage)
		{
			layout.members.push_back(Layout(args.at(0)));
			layout.operands.assign(args.begin() + 1, args.end());
			if (type == spv::Op::OpTypeArray)
				layout.operands[0] = m_constants.at(args[1]);
		}
		else
			layout.operands = args;
		return layout;
	}

	static void Decorate(Decoration& result, std::span<const uint32_t> args)
	{
		switch (static_cast<spv::Decoration>(args[0]))
		{
		case spv::Decoration::Binding: result.binding = args[1]; break;
		case spv::Decoration::DescriptorSet: result.descriptorSet = args[1]; break;
		case spv::Decoration::Offset: result.offset = args[1]; break;
		case spv::Decoration::ArrayStride: result.arrayStride = args[1]; break;
		case spv::Decoration::MatrixStride: result.matrixStride = args[1]; break;
		case spv::Decoration::RowMajor: result.rowMajor = true; break;
		case spv::Decoration::Location: result.location = args[1]; break;
		case spv::Decoration::Component: result.component = args[1]; break;
		case spv::Decoration::BuiltIn: result.builtin = true; break;
		default: break;
		}
	}

	uint32_t Flatten(uint32_t id, Decoration decoration, Interface& result) const
	{
		if (decoration.builtin) return 0;
		const auto& type = m_types.at(id);
		const auto& args = type.operands;
		if (type.op == spv::Op::OpTypeStruct)
		{
			uint32_t consumed = 0;
			for (uint32_t member = 0; member < args.size(); ++member)
			{
				auto memberDecoration = decoration;
				if (memberDecoration.location) *memberDecoration.location += consumed;
				const auto found = m_members.find({id, member});
				if (found != m_members.end())
				{
					if (found->second.location) memberDecoration.location = found->second.location;
					memberDecoration.component = found->second.component;
					memberDecoration.builtin = found->second.builtin;
				}
				consumed += Flatten(args[member], memberDecoration, result);
			}
			return consumed;
		}
		if (!decoration.location)
			throw std::runtime_error("user stage interface has no location");
		if (type.op == spv::Op::OpTypeArray || type.op == spv::Op::OpTypeMatrix)
		{
			const auto count = type.op == spv::Op::OpTypeArray ? m_constants.at(args[1]) : args[1];
			uint32_t consumed = 0;
			for (uint32_t element = 0; element < count; ++element)
			{
				auto elementDecoration = decoration;
				*elementDecoration.location += consumed;
				consumed += Flatten(args[0], elementDecoration, result);
			}
			return consumed;
		}
		const auto& scalar = type.op == spv::Op::OpTypeVector ? m_types.at(args[0]) : type;
		if (scalar.op != spv::Op::OpTypeFloat && scalar.op != spv::Op::OpTypeInt)
			throw std::runtime_error("unsupported stage interface scalar type");
		const uint32_t components = type.op == spv::Op::OpTypeVector ? args[1] : 1;
		const uint32_t scalarSlots = scalar.operands.at(0) == 64 ? 2 : 1;
		for (uint32_t component = 0; component < components * scalarSlots; ++component)
		{
			const auto slot = decoration.component + component;
			const Slot key{*decoration.location + slot / 4, slot % 4};
			if (!result.emplace(key, std::pair{scalar.op, scalar.operands}).second)
				throw std::runtime_error("overlapping stage interface locations");
		}
		return (decoration.component + components * scalarSlots + 3) / 4;
	}

	std::map<uint32_t, Type> m_types;
	std::map<uint32_t, uint32_t> m_constants;
	std::map<uint32_t, std::pair<uint32_t, spv::StorageClass>> m_variables;
	std::map<uint32_t, Decoration> m_decorations;
	std::map<std::pair<uint32_t, uint32_t>, Decoration> m_members;
	std::vector<uint32_t> m_interfaces;
	uint32_t m_entry = 0;
	std::array<uint32_t, 3> m_localSize{};
	bool m_sizeUsesIds = false;
};

} // namespace

std::string PassShaderInterfaceProblem(const ShaderUnit& expected, const ShaderUnit& selected, ShaderType stage)
{
	return PassShaderInterfaceProblem(expected.parsed, expected.spv, selected.parsed, selected.spv, stage);
}

std::string PassShaderInterfaceProblem(const ShaderStruct& a, std::span<const uint32_t> expectedSpv,
                                       const ShaderStruct& b, std::span<const uint32_t> selectedSpv,
                                       ShaderType stage)
{
	if (std::tie(a.local_size_x, a.local_size_y, a.local_size_z) != std::tie(b.local_size_x, b.local_size_y, b.local_size_z))
		return "compute workgroup size differs from the pass dispatch";
	// Keep the whole declared descriptor layout even when the compiler optimizes
	// away an unused image. Socket names may change without changing its binding.
	auto sameUniform = [](const S_Uniform& x, const S_Uniform& y)
	{
		return std::tie(x.binding, x.type, x.actualType, x.count, x.imageFormat, x.qualifiers) ==
		       std::tie(y.binding, y.type, y.actualType, y.count, y.imageFormat, y.qualifiers);
	};
	if (!std::is_permutation(a.uniform_list.begin(), a.uniform_list.end(), b.uniform_list.begin(), b.uniform_list.end(), sameUniform) ||
	    a.ubuffer_list != b.ubuffer_list || a.SB_list != b.SB_list)
		return "descriptor contract differs from the pass";
	if (a.push_constants != b.push_constants)
		return "push-constant contract differs from the pass";

	const auto model = stage == ShaderType::COMPUTE ? spv::ExecutionModel::GLCompute :
	                   stage == ShaderType::VERTEX ? spv::ExecutionModel::Vertex : spv::ExecutionModel::Fragment;
	const StageInterface original(expectedSpv, model), edited(selectedSpv, model);
	if (stage == ShaderType::COMPUTE && original.WorkgroupSize() != edited.WorkgroupSize())
		return "compiled workgroup size differs from the pass dispatch";
	const auto bindings = original.Resources();
	for (const auto& [binding, layout] : edited.Resources())
	{
		const auto found = bindings.find(binding);
		if (found == bindings.end() || found->second != layout)
			return "incompatible descriptor or push-constant layout at binding " + std::to_string(std::get<2>(binding));
	}
	if (stage == ShaderType::VERTEX && original.Read(spv::StorageClass::Input) != edited.Read(spv::StorageClass::Input))
		return "vertex attributes differ from the pass";
	if (stage == ShaderType::FRAGMENT && original.Read(spv::StorageClass::Output) != edited.Read(spv::StorageClass::Output))
		return "fragment outputs differ from the pass";
	return {};
}

bool ValidateRenderShaderInterface(std::span<const uint32_t> vertex,
                                  std::span<const uint32_t> fragment,
                                  std::string& error)
{
	error.clear();
	try
	{
		const auto outputs = StageInterface(vertex, spv::ExecutionModel::Vertex).Read(spv::StorageClass::Output);
		const auto inputs = StageInterface(fragment, spv::ExecutionModel::Fragment).Read(spv::StorageClass::Input);
		for (const auto& [slot, type] : inputs)
		{
			const auto found = outputs.find(slot);
			if (found == outputs.end() || found->second != type)
			{
				error = "Fragment input at location " + std::to_string(slot.first)
					+ ", component " + std::to_string(slot.second) + " has no compatible vertex output";
				return false;
			}
		}
		return true;
	}
	catch (const std::exception& exception)
	{
		error = std::string("Cannot reflect shader interface: ") + exception.what();
		return false;
	}
}

} // namespace neurus
