#include "presets/CameraProperties.h"
#include "items/Vec3Spin.h"
#include "items/ScalarSlider.h"
#include "ui/utils/I18n.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace neurus {

CameraProperties::CameraProperties(QWidget* parent)
    : QWidget(parent)
{
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(8);

	// --- Camera group box ---
	m_group = new QGroupBox(QStringLiteral("Camera"), this);
	auto* camLayout = new QVBoxLayout(m_group);
	camLayout->setSpacing(8);
	layout->addWidget(m_group);

	// --- Look-At Target row ---
	auto* tarRow = new QHBoxLayout();
	m_tarLabel = new QLabel(QStringLiteral("Look-At Target"));
	tarRow->addWidget(m_tarLabel);

	m_tarSpin = new Vec3Spin(-100000.0, 100000.0, 0.01, 2, QString(), this);
	tarRow->addWidget(m_tarSpin, 1);
	camLayout->addLayout(tarRow);

	QObject::connect(m_tarSpin, &Vec3Spin::valueChanged, this,
		[this](double x, double y, double z) {
			if (m_objectId < 0)
			{
				return;
			}
			emit targetChanged(m_objectId, static_cast<float>(x),
			                   static_cast<float>(y), static_cast<float>(z));
		});

	// --- FOV row ---
	auto* fovRow = new QHBoxLayout();
	m_fovLabel = new QLabel(QStringLiteral("FOV (\u00B0)"));
	fovRow->addWidget(m_fovLabel);

	m_fovSlider = new ScalarSlider(1.0, 179.0, 178, 60.0, this);
	fovRow->addWidget(m_fovSlider, 1);
	camLayout->addLayout(fovRow);

	QObject::connect(m_fovSlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0)
			{
				return;
			}
			emit fovChanged(m_objectId, static_cast<float>(m_fovSlider->value()));
		});

	// --- Active Camera row ---
	// No label beside it: the checkbox carries its own text, and unlike the two
	// rows above this is not a camera property at all but the Scene's choice of
	// view camera, so it reads as a statement rather than a value to edit.
	m_activeChk = new QCheckBox(QStringLiteral("Active Camera"), this);
	camLayout->addWidget(m_activeChk);

	QObject::connect(m_activeChk, &QCheckBox::toggled, this,
		[this](bool checked) {
			if (m_objectId < 0)
			{
				return;
			}
			emit activeCameraChanged(m_objectId, checked);
		});

	layout->addStretch();

	// Apply the active language (labels were built in English).
	Retranslate();
}

void CameraProperties::Retranslate()
{
	auto& i18n = I18n::instance();
	m_group->setTitle(i18n.translate("Camera"));
	m_tarLabel->setText(i18n.translate("Look-At Target"));
	m_fovLabel->setText(i18n.translate("FOV (°)"));
	m_activeChk->setText(i18n.translate("Active Camera"));
}

void CameraProperties::setObjectId(int id)
{
	if (m_objectId != id)
	{
		m_objectId = id;
		// Reset caches to sentinel values so the next setTarget() / setFov()
		// always applies — forces a full refresh for the new object.
		m_cachedTarget = glm::vec3(FLT_MAX, FLT_MAX, FLT_MAX);
		m_cachedFov    = -1.0f;
		m_cachedActive = -1;
	}
}

void CameraProperties::setTarget(const glm::vec3& target)
{
	if (m_cachedTarget == target)
	{
		return;
	}
	m_cachedTarget = target;
	m_tarSpin->setValue(static_cast<double>(target.x),
	                    static_cast<double>(target.y),
	                    static_cast<double>(target.z));
}

void CameraProperties::setFov(float fov)
{
	if (m_cachedFov == fov)
	{
		return;
	}
	m_cachedFov = fov;
	m_fovSlider->setValue(static_cast<double>(fov));
}

void CameraProperties::setActive(bool active)
{
	const int val = active ? 1 : 0;
	if (m_cachedActive == val)
	{
		return;
	}
	m_cachedActive = val;
	// Blocked: setChecked() would otherwise emit toggled() and send the state we
	// were just handed straight back as a user edit — one that would record an
	// undo entry for a change nobody made.
	m_activeChk->blockSignals(true);
	m_activeChk->setChecked(active);
	m_activeChk->blockSignals(false);
}

} // namespace neurus
