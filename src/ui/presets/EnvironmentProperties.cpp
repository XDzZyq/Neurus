#include "presets/EnvironmentProperties.h"
#include "items/ScalarSlider.h"
#include "ui/utils/I18n.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QFont>

namespace neurus {

EnvironmentProperties::EnvironmentProperties(QWidget* parent)
    : QWidget(parent)
{
	auto* outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	// --- Environment Group Box ---
	m_group = new QGroupBox(QStringLiteral("Environment"), this);
	outerLayout->addWidget(m_group);

	auto* groupLayout = new QVBoxLayout(m_group);
	groupLayout->setSpacing(6);

	// --- Section: Map ---
	m_mapLabel = new QLabel(QStringLiteral("Map"));
	QFont headerFont = m_mapLabel->font();
	headerFont.setBold(true);
	m_mapLabel->setFont(headerFont);
	groupLayout->addWidget(m_mapLabel);

	auto* pathRow = new QHBoxLayout();
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
	groupLayout->addLayout(pathRow);

	// --- Section: IBL ---
	m_iblLabel = new QLabel(QStringLiteral("IBL"));
	m_iblLabel->setFont(headerFont);
	groupLayout->addWidget(m_iblLabel);

	auto* intensityRow = new QHBoxLayout();
	m_intensityLabel = new QLabel(QStringLiteral("Intensity"));
	intensityRow->addWidget(m_intensityLabel);
	m_intensitySlider = new ScalarSlider(0.0, 10.0, 1000, 1.0, this);
	intensityRow->addWidget(m_intensitySlider, 1);
	groupLayout->addLayout(intensityRow);

	auto* rotationRow = new QHBoxLayout();
	m_rotationLabel = new QLabel(QStringLiteral("Rotation (\u00B0)"));
	rotationRow->addWidget(m_rotationLabel);
	m_rotationSlider = new ScalarSlider(0.0, 360.0, 360, 0.0, this);
	rotationRow->addWidget(m_rotationSlider, 1);
	groupLayout->addLayout(rotationRow);

	outerLayout->addStretch();

	// Apply the active language (labels were built in English).
	Retranslate();

	// --- Signal wiring ---
	QObject::connect(m_intensitySlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0)
			{
				return;
			}
			emit intensityChanged(m_objectId, static_cast<float>(m_intensitySlider->value()));
		});

	QObject::connect(m_rotationSlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0)
			{
				return;
			}
			emit rotationChanged(m_objectId, static_cast<float>(m_rotationSlider->value()));
		});
}

void EnvironmentProperties::Retranslate()
{
	auto& i18n = I18n::instance();
	m_group->setTitle(i18n.translate("Environment"));
	m_mapLabel->setText(i18n.translate("Map"));
	m_pathPrefix->setText(i18n.translate("Path"));
	m_iblLabel->setText(i18n.translate("IBL"));
	m_intensityLabel->setText(i18n.translate("Intensity"));
	m_rotationLabel->setText(i18n.translate("Rotation (°)"));
}

void EnvironmentProperties::setObjectId(int id)
{
	if (m_objectId != id)
	{
		m_objectId = id;
		// Reset caches to sentinel values so the next setIntensity() /
		// setRotation() / setEquirectPath() always applies — forces a
		// full refresh for the new object.
		m_cachedIntensity = -1.0f;
		m_cachedRotation  = -999.0f;
		m_cachedPath.clear();
	}
}

void EnvironmentProperties::setIntensity(float intensity)
{
	if (m_cachedIntensity == intensity)
	{
		return;
	}
	m_cachedIntensity = intensity;
	m_intensitySlider->setValue(static_cast<double>(intensity));
}

void EnvironmentProperties::setRotation(float rotation)
{
	if (m_cachedRotation == rotation)
	{
		return;
	}
	m_cachedRotation = rotation;
	m_rotationSlider->setValue(static_cast<double>(rotation));
}

void EnvironmentProperties::setEquirectPath(const std::string& path)
{
	if (m_cachedPath == path)
	{
		return;
	}
	m_cachedPath = path;
	m_pathLabel->setText(QString::fromStdString(path));
}

} // namespace neurus
