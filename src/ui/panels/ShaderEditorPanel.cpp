#include "ShaderEditorPanel.h"

#include "items/CodeEditor.h"
#include "delegates/ShaderFieldDelegate.h"
#include "models/ShaderStructModel.h"
#include "UIContext.h"
#include "ui/utils/I18n.h"
#include "scene/Scene.h"
#include "render/shaders/ShaderUnit.h"
#include "render/shaders/ShaderStruct.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTextStream>
#include <QTreeView>
#include <QVBoxLayout>

#include <vector>

namespace neurus
{

// =========================================================================
// Tree state helpers (file-local)
// =========================================================================

namespace
{

/** @brief Returns the row path of @p idx: [rootRow, ..., idxRow]. */
std::vector<int> indexPathOf(const QModelIndex& idx)
{
	std::vector<int> path;
	for (QModelIndex cur = idx; cur.isValid(); cur = cur.parent())
		path.insert(path.begin(), cur.row());
	return path;
}

/** @brief Collects row paths of all expanded nodes under @p parent. */
void collectExpandedPaths(QTreeView* tree, QAbstractItemModel* model,
                          const QModelIndex& parent, std::vector<std::vector<int>>& out)
{
	for (int row = 0; row < model->rowCount(parent); ++row)
	{
		QModelIndex idx = model->index(row, 0, parent);
		if (tree->isExpanded(idx))
		{
			out.push_back(indexPathOf(idx));
			collectExpandedPaths(tree, model, idx, out);
		}
	}
}

/** @brief Resolves a row path back to a QModelIndex after a model rebuild. */
QModelIndex indexFromPath(QAbstractItemModel* model, const std::vector<int>& path)
{
	QModelIndex parent;
	for (int row : path)
	{
		QModelIndex child = model->index(row, 0, parent);
		if (!child.isValid())
			return QModelIndex();
		parent = child;
	}
	return parent;
}

} // namespace

// =========================================================================
// Constructor
// =========================================================================

ShaderEditorPanel::ShaderEditorPanel(QWidget* parent)
	: UIPanel(kType, "Shader Editor", parent)
{
	auto* rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(4, 4, 4, 4);
	rootLayout->setSpacing(4);

	// --- Top toolbar (two rows) ---
	auto* toolbarContainer = new QVBoxLayout();
	toolbarContainer->setContentsMargins(0, 0, 0, 0);
	toolbarContainer->setSpacing(2);

	// Row 1: Mode + Stage
	auto* row1 = new QHBoxLayout();
	m_modeLabel = new QLabel("Mode:", this);
	row1->addWidget(m_modeLabel);
	m_modeCombo = new QComboBox(this);
	m_modeCombo->addItem("Code");
	m_modeCombo->addItem("Structure");
	m_modeCombo->setCurrentIndex(1);
	m_modeCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	row1->addWidget(m_modeCombo, 1);
	row1->addSpacing(8);
	m_stageLabel = new QLabel("Stage:", this);
	row1->addWidget(m_stageLabel);
	m_stageCombo = new QComboBox(this);
	m_stageCombo->addItem("VERTEX");
	m_stageCombo->addItem("FRAGMENT");
	m_stageCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	row1->addWidget(m_stageCombo, 1);
	toolbarContainer->addLayout(row1);

	// Row 2: Compile + Save
	auto* row2 = new QHBoxLayout();
	m_compileBtn = new QPushButton("Compile", this);
	m_compileBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	row2->addWidget(m_compileBtn, 1);
	m_saveBtn = new QPushButton("Save", this);
	m_saveBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	row2->addWidget(m_saveBtn, 1);
	toolbarContainer->addLayout(row2);

	rootLayout->addLayout(toolbarContainer);

	// --- Empty state label (shown above content stack in both modes) ---
	// Text is filled in by Retranslate() once the widget tree is built.
	m_emptyLabel = new QLabel(this);
	m_emptyLabel->setAlignment(Qt::AlignCenter);
	m_emptyLabel->setStyleSheet("color: gray;");
	m_emptyLabel->setVisible(false);
	m_emptyLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	rootLayout->addWidget(m_emptyLabel);

	// --- Create Shader button (shown above content stack in both modes) ---
	m_createBtn = new QPushButton("Create Shader", this);
	m_createBtn->setVisible(false);
	m_createBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	rootLayout->addWidget(m_createBtn);

	// --- Content stack ---
	m_contentStack = new QStackedWidget(this);

	// Page 0: Code Editor
	auto* codePage = new QWidget();
	auto* codeLayout = new QVBoxLayout(codePage);
	codeLayout->setContentsMargins(0, 0, 0, 0);
	m_codeEditor = new CodeEditor(this);
	m_codeEditor->setLanguage(CodeEditor::Language::GLSL);
	codeLayout->addWidget(m_codeEditor);
	m_contentStack->addWidget(codePage);

	// Page 1: Struct Editor
	auto* structPage = new QWidget();
	auto* structPageLayout = new QVBoxLayout(structPage);
	structPageLayout->setContentsMargins(0, 0, 0, 0);
	structPageLayout->setSpacing(2);

	auto* treeToolbar = new QHBoxLayout();
	m_removeBtn = new QPushButton("-", structPage);
	m_removeBtn->setToolTip("Remove entry");
	m_removeBtn->setFixedSize(24, 24);
	m_removeBtn->setEnabled(false);
	treeToolbar->addWidget(m_removeBtn);
	treeToolbar->addStretch();
	structPageLayout->addLayout(treeToolbar);

	m_treeView = new QTreeView(structPage);
	m_model = new ShaderStructModel(this);
	m_delegate = new ShaderFieldDelegate(this);
	m_treeView->setModel(m_model);
	m_treeView->setItemDelegate(m_delegate);

	// "+" clicks on section / struct-def rows flow through the delegate (the
	// glyph is painted at the row's right edge — no per-row index widgets,
	// which fought with the frequent model resets and macOS accessibility).
	QObject::connect(m_delegate, &ShaderFieldDelegate::addClicked,
	                 this, &ShaderEditorPanel::handleAddClick);
	m_treeView->setAlternatingRowColors(true);
	m_treeView->setUniformRowHeights(true);
	m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_treeView->setSelectionMode(QAbstractItemView::SingleSelection);
	m_treeView->setIndentation(12);  // tighter indentation so struct members have more space

	// Load tree stylesheet from Qt resource (qml/shader_editor.qss)
	{
		QFile file(":/qml/shader_editor.qss");
		if (file.open(QIODevice::ReadOnly | QIODevice::Text))
			m_treeView->setStyleSheet(QTextStream(&file).readAll());
	}
	structPageLayout->addWidget(m_treeView, 1);

	m_contentStack->addWidget(structPage);

	// Default to Struct Editor
	m_contentStack->setCurrentIndex(1);

	rootLayout->addWidget(m_contentStack, 1);

	// --- Mode combo -> switch pages ---
	QObject::connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
	                 m_contentStack, &QStackedWidget::setCurrentIndex);

	// --- Compile button -> emit with unitType (0=Code, 1=Struct) ---
	QObject::connect(m_compileBtn, &QPushButton::clicked, [this]()
	{
		if (m_activeObjectId != 0)
		{
			int unitType = (m_contentStack->currentIndex() == 0) ? 0 : 1;
			emit compileRequested({m_activeObjectId, m_cachedStageType, unitType});
		}
	});

	// --- CodeEditor text changes -> codeEdited event ---
	QObject::connect(m_codeEditor, &CodeEditor::codeChanged, [this](const std::string& code)
	{
		if (m_activeObjectId != 0)
			emit codeEdited({m_activeObjectId, m_cachedStageType, code});
	});

	// --- CodeEditor focus in/out -> gesture brackets for undo coalescing ---
	// A keystroke burst between focus-in and focus-out collapses to one undo
	// entry recorded on focus-out (debounce_discrete).
	QObject::connect(m_codeEditor, &CodeEditor::editingStarted, [this]()
	{
		if (m_activeObjectId != 0)
			emit editBegin({m_activeObjectId, m_cachedStageType});
	});
	QObject::connect(m_codeEditor, &CodeEditor::editingFinished, [this]()
	{
		if (m_activeObjectId != 0)
			emit editEnd({m_activeObjectId, m_cachedStageType});
	});

	// --- Create Shader button -> event signal ---
	QObject::connect(m_createBtn, &QPushButton::clicked, [this]()
	{
		if (m_activeObjectId != 0)
			emit createShaderRequested({m_activeObjectId});
	});

	// --- Stage combo change -> invalidate cache ---
	QObject::connect(m_stageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
	                 [this](int index) {
		m_cachedStageType = index;
		m_activeObjectId = 0;  // Re-read on next Refresh
	});

	// --- Tree model edits -> structEdited event ---
	QObject::connect(m_model, &ShaderStructModel::fieldEdited,
	                 [this](ShaderSection section, int fieldIndex, int subFieldIndex,
	                        const QString& field, const QString& value) {
		if (m_activeObjectId != 0)
		{
			emit structEdited({m_activeObjectId, m_cachedStageType, section,
			                   fieldIndex, subFieldIndex, field.toStdString(), value.toStdString()});
			m_cachedShaderVersion = -1;  // invalidate cache -> re-read on next Refresh
		}
	});

	// Start with empty state
	setShowEmptyState(true);

	// Apply the active language (toolbar was built with English literals).
	Retranslate();
}

// =========================================================================
// Retranslate - re-apply toolbar / button texts in the active language
// =========================================================================

void ShaderEditorPanel::Retranslate()
{
	auto& i18n = I18n::instance();

	m_modeLabel->setText(i18n.translate("Mode:"));
	m_modeCombo->setItemText(0, i18n.translate("Code"));
	m_modeCombo->setItemText(1, i18n.translate("Structure"));
	m_stageLabel->setText(i18n.translate("Stage:"));
	// VERTEX / FRAGMENT are GLSL keywords — intentionally untranslated.
	m_compileBtn->setText(i18n.translate("Compile"));
	m_saveBtn->setText(i18n.translate("Save"));
	m_emptyLabel->setText(i18n.translate("No object selected"));
	m_createBtn->setText(i18n.translate("Create Shader"));
	m_removeBtn->setToolTip(i18n.translateCtx("Remove entry", "Tooltip"));

	// The structure tree's column headers and section titles are model-side.
	m_model->Retranslate();
}

// =========================================================================
// Refresh
// =========================================================================

void ShaderEditorPanel::Refresh(const UIContext& ctx)
{
	const auto* scene = static_cast<const Scene*>(ctx.editor.scene);
	if (!scene)
	{
		setShowEmptyState(true);
		setShowCreateButton(false);
		m_activeObjectId = 0;
		return;
	}

	const auto* activeObj = scene->selections.GetActiveObject();
	if (!activeObj)
	{
		setShowEmptyState(true);
		setShowCreateButton(false);
		m_activeObjectId = 0;
		return;
	}

	// Only handle GO_MESH objects
	if (activeObj->o_type != ObjectID::GOType::GO_MESH)
	{
		setShowEmptyState(true);
		setShowCreateButton(false);
		m_activeObjectId = 0;
		return;
	}

	const int objectId = activeObj->GetObjectID();

	// Get shader unit and its version (-1 if no shader)
	int unitVersion = -1;
	void* unitPtr = activeObj->GetShaderUnit(m_cachedStageType);
	if (unitPtr)
		unitVersion = static_cast<const ShaderUnit*>(unitPtr)->GetVersion();

	// Dirty-check: same object UID AND same version = nothing changed
	if (objectId == m_activeObjectId && unitVersion == m_cachedShaderVersion)
		return;

	bool objectChanged = (objectId != m_activeObjectId);
	m_activeObjectId = objectId;
	m_cachedShaderVersion = unitVersion;

	if (!unitPtr)
	{
		// No shader assigned — show create button, hide everything else
		m_emptyLabel->setVisible(false);
		m_showingEmptyState = false;
		setShowCreateButton(true);
		if (m_codeEditor) m_codeEditor->setVisible(false);
		m_treeView->setVisible(false);
		m_removeBtn->setVisible(false);
		return;
	}

	// Has shader — show content, hide create button and empty label
	m_emptyLabel->setVisible(false);
	m_showingEmptyState = false;
	setShowCreateButton(false);
	if (m_codeEditor) m_codeEditor->setVisible(true);
	m_treeView->setVisible(true);
	m_removeBtn->setVisible(true);
	populateSections(unitPtr, objectChanged);
}

// =========================================================================
// populateSections
// =========================================================================

void ShaderEditorPanel::populateSections(const void* shaderUnitPtr, bool objectChanged)
{
	auto* unit = static_cast<const ShaderUnit*>(shaderUnitPtr);
	const auto& parsed = unit->parsed;

	// Populate code editor with generated source
	m_codeEditor->setCode(unit->code);

	// Remember which sections/structs are expanded and the current selection
	// before the model rebuild: beginResetModel()/endResetModel() clears the
	// view's expansion state AND its selection, which would collapse every
	// section and drop the selected row on every refresh.
	std::vector<std::vector<int>> expandedPaths;
	std::vector<int> currentPath;
	const bool hadFocus = m_treeView->hasFocus();
	if (!objectChanged)
	{
		collectExpandedPaths(m_treeView, m_model, QModelIndex(), expandedPaths);
		QModelIndex current = m_treeView->currentIndex();
		if (current.isValid())
			currentPath = indexPathOf(current);
	}

	// Build tree model; only expand all when the object changes (preserve collapse state on refresh)
	m_model->setShaderStruct(&parsed);
	if (objectChanged)
	{
		m_treeView->expandAll();
	}
	else
	{
		for (const auto& path : expandedPaths)
		{
			QModelIndex idx = indexFromPath(m_model, path);
			if (idx.isValid())
				m_treeView->setExpanded(idx, true);
		}

		// Restore the selection so clicking "+" keeps the previously selected row.
		if (!currentPath.empty())
		{
			QModelIndex idx = indexFromPath(m_model, currentPath);
			if (idx.isValid())
				m_treeView->setCurrentIndex(idx);
		}
	}

	// Span the title across the full row for section headers and struct-def
	// rows; the delegate paints the "+" at the row's right edge. Re-applied on
	// every populate because the model reset clears spans.
	for (int row = 0; row < m_model->rowCount(QModelIndex()); ++row)
	{
		QModelIndex sectionIdx = m_model->index(row, 0, QModelIndex());
		m_treeView->setFirstColumnSpanned(row, QModelIndex(), true);
		for (int child = 0; child < m_model->rowCount(sectionIdx); ++child)
		{
			QModelIndex childIdx = m_model->index(child, 0, sectionIdx);
			if (childIdx.data(ShaderStructModel::RoleNodeType).toInt() ==
			    ShaderStructModel::NodeStructDef)
			{
				m_treeView->setFirstColumnSpanned(child, sectionIdx, true);
			}
		}
	}

	// The model reset drops the view's keyboard focus. Restore it so window
	// shortcuts (Ctrl+Z) keep working: with no focus widget, macOS accessibility
	// chases the dangling focused table and swallows key input until the user
	// clicks elsewhere ("undo not working after add" bug).
	if (hadFocus)
		m_treeView->setFocus();
}

// =========================================================================
// Per-row add handling (delegate-painted "+" column -> handleAddClick)
// =========================================================================

void ShaderEditorPanel::handleAddClick(const QModelIndex& index)
{
	if (m_activeObjectId == 0 || !index.isValid())
		return;

	m_treeView->expand(index);  // reveal the appended row

	// Section row -> append to that section; struct def row -> add a member field.
	int nodeType = index.data(ShaderStructModel::RoleNodeType).toInt();
	ShaderSection section = static_cast<ShaderSection>(index.data(ShaderStructModel::RoleSection).toInt());
	int subFieldIndex = -1;
	if (nodeType == ShaderStructModel::NodeStructDef)
		subFieldIndex = index.data(ShaderStructModel::RoleFieldIndex).toInt();

	emit fieldAdded({m_activeObjectId, m_cachedStageType, section, subFieldIndex});
	m_cachedShaderVersion = -1;  // invalidate cache -> re-read on next Refresh
}

// =========================================================================
// State visibility helpers
// =========================================================================

void ShaderEditorPanel::setShowEmptyState(bool show)
{
	if (m_showingEmptyState == show) return;
	m_showingEmptyState = show;
	m_emptyLabel->setVisible(show);

	// Hide all content when showing empty state
	if (m_codeEditor) m_codeEditor->setVisible(!show);
	m_treeView->setVisible(!show);
	m_removeBtn->setVisible(!show);
}

void ShaderEditorPanel::setShowCreateButton(bool show)
{
	if (m_showingCreateButton == show) return;
	m_showingCreateButton = show;
	m_createBtn->setVisible(show);
}

} // namespace neurus
