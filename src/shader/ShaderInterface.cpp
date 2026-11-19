/**
 * @file ShaderInterface.cpp
 * @brief Reflects SPIR-V stage locations without GPU or renderer dependencies.
 */
#include "shader/ShaderInterface.hpp"

#include <glslang/SPIRV/spirv.hpp11>

#include <map>
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
};

struct Type
{
	spv::Op op;
	std::vector<uint32_t> operands;
};

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

private:
	static void Decorate(Decoration& result, std::span<const uint32_t> args)
	{
		switch (static_cast<spv::Decoration>(args[0]))
		{
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
};

} // namespace

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
