#pragma once

#include <QWidget>

#include <string>

class QComboBox;
class QLineEdit;

namespace neurus
{

/**
 * @brief Reusable widget for a single shader field (type combo + name field).
 *
 * setField() dirty-checks against cached values to avoid redundant updates.
 */
class ShaderFieldRow : public QWidget
{
	Q_OBJECT

public:
	explicit ShaderFieldRow(QWidget* parent = nullptr);
	~ShaderFieldRow() override = default;

	ShaderFieldRow(const ShaderFieldRow&) = delete;
	ShaderFieldRow& operator=(const ShaderFieldRow&) = delete;

	/** @brief Set field data with type and name. Dirty-checked. */
	void setField(const std::string& type, const std::string& name);

	/** @brief Make all widgets read-only. */
	void setReadOnly(bool readOnly);

	/** @brief Reset all cached values to sentinel. Call when field identity changes. */
	void resetCaches();

signals:
	/** @brief Emitted when type or name changes (user edit). */
	void fieldChanged(const QString& type, const QString& name);

private:
	/** @brief Populate the type combo with GLSL types from ShaderStruct::type_table. */
	void populateTypeCombo();

	QComboBox* m_typeCombo = nullptr;
	QLineEdit* m_nameField = nullptr;

	// Cached values for dirty-check
	std::string m_cachedType;
	std::string m_cachedName;
};

} // namespace neurus
