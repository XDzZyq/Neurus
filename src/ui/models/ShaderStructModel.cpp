#include "models/ShaderStructModel.h"

#include "ui/utils/I18n.h"

#include <QFont>

namespace neurus
{

ShaderStructModel::ShaderStructModel(QObject* parent)
	: QAbstractItemModel(parent)
{
}

QModelIndex ShaderStructModel::index(int row, int column, const QModelIndex& parent) const
{
	if (!m_root)
		return QModelIndex();
	if (row < 0 || column < 0 || column >= 2)
		return QModelIndex();

	Node* parentNode = parent.isValid() ? static_cast<Node*>(parent.internalPointer()) : m_root.get();
	if (row < 0 || row >= static_cast<int>(parentNode->children.size()))
		return QModelIndex();

	return createIndex(row, column, parentNode->children[row].get());
}

QModelIndex ShaderStructModel::parent(const QModelIndex& child) const
{
	if (!child.isValid())
		return QModelIndex();

	Node* childNode = static_cast<Node*>(child.internalPointer());
	Node* parentNode = childNode->parent;
	if (!parentNode || parentNode == m_root.get())
		return QModelIndex();

	Node* grandParent = parentNode->parent;
	if (!grandParent)
		return QModelIndex();

	for (int i = 0; i < static_cast<int>(grandParent->children.size()); ++i)
	{
		if (grandParent->children[i].get() == parentNode)
			return createIndex(i, 0, parentNode);
	}
	return QModelIndex();
}

int ShaderStructModel::rowCount(const QModelIndex& parent) const
{
	if (!m_root)
		return 0;

	Node* parentNode = parent.isValid() ? static_cast<Node*>(parent.internalPointer()) : m_root.get();
	return static_cast<int>(parentNode->children.size());
}

int ShaderStructModel::columnCount(const QModelIndex& /*parent*/) const
{
	return 2;
}

QVariant ShaderStructModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || !m_struct)
		return QVariant();

	Node* node = static_cast<Node*>(index.internalPointer());

	if (role == Qt::DisplayRole || role == Qt::EditRole)
		return nodeString(node, index.column());

	if (role == Qt::FontRole && node->type == NodeSection)
	{
		QFont font;
		font.setBold(true);
		return font;
	}

	if (role == RoleNodeType)
		return static_cast<int>(node->type);
	if (role == RoleSection)
		return node->sectionIndex;
	if (role == RoleFieldIndex)
		return node->fieldIndex;
	if (role == RoleSubFieldIndex)
		return node->memberIndex;

	return QVariant();
}

bool ShaderStructModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
	if (role != Qt::EditRole || !index.isValid() || !m_struct)
		return false;

	Node* node = static_cast<Node*>(index.internalPointer());
	if (node->type == NodeSection)
		return false;

	// Struct def rows are name-only (span both columns) — always "name"
	QString field;
	if (node->type == NodeStructDef)
		field = "name";
	else
		field = (index.column() == 0) ? "type" : "name";
	int subFieldIndex = (node->type == NodeStructMember) ? node->memberIndex : -1;

	emit fieldEdited(static_cast<ShaderSection>(node->sectionIndex), node->fieldIndex,
	                 subFieldIndex, field, value.toString());
	return true;
}

Qt::ItemFlags ShaderStructModel::flags(const QModelIndex& index) const
{
	if (!index.isValid())
		return Qt::NoItemFlags;

	Node* node = static_cast<Node*>(index.internalPointer());
	if (node->type == NodeSection)
		return Qt::ItemIsEnabled;

	// Struct def: column 1 is empty (spanned) — not editable
	if (node->type == NodeStructDef && index.column() == 1)
		return Qt::NoItemFlags;

	return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

QVariant ShaderStructModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
	{
		auto& i18n = I18n::instance();
		if (section == 0) return i18n.translate("Type");
		if (section == 1) return i18n.translate("Name");
	}
	return QVariant();
}

void ShaderStructModel::Retranslate()
{
	emit headerDataChanged(Qt::Horizontal, 0, 1);
	if (!m_root || m_root->children.empty())
		return;

	// Only the section rows carry translated text, and they are exactly the
	// top-level rows — their children are shader identifiers, which are code,
	// not UI text. Column 0 is the one that shows the title.
	const int last = static_cast<int>(m_root->children.size()) - 1;
	emit dataChanged(index(0, 0), index(last, 0), { Qt::DisplayRole });
}

void ShaderStructModel::setShaderStruct(const ShaderStruct* shaderStruct)
{
	beginResetModel();
	m_struct = shaderStruct;
	buildTree();
	endResetModel();
}

void ShaderStructModel::buildTree()
{
	m_root = std::make_unique<Node>();
	m_root->type = NodeSection;
	m_root->sectionIndex = -1;

	if (!m_struct)
		return;

	addSection(ShaderSection::Attributes, static_cast<int>(m_struct->AB_list.size()));
	addSection(ShaderSection::PassOutputs, static_cast<int>(m_struct->pass_list.size()));
	addSection(ShaderSection::Inputs, static_cast<int>(m_struct->input_list.size()));
	addSection(ShaderSection::Outputs, static_cast<int>(m_struct->output_list.size()));
	addSection(ShaderSection::Uniforms, static_cast<int>(m_struct->uniform_list.size()));
	addSection(ShaderSection::StructDefs, static_cast<int>(m_struct->struct_def_list.size()));
	addSection(ShaderSection::Functions, static_cast<int>(m_struct->func_list.size()));
	addSection(ShaderSection::PushConstants, static_cast<int>(m_struct->push_constants.size()));
}

void ShaderStructModel::addSection(ShaderSection section, int count)
{
	auto sectionNode = std::make_unique<Node>();
	sectionNode->type = NodeSection;
	sectionNode->sectionIndex = static_cast<int>(section);
	sectionNode->parent = m_root.get();
	Node* sectionPtr = sectionNode.get();
	m_root->children.push_back(std::move(sectionNode));

	if (section == ShaderSection::StructDefs)
	{
		for (int i = 0; i < count; ++i)
		{
			auto structNode = std::make_unique<Node>();
			structNode->type = NodeStructDef;
			structNode->sectionIndex = static_cast<int>(section);
			structNode->fieldIndex = i;
			structNode->parent = sectionPtr;
			Node* structPtr = structNode.get();
			sectionPtr->children.push_back(std::move(structNode));

			const auto& fields = m_struct->struct_def_list[i].fields;
			for (int j = 0; j < static_cast<int>(fields.size()); ++j)
			{
				auto memberNode = std::make_unique<Node>();
				memberNode->type = NodeStructMember;
				memberNode->sectionIndex = static_cast<int>(section);
				memberNode->fieldIndex = i;
				memberNode->memberIndex = j;
				memberNode->parent = structPtr;
				structPtr->children.push_back(std::move(memberNode));
			}
		}
	}
	else
	{
		for (int i = 0; i < count; ++i)
		{
			auto fieldNode = std::make_unique<Node>();
			fieldNode->type = NodeField;
			fieldNode->sectionIndex = static_cast<int>(section);
			fieldNode->fieldIndex = i;
			fieldNode->parent = sectionPtr;
			sectionPtr->children.push_back(std::move(fieldNode));
		}
	}
}

QString ShaderStructModel::nodeString(const Node* node, int column) const
{
	if (!m_struct)
		return "";

	switch (node->type)
	{
		case NodeSection:
			return column == 0 ? sectionTitle(node->sectionIndex) : "";

		case NodeField:
		{
			switch (static_cast<ShaderSection>(node->sectionIndex))
			{
				case ShaderSection::Attributes:
				{
					const S_IO& io = m_struct->AB_list[node->fieldIndex];
					return column == 0
						? QString::fromStdString(io.typeName.empty() ? ShaderStruct::ParseType(io.type) : io.typeName)
						: QString::fromStdString(io.name);
				}
				case ShaderSection::PassOutputs:
				{
					const S_IO& io = m_struct->pass_list[node->fieldIndex];
					return column == 0
						? QString::fromStdString(io.typeName.empty() ? ShaderStruct::ParseType(io.type) : io.typeName)
						: QString::fromStdString(io.name);
				}
				case ShaderSection::Inputs:
				case ShaderSection::Outputs:
				case ShaderSection::Uniforms:
				{
					const S_Uniform& u = getUniform(node->sectionIndex, node->fieldIndex);
					return column == 0
						? QString::fromStdString(u.actualType.empty() ? ShaderStruct::ParseType(u.type) : u.actualType)
						: QString::fromStdString(u.name);
				}
				case ShaderSection::Functions:
				{
					const S_Func& f = m_struct->func_list[node->fieldIndex];
					return column == 0
						? QString::fromStdString(ShaderStruct::ParseType(f.returnType))
						: QString::fromStdString(f.name);
				}
				case ShaderSection::PushConstants:
				{
					const S_PushConstant& pc = m_struct->push_constants[node->fieldIndex];
					return column == 0
						? QString::fromStdString(pc.typeName)
						: QString::fromStdString(pc.name);
				}
				default:
					return "";
			}
		}

case NodeStructDef:
	{
		const S_StructDef& sd = m_struct->struct_def_list[node->fieldIndex];
		// Name-only row (column 1 is empty — first column is spanned)
		return column == 0 ? QString::fromStdString(sd.name) : "";
	}

		case NodeStructMember:
		{
			const S_IO& member = m_struct->struct_def_list[node->fieldIndex].fields[node->memberIndex];
			return column == 0
				? QString::fromStdString(member.typeName.empty() ? ShaderStruct::ParseType(member.type) : member.typeName)
				: QString::fromStdString(member.name);
		}
	}
	return "";
}

const S_Uniform& ShaderStructModel::getUniform(int sectionIndex, int fieldIndex) const
{
	static S_Uniform dummy;
	switch (static_cast<ShaderSection>(sectionIndex))
	{
		case ShaderSection::Inputs: return m_struct->input_list[fieldIndex];
		case ShaderSection::Outputs: return m_struct->output_list[fieldIndex];
		case ShaderSection::Uniforms: return m_struct->uniform_list[fieldIndex];
		default: return dummy;
	}
}

QString ShaderStructModel::sectionTitle(int sectionIndex) const
{
	auto& i18n = I18n::instance();
	switch (static_cast<ShaderSection>(sectionIndex))
	{
		case ShaderSection::Attributes: return i18n.translate("Attributes");
		case ShaderSection::PassOutputs: return i18n.translate("Pass Outputs");
		case ShaderSection::Inputs: return i18n.translate("Inputs");
		case ShaderSection::Outputs: return i18n.translate("Outputs");
		case ShaderSection::Uniforms: return i18n.translate("Uniforms");
		case ShaderSection::StructDefs: return i18n.translate("Struct Definitions");
		case ShaderSection::Functions: return i18n.translate("Functions");
		case ShaderSection::PushConstants: return i18n.translate("Push Constants");
		default: return "";
	}
}

} // namespace neurus
