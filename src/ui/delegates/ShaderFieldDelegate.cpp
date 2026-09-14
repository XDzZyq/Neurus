#include "delegates/ShaderFieldDelegate.h"
#include "models/ShaderStructModel.h"
#include "render/shaders/ShaderStruct.h"
#include "ui/utils/I18n.h"

#include <QComboBox>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace neurus
{

ShaderFieldDelegate::ShaderFieldDelegate(QObject* parent)
	: QStyledItemDelegate(parent)
{
}

QWidget* ShaderFieldDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& /*option*/,
                                           const QModelIndex& index) const
{
	if (!index.isValid())
		return nullptr;

	int nodeType = index.data(ShaderStructModel::RoleNodeType).toInt();
	if (nodeType == ShaderStructModel::NodeSection)
		return nullptr;

	if (index.column() == 0)
	{
		if (nodeType == ShaderStructModel::NodeField || nodeType == ShaderStructModel::NodeStructMember)
		{
			auto* combo = new QComboBox(parent);
			combo->setEditable(true);
			combo->setMinimumWidth(120);
			populateTypeCombo(combo);
			return combo;
		}
		// NodeStructDef: struct name edit (name only, no type selector)
		// Editors are created on demand, so translating here is enough — there
		// is no long-lived widget for a language switch to miss.
		auto* edit = new QLineEdit(parent);
		edit->setPlaceholderText(I18n::instance().translate("struct name"));
		return edit;
	}

	// Column 1: name / varName edit (not reached for struct defs — they span both columns)
	return new QLineEdit(parent);
}

void ShaderFieldDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
	QString value = index.data(Qt::EditRole).toString();

	if (auto* combo = qobject_cast<QComboBox*>(editor))
		combo->setCurrentText(value);
	else if (auto* lineEdit = qobject_cast<QLineEdit*>(editor))
		lineEdit->setText(value);
}

void ShaderFieldDelegate::setModelData(QWidget* editor, QAbstractItemModel* model,
                                       const QModelIndex& index) const
{
	QString value;
	if (auto* combo = qobject_cast<QComboBox*>(editor))
		value = combo->currentText();
	else if (auto* lineEdit = qobject_cast<QLineEdit*>(editor))
		value = lineEdit->text();
	else
		return;

	model->setData(index, value, Qt::EditRole);
}

void ShaderFieldDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                                              const QModelIndex& /*index*/) const
{
	editor->setGeometry(option.rect);
}

QRect ShaderFieldDelegate::AddButtonRect(const QRect& cell)
{
	const int size = 14;
	return QRect(cell.right() - size - 4, cell.center().y() - size / 2, size, size);
}

void ShaderFieldDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
	QStyledItemDelegate::paint(painter, option, index);

	// Section / struct-def rows span the full row; draw the "+" at its right
	// edge (a plain-delegate control — no index widgets to fight the resets).
	const int nodeType = index.data(ShaderStructModel::RoleNodeType).toInt();
	if (nodeType == ShaderStructModel::NodeSection ||
	    nodeType == ShaderStructModel::NodeStructDef)
	{
		painter->save();
		painter->setRenderHint(QPainter::Antialiasing, true);
		QStyleOptionButton btnOpt;
		btnOpt.rect = AddButtonRect(option.rect);
		btnOpt.state = QStyle::State_Enabled;
		if (option.state & QStyle::State_MouseOver)
			btnOpt.state |= QStyle::State_MouseOver;
		if (const QWidget* w = option.widget)
			w->style()->drawControl(QStyle::CE_PushButton, &btnOpt, painter, w);
		painter->drawText(btnOpt.rect, Qt::AlignCenter, QStringLiteral("+"));
		painter->restore();
	}
}

bool ShaderFieldDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                      const QStyleOptionViewItem& option,
                                      const QModelIndex& index)
{
	const int nodeType = index.data(ShaderStructModel::RoleNodeType).toInt();
	if (nodeType == ShaderStructModel::NodeSection ||
	    nodeType == ShaderStructModel::NodeStructDef)
	{
		if (event->type() == QEvent::MouseButtonRelease)
		{
			auto* me = static_cast<QMouseEvent*>(event);
			if (me->button() == Qt::LeftButton &&
			    AddButtonRect(option.rect).contains(me->pos()))
			{
				emit addClicked(index);
				return true; // consumed: the "+" is an action, not a row click
			}
		}
	}

	return QStyledItemDelegate::editorEvent(event, model, option, index);
}

void ShaderFieldDelegate::populateTypeCombo(QComboBox* combo) const
{
	combo->clear();
	combo->addItem("float");
	combo->addItem("vec2");
	combo->addItem("vec3");
	combo->addItem("vec4");
	combo->addItem("mat3");
	combo->addItem("mat4");
	combo->addItem("int");
	combo->addItem("uint");

	for (const auto& typeName : ShaderStruct::type_table)
	{
		if (typeName.empty())
			continue;
		if (combo->findText(QString::fromStdString(typeName)) == -1)
			combo->addItem(QString::fromStdString(typeName));
	}
}

} // namespace neurus
