#include "ShaderFieldRow.h"

#include "render/shaders/ShaderStruct.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>

namespace neurus
{

ShaderFieldRow::ShaderFieldRow(QWidget* parent)
	: QWidget(parent)
{
	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	m_typeCombo = new QComboBox(this);
	m_typeCombo->setMinimumWidth(90);
	populateTypeCombo();
	layout->addWidget(m_typeCombo);

	m_nameField = new QLineEdit(this);
	m_nameField->setPlaceholderText("name");
	layout->addWidget(m_nameField, 1);

	QObject::connect(m_typeCombo, &QComboBox::currentTextChanged,
	                 this, [this](const QString& text) {
		emit fieldChanged(text, m_nameField->text());
	});
	QObject::connect(m_nameField, &QLineEdit::textChanged,
	                 this, [this](const QString& text) {
		emit fieldChanged(m_typeCombo->currentText(), text);
	});
}

void ShaderFieldRow::populateTypeCombo()
{
	m_typeCombo->clear();
	// Add commonly used GLSL types
	m_typeCombo->addItem("float");
	m_typeCombo->addItem("vec2");
	m_typeCombo->addItem("vec3");
	m_typeCombo->addItem("vec4");
	m_typeCombo->addItem("mat3");
	m_typeCombo->addItem("mat4");
	m_typeCombo->addItem("int");
	m_typeCombo->addItem("uint");
	// Add all registered types from ShaderStruct type table
	for (const auto& typeName : ShaderStruct::type_table)
	{
		if (typeName.empty()) continue;
		// Skip duplicates from hardcoded list above
		if (m_typeCombo->findText(QString::fromStdString(typeName)) == -1)
			m_typeCombo->addItem(QString::fromStdString(typeName));
	}
}

void ShaderFieldRow::setField(const std::string& type, const std::string& name)
{
	if (m_cachedType == type && m_cachedName == name) return;
	m_cachedType = type;
	m_cachedName = name;

	QSignalBlocker lockType(m_typeCombo);
	QSignalBlocker lockName(m_nameField);

	int idx = m_typeCombo->findText(QString::fromStdString(type));
	if (idx >= 0)
		m_typeCombo->setCurrentIndex(idx);

	m_nameField->setText(QString::fromStdString(name));
}

void ShaderFieldRow::setReadOnly(bool readOnly)
{
	m_typeCombo->setEnabled(!readOnly);
	m_nameField->setReadOnly(readOnly);
}

void ShaderFieldRow::resetCaches()
{
	m_cachedType.clear();
	m_cachedName.clear();
}

} // namespace neurus
