/**
 * @file RenderConfigPanel.cpp
 * @brief RenderConfigPanel implementation — hard-coded UI layout for render settings.
 */

#include "panels/RenderConfigPanel.h"

#include "items/ScalarSlider.h"
#include "render/RenderConfig.h"
#include "UIContext.h"
#include "ui/utils/I18n.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace neurus
{

namespace
{

/** @brief Sets the form-row label of @p field from an i18n key. */
void SetFormRowLabel(QFormLayout* form, QWidget* field, const char* key)
{
	// addRow(QString, widget) creates a QLabel for the row label.
	if (QLabel* label = static_cast<QLabel*>(form->labelForField(field)))
		label->setText(I18n::instance().translate(key));
}

/** @brief Re-texts a combo's items from i18n keys (order/indices preserved). */
void SetComboItems(QComboBox* combo, std::initializer_list<const char*> keys)
{
	int index = 0;
	for (const char* key : keys)
		combo->setItemText(index++, I18n::instance().translate(key));
}

} // namespace

// =========================================================================
// Constructor / Destructor
// =========================================================================

RenderConfigPanel::RenderConfigPanel(QWidget* parent)
	: UIPanel(PanelType::RenderConfig, nullptr, parent)
{
	// --- Main layout ---
	auto* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(0, 0, 0, 0);

	// --- Scroll area ---
	auto* scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	mainLayout->addWidget(scrollArea);

	// --- Form container ---
	auto* formContainer = new QWidget();
	auto* formLayout = new QVBoxLayout(formContainer);
	formLayout->setContentsMargins(6, 6, 6, 6);
	formLayout->setSpacing(6);

	// --- Build sections ---
	BuildShadowsSection();
	formLayout->addWidget(m_shadowsGroup);

	BuildAmbientOcclusionSection();
	formLayout->addWidget(m_aoGroup);

	BuildLightingSection();
	formLayout->addWidget(m_lightingGroup);

	BuildPostProcessingSection();
	formLayout->addWidget(m_postGroup);

	BuildPipelineSection();
	formLayout->addWidget(m_pipelineGroup);

	// --- Bottom stretch ---
	formLayout->addStretch();

	scrollArea->setWidget(formContainer);

	// --- Wire all signals ---
	ConnectAllSignals();

	// Apply the active language (menus/docks were built with English literals).
	Retranslate();
}

// =========================================================================
// Retranslate — re-apply every section title / row label / combo item
// =========================================================================

void RenderConfigPanel::Retranslate()
{
	auto& i18n = I18n::instance();
	auto* shadowsForm  = static_cast<QFormLayout*>(m_shadowsGroup->layout());
	auto* aoForm       = static_cast<QFormLayout*>(m_aoGroup->layout());
	auto* lightingForm = static_cast<QFormLayout*>(m_lightingGroup->layout());
	auto* postForm     = static_cast<QFormLayout*>(m_postGroup->layout());
	auto* pipelineForm = static_cast<QFormLayout*>(m_pipelineGroup->layout());

	// --- Shadows ---
	m_shadowsGroup->setTitle(i18n.translate("Shadows"));
	SetFormRowLabel(shadowsForm, m_shadowAlgCombo, N_("Algorithm"));
	SetComboItems(m_shadowAlgCombo,
	              {N_("None"), N_("Shadow Mapping"), N_("SDF Soft Shadow"), N_("VSSM")});
	SetFormRowLabel(shadowsForm, m_shadowPCFCombo, N_("PCF Filter"));
	SetComboItems(m_shadowPCFCombo, {N_("Hard"), N_("Soft PCF 16"), N_("Soft PCF 64")});
	SetFormRowLabel(shadowsForm, m_shadowBiasSlider, N_("Bias"));
	SetFormRowLabel(shadowsForm, m_samplingModeCombo, N_("Sampling Mode"));
	SetComboItems(m_samplingModeCombo, {N_("Fixed EMA (1/8)"), N_("Moving Average")});

	// --- Ambient Occlusion ---
	m_aoGroup->setTitle(i18n.translate("Ambient Occlusion"));
	SetFormRowLabel(aoForm, m_aoAlgCombo, N_("Algorithm"));
	SetComboItems(m_aoAlgCombo, {N_("None"), N_("SSAO")});
	SetFormRowLabel(aoForm, m_aoKernelSpin, N_("Kernel Size"));
	SetFormRowLabel(aoForm, m_aoRadiusSlider, N_("Radius"));

	// --- Lighting ---
	m_lightingGroup->setTitle(i18n.translate("Lighting"));
	m_iblCheckBox->setText(i18n.translate("Enable IBL"));
	m_transCheckBox->setText(i18n.translate("Transparent Background"));
	SetFormRowLabel(lightingForm, m_exposureSlider, N_("Exposure"));

	// --- Post-Processing ---
	m_postGroup->setTitle(i18n.translate("Post-Processing"));
	SetFormRowLabel(postForm, m_aaCombo, N_("Anti-Aliasing"));
	SetComboItems(m_aaCombo, {N_("None"), N_("MSAA"), N_("FXAA")});
	SetFormRowLabel(postForm, m_gammaSlider, N_("Gamma"));
	SetFormRowLabel(postForm, m_fxaaSubpixSlider, N_("FXAA Subpix"));
	SetFormRowLabel(postForm, m_fxaaEdgeSlider, N_("FXAA Edge"));
	SetFormRowLabel(postForm, m_fxaaEdgeMinSlider, N_("FXAA Edge Min"));

	// --- Pipeline ---
	m_pipelineGroup->setTitle(i18n.translate("Pipeline"));
	SetFormRowLabel(pipelineForm, m_pipelineCombo, N_("Pipeline"));
	SetComboItems(m_pipelineCombo, {N_("Forward"), N_("Deferred")});
	SetFormRowLabel(pipelineForm, m_ssrCombo, N_("SSR"));
	SetComboItems(m_ssrCombo,
	              {N_("None"), N_("Ray Marching"), N_("SDF Ray Marching"), N_("SDF Resolved")});
	SetFormRowLabel(pipelineForm, m_samplesPerFrameSpin, N_("Samples / Frame"));
}

// =========================================================================
// Section builders
// =========================================================================

// --- Helper: create a labelled combo box row in a form layout ---
static QComboBox* addComboRow(QFormLayout* form, const QString& label, const QStringList& items)
{
	auto* combo = new QComboBox();
	combo->addItems(items);
	form->addRow(label, combo);
	return combo;
}

// --- Helper: create a QSpinBox row ---
static QSpinBox* addSpinRow(QFormLayout* form, const QString& label, int min, int max, int initial)
{
	auto* spin = new QSpinBox();
	spin->setRange(min, max);
	spin->setValue(initial);
	form->addRow(label, spin);
	return spin;
}

// =========================================================================
// Shadows
// =========================================================================

void RenderConfigPanel::BuildShadowsSection()
{
	m_shadowsGroup = new QGroupBox("Shadows");
	m_shadowsGroup->setCheckable(true);
	m_shadowsGroup->setChecked(true);

	auto* form = new QFormLayout(m_shadowsGroup);
	form->setContentsMargins(8, 12, 8, 8);
	form->setSpacing(4);

	m_shadowAlgCombo = addComboRow(form, "Algorithm",
		{"None", "Shadow Mapping", "SDF Soft Shadow", "VSSM"});

	m_shadowPCFCombo = addComboRow(form, "PCF Filter",
		{"Hard", "Soft PCF 16", "Soft PCF 64"});

	m_shadowBiasSlider = new ScalarSlider(0.0, 0.01, 1000, 0.0005, this);
	form->addRow("Bias", m_shadowBiasSlider);

	m_samplingModeCombo = addComboRow(form, "Sampling Mode",
		{"Fixed EMA (1/8)", "Moving Average"});
}

// =========================================================================
// Ambient Occlusion
// =========================================================================

void RenderConfigPanel::BuildAmbientOcclusionSection()
{
	m_aoGroup = new QGroupBox("Ambient Occlusion");
	m_aoGroup->setCheckable(true);
	m_aoGroup->setChecked(true);

	auto* form = new QFormLayout(m_aoGroup);
	form->setContentsMargins(8, 12, 8, 8);
	form->setSpacing(4);

	m_aoAlgCombo = addComboRow(form, "Algorithm", {"None", "SSAO"});
	m_aoAlgCombo->setCurrentIndex(1);  // Default: SSAO

	m_aoKernelSpin = addSpinRow(form, "Kernel Size", 1, 64, 16);

	m_aoRadiusSlider = new ScalarSlider(0.0, 5.0, 500, 0.5, this);
	form->addRow("Radius", m_aoRadiusSlider);
}

// =========================================================================
// Lighting
// =========================================================================

void RenderConfigPanel::BuildLightingSection()
{
	m_lightingGroup = new QGroupBox("Lighting");
	m_lightingGroup->setCheckable(false);  // Always enabled

	auto* form = new QFormLayout(m_lightingGroup);
	form->setContentsMargins(8, 12, 8, 8);
	form->setSpacing(4);

	m_iblCheckBox = new QCheckBox("Enable IBL");
	m_iblCheckBox->setChecked(true);
	form->addRow(m_iblCheckBox);

	m_transCheckBox = new QCheckBox("Transparent Background");
	m_transCheckBox->setChecked(false);
	form->addRow(m_transCheckBox);

	m_exposureSlider = new ScalarSlider(0.0, 5.0, 100, 1.0, this);
	form->addRow("Exposure", m_exposureSlider);
}

// =========================================================================
// Post-Processing
// =========================================================================

void RenderConfigPanel::BuildPostProcessingSection()
{
	m_postGroup = new QGroupBox("Post-Processing");
	m_postGroup->setCheckable(false);

	auto* form = new QFormLayout(m_postGroup);
	form->setContentsMargins(8, 12, 8, 8);
	form->setSpacing(4);

	m_aaCombo = addComboRow(form, "Anti-Aliasing", {"None", "MSAA", "FXAA"});

	m_gammaSlider = new ScalarSlider(1.0, 3.0, 200, 1.0, this);
	form->addRow("Gamma", m_gammaSlider);

	// --- FXAA parameters ---
	m_fxaaSubpixSlider = new ScalarSlider(0.0, 1.0, 100, 0.75, this);
	form->addRow("FXAA Subpix", m_fxaaSubpixSlider);

	m_fxaaEdgeSlider = new ScalarSlider(0.063, 0.333, 270, 0.166, this);
	form->addRow("FXAA Edge", m_fxaaEdgeSlider);

	m_fxaaEdgeMinSlider = new ScalarSlider(0.0312, 0.0833, 52, 0.0833, this);
	form->addRow("FXAA Edge Min", m_fxaaEdgeMinSlider);
}

// =========================================================================
// Pipeline
// =========================================================================

void RenderConfigPanel::BuildPipelineSection()
{
	m_pipelineGroup = new QGroupBox("Pipeline");
	m_pipelineGroup->setCheckable(false);

	auto* form = new QFormLayout(m_pipelineGroup);
	form->setContentsMargins(8, 12, 8, 8);
	form->setSpacing(4);

	m_pipelineCombo = addComboRow(form, "Pipeline",
		{"Forward", "Deferred"});
	m_pipelineCombo->setCurrentIndex(1);  // Default: Deferred

	m_ssrCombo = addComboRow(form, "SSR",
		{"None", "Ray Marching", "SDF Ray Marching", "SDF Resolved"});

	m_samplesPerFrameSpin = addSpinRow(form, "Samples / Frame", 1, 1024, 128);
}

// =========================================================================
// Signal wiring
// =========================================================================

void RenderConfigPanel::ConnectAllSignals()
{
	auto emitCfg = [this]() { 
		emit configValueChanged(RenderConfigChangedEvent{Save()}); 
		};

	// --- Slider gesture boundaries: press begins one undo entry, release ends it ---
	auto bindGesture = [this](ScalarSlider* s) {
		connect(s, &ScalarSlider::pressed, this,
			[this]() { emit editBegin(ConfigEditBegin{}); });
		connect(s, &ScalarSlider::released, this,
			[this]() { emit editEnd(ConfigEditEnd{}); });
	};

	// --- Shadows ---
	connect(m_shadowsGroup, &QGroupBox::toggled, this, emitCfg);
	connect(m_shadowAlgCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, emitCfg);
	connect(m_shadowPCFCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, emitCfg);
	connect(m_shadowBiasSlider, &ScalarSlider::valueChanged,
		this, emitCfg);
	bindGesture(m_shadowBiasSlider);
	connect(m_samplingModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, emitCfg);

	// --- Ambient Occlusion ---
	connect(m_aoGroup, &QGroupBox::toggled, this, emitCfg);
	connect(m_aoAlgCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, emitCfg);
	connect(m_aoKernelSpin, QOverload<int>::of(&QSpinBox::valueChanged),
		this, emitCfg);
	connect(m_aoRadiusSlider, &ScalarSlider::valueChanged,
		this, emitCfg);
	bindGesture(m_aoRadiusSlider);

	// --- Lighting ---
	connect(m_iblCheckBox, &QCheckBox::toggled, this, emitCfg);
	connect(m_transCheckBox, &QCheckBox::toggled, this, emitCfg);
	connect(m_exposureSlider, &ScalarSlider::valueChanged,
		this, emitCfg);
	bindGesture(m_exposureSlider);

	// --- Post-Processing ---
	connect(m_aaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, emitCfg);
	connect(m_gammaSlider, &ScalarSlider::valueChanged,
		this, emitCfg);
	bindGesture(m_gammaSlider);
	connect(m_fxaaSubpixSlider, &ScalarSlider::valueChanged,
		this, emitCfg);
	bindGesture(m_fxaaSubpixSlider);
	connect(m_fxaaEdgeSlider, &ScalarSlider::valueChanged,
		this, emitCfg);
	bindGesture(m_fxaaEdgeSlider);
	connect(m_fxaaEdgeMinSlider, &ScalarSlider::valueChanged,
		this, emitCfg);
	bindGesture(m_fxaaEdgeMinSlider);

	// --- Pipeline ---
	connect(m_pipelineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, emitCfg);
	connect(m_ssrCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, emitCfg);
	connect(m_samplesPerFrameSpin, QOverload<int>::of(&QSpinBox::valueChanged),
		this, emitCfg);
}

// =========================================================================
// RenderConfig ↔ UI sync
// =========================================================================

void RenderConfigPanel::Refresh(const UIContext& ctx)
{
	const auto* config = static_cast<const RenderConfig*>(ctx.editor.config);
	if (!config) return;

	// --- Shadows ---
	{
		QSignalBlocker b1(m_shadowAlgCombo);
		QSignalBlocker b2(m_shadowsGroup);
		switch (config->r_shadow)
		{
		case ShadowAlg::None:           m_shadowAlgCombo->setCurrentIndex(0); break;
		case ShadowAlg::ShadowMapping:  m_shadowAlgCombo->setCurrentIndex(1); break;
		case ShadowAlg::SDFSoftShadow:  m_shadowAlgCombo->setCurrentIndex(2); break;
		case ShadowAlg::VSSM:           m_shadowAlgCombo->setCurrentIndex(3); break;
		}
		m_shadowsGroup->setChecked(config->r_shadow != ShadowAlg::None);

		m_shadowBiasSlider->setValue(config->r_shadow_bias);

		QSignalBlocker b3(m_samplingModeCombo);
		m_samplingModeCombo->setCurrentIndex(
			static_cast<int>(config->r_sampling_mode));
	}

	// --- Ambient Occlusion ---
	{
		QSignalBlocker b1(m_aoAlgCombo);
		QSignalBlocker b2(m_aoGroup);
		switch (config->r_ao)
		{
		case AOAlg::None: m_aoAlgCombo->setCurrentIndex(0); break;
		case AOAlg::SSAO: m_aoAlgCombo->setCurrentIndex(1); break;
		}
		m_aoGroup->setChecked(config->r_ao != AOAlg::None);

		QSignalBlocker b3(m_aoKernelSpin);
		m_aoKernelSpin->setValue(config->r_ao_ksize);

		m_aoRadiusSlider->setValue(config->r_ao_radius);
	}

	// --- Post-Processing ---
	{
		QSignalBlocker b1(m_aaCombo);
		switch (config->r_aa)
		{
		case AAAlg::None: m_aaCombo->setCurrentIndex(0); break;
		case AAAlg::MSAA: m_aaCombo->setCurrentIndex(1); break;
		case AAAlg::FXAA: m_aaCombo->setCurrentIndex(2); break;
		}

		m_gammaSlider->setValue(config->r_gamma);

		m_fxaaSubpixSlider->setValue(config->r_fxaa_subpix);
		m_fxaaEdgeSlider->setValue(config->r_fxaa_edge_threshold);
		m_fxaaEdgeMinSlider->setValue(config->r_fxaa_edge_threshold_min);

		QSignalBlocker bt(m_transCheckBox);
		m_transCheckBox->setChecked(config->r_transparent);
	}

	// --- Pipeline ---
	{
		QSignalBlocker b1(m_pipelineCombo);
		switch (config->r_pipeline)
		{
		case RenderPipeLine::Forward:  m_pipelineCombo->setCurrentIndex(0); break;
		case RenderPipeLine::Deferred: m_pipelineCombo->setCurrentIndex(1); break;
		case RenderPipeLine::Custom0:  m_pipelineCombo->setCurrentIndex(0); break;  // fallback
		}

		QSignalBlocker b2(m_ssrCombo);
		switch (config->r_ssr)
		{
		case SSRAlg::None:                    m_ssrCombo->setCurrentIndex(0); break;
		case SSRAlg::RayMarching:             m_ssrCombo->setCurrentIndex(1); break;
		case SSRAlg::SDFRayMarching:          m_ssrCombo->setCurrentIndex(2); break;
		case SSRAlg::SDFResolvedRayMarching:  m_ssrCombo->setCurrentIndex(3); break;
		}

		QSignalBlocker b3(m_samplesPerFrameSpin);
		m_samplesPerFrameSpin->setValue(config->r_sample_pf);
	}
}

RenderConfig RenderConfigPanel::Save() const
{
	RenderConfig config{};

	// --- Shadows ---
	switch (m_shadowAlgCombo->currentIndex())
	{
	case 0: config.r_shadow = ShadowAlg::None;          break;
	case 1: config.r_shadow = ShadowAlg::ShadowMapping; break;
	case 2: config.r_shadow = ShadowAlg::SDFSoftShadow; break;
	case 3: config.r_shadow = ShadowAlg::VSSM;          break;
	}
	if (!m_shadowsGroup->isChecked())
		config.r_shadow = ShadowAlg::None;

	config.r_shadow_bias = static_cast<float>(m_shadowBiasSlider->value());

	config.r_sampling_mode = static_cast<uint32_t>(m_samplingModeCombo->currentIndex());

	// --- Ambient Occlusion ---
	switch (m_aoAlgCombo->currentIndex())
	{
	case 0: config.r_ao = AOAlg::None; break;
	case 1: config.r_ao = AOAlg::SSAO; break;
	}
	if (!m_aoGroup->isChecked())
		config.r_ao = AOAlg::None;

	config.r_ao_ksize  = m_aoKernelSpin->value();
	config.r_ao_radius = static_cast<float>(m_aoRadiusSlider->value());

	// --- Post-Processing ---
	switch (m_aaCombo->currentIndex())
	{
	case 0: config.r_aa = AAAlg::None; break;
	case 1: config.r_aa = AAAlg::MSAA; break;
	case 2: config.r_aa = AAAlg::FXAA; break;
	}
	config.r_gamma = static_cast<float>(m_gammaSlider->value());
	config.r_transparent = m_transCheckBox->isChecked();

	// --- FXAA ---
	config.r_fxaa_subpix           = static_cast<float>(m_fxaaSubpixSlider->value());
	config.r_fxaa_edge_threshold   = static_cast<float>(m_fxaaEdgeSlider->value());
	config.r_fxaa_edge_threshold_min = static_cast<float>(m_fxaaEdgeMinSlider->value());

	// --- Pipeline ---
	switch (m_pipelineCombo->currentIndex())
	{
	case 0: config.r_pipeline = RenderPipeLine::Forward;  break;
	case 1: config.r_pipeline = RenderPipeLine::Deferred; break;
	}

	switch (m_ssrCombo->currentIndex())
	{
	case 0: config.r_ssr = SSRAlg::None;                    break;
	case 1: config.r_ssr = SSRAlg::RayMarching;             break;
	case 2: config.r_ssr = SSRAlg::SDFRayMarching;          break;
	case 3: config.r_ssr = SSRAlg::SDFResolvedRayMarching;  break;
	}
	config.r_sample_pf = m_samplesPerFrameSpin->value();

	return config;
}

} // namespace neurus
