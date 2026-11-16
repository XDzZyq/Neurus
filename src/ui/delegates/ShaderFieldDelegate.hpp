#pragma once

#include <QStyledItemDelegate>

class QAbstractItemModel;
class QComboBox;
class QEvent;
class QModelIndex;
class QPainter;
class QStyleOptionViewItem;
class QWidget;

namespace neurus {

/**
 * @brief Delegate for editing shader struct fields in a QTreeView.
 *
 * Renders a QComboBox for type columns and a QLineEdit for name columns. On
 * section / struct-def rows it paints a "+" button at the right edge of the
 * spanned cell and emits addClicked on a left click there - a plain-delegate
 * control, so the tree keeps no per-row index widgets.
 */
class ShaderFieldDelegate : public QStyledItemDelegate
{
	Q_OBJECT

public:
	explicit ShaderFieldDelegate(QObject* parent = nullptr);
	~ShaderFieldDelegate() override = default;

	ShaderFieldDelegate(const ShaderFieldDelegate&) = delete;
	ShaderFieldDelegate& operator=(const ShaderFieldDelegate&) = delete;

	QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
	                      const QModelIndex& index) const override;
	void setEditorData(QWidget* editor, const QModelIndex& index) const override;
	void setModelData(QWidget* editor, QAbstractItemModel* model,
	                  const QModelIndex& index) const override;
	void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
	                          const QModelIndex& index) const override;

	/** @brief Paints the "+" glyph at the right edge of section/struct-def rows. */
	void paint(QPainter* painter, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override;

	/** @brief Handles left clicks on the "+" glyph (emits addClicked). */
	bool editorEvent(QEvent* event, QAbstractItemModel* model,
	                 const QStyleOptionViewItem& option,
	                 const QModelIndex& index) override;

signals:
	/** @brief Emitted when the "+" glyph of a section / struct-def row is clicked. */
	void addClicked(const QModelIndex& index);

private:
	void populateTypeCombo(QComboBox* combo) const;

	/** @brief Returns the "+" button rect within @p cell (must match paint). */
	static QRect AddButtonRect(const QRect& cell);
};

} // namespace neurus
