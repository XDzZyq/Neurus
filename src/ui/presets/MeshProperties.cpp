#include "presets/MeshProperties.h"

#include "ui/utils/I18n.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QFont>

namespace neurus
{

MeshProperties::MeshProperties(QWidget* parent)
    : QWidget(parent)
{
	auto* outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);
	outerLayout->setSpacing(0);

	// --- Top-level group ---
	m_group = new QGroupBox(QStringLiteral("Mesh"));
	outerLayout->addWidget(m_group);

	auto* innerLayout = new QVBoxLayout(m_group);
	innerLayout->setSpacing(8);

	// --- Section: Asset ---
	m_assetLabel = new QLabel(QStringLiteral("Asset"));
	QFont boldFont = m_assetLabel->font();
	boldFont.setBold(true);
	m_assetLabel->setFont(boldFont);
	innerLayout->addWidget(m_assetLabel);

	// --- Row: Path label + mesh path ---
	auto* pathRow = new QHBoxLayout();
	pathRow->setSpacing(4);

	m_pathPrefix = new QLabel(QStringLiteral("Path"));
	m_pathPrefix->setStyleSheet(QStringLiteral("QLabel { color: #aaa; }"));
	pathRow->addWidget(m_pathPrefix);

	m_pathLabel = new QLabel();
	m_pathLabel->setWordWrap(true);
	m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	QFont pathFont = m_pathLabel->font();
	pathFont.setPointSize(pathFont.pointSize() - 1);
	m_pathLabel->setFont(pathFont);
	m_pathLabel->setStyleSheet(QStringLiteral("QLabel { color: #aaa; }"));
	pathRow->addWidget(m_pathLabel, 1);

	innerLayout->addLayout(pathRow);

	// --- Section: Flags ---
	m_flagsLabel = new QLabel(QStringLiteral("Flags"));
	m_flagsLabel->setFont(boldFont);
	innerLayout->addWidget(m_flagsLabel);

	// --- Row: Cast Shadow checkbox ---
	m_shadowChk = new QCheckBox(QStringLiteral("Cast Shadow"));
	innerLayout->addWidget(m_shadowChk);

	// --- Row: Use Material checkbox ---
	m_materialChk = new QCheckBox(QStringLiteral("Use Material"));
	innerLayout->addWidget(m_materialChk);

	outerLayout->addStretch();

	// Apply the active language (labels were built in English).
	Retranslate();

	// --- Signal wiring ---
	QObject::connect(m_shadowChk, &QCheckBox::toggled, this,
		[this](bool checked) {
			if (m_objectId < 0)
				return;
			emit shadowChanged(m_objectId, checked);
		});

	QObject::connect(m_materialChk, &QCheckBox::toggled, this,
		[this](bool checked) {
			if (m_objectId < 0)
				return;
			emit materialChanged(m_objectId, checked);
		});
}

void MeshProperties::Retranslate()
{
	auto& i18n = I18n::instance();
	m_group->setTitle(i18n.translate("Mesh"));
	m_assetLabel->setText(i18n.translate("Asset"));
	m_pathPrefix->setText(i18n.translate("Path"));
	m_flagsLabel->setText(i18n.translate("Flags"));
	m_shadowChk->setText(i18n.translate("Cast Shadow"));
	m_materialChk->setText(i18n.translate("Use Material"));
}

void MeshProperties::setObjectId(int id)
{
	if (m_objectId != id)
	{
		m_objectId = id;
		m_cachedPath.clear();
		m_cachedShadow   = -1;
		m_cachedMaterial = -1;
	}
}

void MeshProperties::setMeshPath(const std::string& path)
{
	if (m_cachedPath == path)
		return;
	m_cachedPath = path;
	m_pathLabel->setText(QString::fromStdString(path));
}

void MeshProperties::setShadowEnabled(bool enabled)
{
	int val = enabled ? 1 : 0;
	if (m_cachedShadow == val)
		return;
	m_cachedShadow = val;
	m_shadowChk->blockSignals(true);
	m_shadowChk->setChecked(enabled);
	m_shadowChk->blockSignals(false);
}

void MeshProperties::setMaterialEnabled(bool enabled)
{
	int val = enabled ? 1 : 0;
	if (m_cachedMaterial == val)
		return;
	m_cachedMaterial = val;
	m_materialChk->blockSignals(true);
	m_materialChk->setChecked(enabled);
	m_materialChk->blockSignals(false);
}

} // namespace neurus
