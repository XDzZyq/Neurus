#include "presets/LightProperties.h"
#include "items/ScalarSlider.h"
#include "ui/utils/I18n.h"

#include <QCheckBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <cmath>

namespace neurus {

namespace {

// --- Degree ↔ cosine conversion helpers (spot-cone UI convention) ---
// UI presents user-friendly half-angle in degrees; scene stores cosines.
constexpr float kPi = 3.14159265358979323846f;

inline float DegToCosine(double deg)
{
	return std::cos(static_cast<float>(deg) * kPi / 180.0f);
}

inline double CosineToDeg(float cosine)
{
	return static_cast<double>(std::acos(cosine) * 180.0f / kPi);
}

} // anonymous namespace

// =========================================================================
// Constructor
// =========================================================================

LightProperties::LightProperties(QWidget* parent)
	: QWidget(parent)
{
	auto* outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	m_group = new QGroupBox("Light", this);
	outerLayout->addWidget(m_group);

	auto* groupLayout = new QVBoxLayout(m_group);
	groupLayout->setSpacing(6);

	// --- Row: Type label + type name (readonly, bold) ---
	auto* typeRow = new QHBoxLayout();
	m_typeCaption = new QLabel("Type");
	typeRow->addWidget(m_typeCaption);

	m_typeLabel = new QLabel("Unknown");
	QFont typeFont = m_typeLabel->font();
	typeFont.setBold(true);
	m_typeLabel->setFont(typeFont);
	typeRow->addWidget(m_typeLabel, 1);
	groupLayout->addLayout(typeRow);

	// --- Row: Power label + ScalarSlider ---
	auto* powerRow = new QHBoxLayout();
	m_powerLabel = new QLabel("Power");
	powerRow->addWidget(m_powerLabel);
	m_powerSlider = new ScalarSlider(0.0, 100.0, 1000, 10.0);
	powerRow->addWidget(m_powerSlider, 1);
	groupLayout->addLayout(powerRow);

	// --- Row: Radius label + ScalarSlider ---
	auto* radiusRow = new QHBoxLayout();
	m_radiusLabel = new QLabel("Radius");
	radiusRow->addWidget(m_radiusLabel);
	m_radiusSlider = new ScalarSlider(0.0, 10.0, 1000, 0.05);
	radiusRow->addWidget(m_radiusSlider, 1);
	groupLayout->addLayout(radiusRow);

	// --- Row: Cast Shadow checkbox ---
	m_shadowChk = new QCheckBox("Cast Shadow");
	groupLayout->addWidget(m_shadowChk);

	// --- Row: Inner cone half-angle (degrees) — SPOTLIGHT only ---
	m_innerConeRow = new QWidget();
	{
		auto* row = new QHBoxLayout(m_innerConeRow);
		row->setContentsMargins(0, 0, 0, 0);
		m_innerConeLabel = new QLabel("Inner Cone");
		row->addWidget(m_innerConeLabel);
		m_innerConeSlider = new ScalarSlider(0.5, 89.0, 1000, 25.0);
		row->addWidget(m_innerConeSlider, 1);
	}
	groupLayout->addWidget(m_innerConeRow);

	// --- Row: Outer cone half-angle (degrees) — SPOTLIGHT only ---
	m_outerConeRow = new QWidget();
	{
		auto* row = new QHBoxLayout(m_outerConeRow);
		row->setContentsMargins(0, 0, 0, 0);
		m_outerConeLabel = new QLabel("Outer Cone");
		row->addWidget(m_outerConeLabel);
		m_outerConeSlider = new ScalarSlider(0.5, 89.0, 1000, 35.0);
		row->addWidget(m_outerConeSlider, 1);
	}
	groupLayout->addWidget(m_outerConeRow);

	m_innerConeRow->setVisible(false);
	m_outerConeRow->setVisible(false);

	outerLayout->addStretch();

	// Apply the active language (labels were built in English).
	Retranslate();

	// --- Signal wiring ---
	QObject::connect(m_powerSlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0) return;
			emit powerChanged(m_objectId, static_cast<float>(m_powerSlider->value()));
		});

	QObject::connect(m_radiusSlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0) return;
			emit radiusChanged(m_objectId, static_cast<float>(m_radiusSlider->value()));
		});

	QObject::connect(m_shadowChk, &QCheckBox::toggled, this,
		[this](bool checked) {
			if (m_objectId < 0) return;
			emit shadowChanged(m_objectId, checked);
		});

	QObject::connect(m_innerConeSlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0) return;
			emit cutoffChanged(m_objectId, DegToCosine(m_innerConeSlider->value()));
		});

	QObject::connect(m_outerConeSlider, &ScalarSlider::valueChanged, this,
		[this]() {
			if (m_objectId < 0) return;
			emit outerCutoffChanged(m_objectId, DegToCosine(m_outerConeSlider->value()));
		});
}

// =========================================================================
// Setters (all dirty-checked)
// =========================================================================

void LightProperties::Retranslate()
{
	auto& i18n = I18n::instance();
	m_group->setTitle(i18n.translate("Light"));
	m_typeCaption->setText(i18n.translate("Type"));
	// Re-translate the light-type name from its raw key (falls back to English).
	// An empty cache means no light is bound yet — at ctor time, or between
	// setObjectId() and the next setLightType() — and translate("") returns "",
	// which would blank the row instead of showing the placeholder.
	m_typeLabel->setText(m_cachedType.empty()
	                     ? i18n.translate("Unknown")
	                     : i18n.translate(m_cachedType.c_str()));
	m_powerLabel->setText(i18n.translate("Power"));
	m_radiusLabel->setText(i18n.translate("Radius"));
	m_shadowChk->setText(i18n.translate("Cast Shadow"));
	m_innerConeLabel->setText(i18n.translate("Inner Cone"));
	m_outerConeLabel->setText(i18n.translate("Outer Cone"));
}

void LightProperties::setObjectId(int id)
{
	if (m_objectId != id)
	{
		m_objectId = id;
		m_cachedType.clear();
		m_cachedPower       = -1.0f;
		m_cachedRadius      = -1.0f;
		m_cachedShadow      = -1;
		m_cachedCutoff      = -2.0f;
		m_cachedOuterCutoff = -2.0f;
	}
}

void LightProperties::setLightType(const std::string& typeName)
{
	if (m_cachedType == typeName) return;
	m_cachedType = typeName;
	m_typeLabel->setText(I18n::instance().translate(typeName.c_str()));
}

void LightProperties::setPower(float power)
{
	if (m_cachedPower == power) return;
	m_cachedPower = power;
	m_powerSlider->setValue(static_cast<double>(power));
}

void LightProperties::setRadius(float radius)
{
	if (m_cachedRadius == radius) return;
	m_cachedRadius = radius;
	m_radiusSlider->setValue(static_cast<double>(radius));
}

void LightProperties::setShadowEnabled(bool enabled)
{
	int val = enabled ? 1 : 0;
	if (m_cachedShadow == val) return;
	m_cachedShadow = val;
	m_shadowChk->blockSignals(true);
	m_shadowChk->setChecked(enabled);
	m_shadowChk->blockSignals(false);
}

void LightProperties::setCutoff(float cosine)
{
	if (m_cachedCutoff == cosine) return;
	m_cachedCutoff = cosine;
	m_innerConeSlider->setValue(CosineToDeg(cosine));
}

void LightProperties::setOuterCutoff(float cosine)
{
	if (m_cachedOuterCutoff == cosine) return;
	m_cachedOuterCutoff = cosine;
	m_outerConeSlider->setValue(CosineToDeg(cosine));
}

void LightProperties::setSpotConeVisible(bool visible)
{
	m_innerConeRow->setVisible(visible);
	m_outerConeRow->setVisible(visible);
}

} // namespace neurus
