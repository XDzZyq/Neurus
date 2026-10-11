/**
 * @file RenderConfigPanel.h
 * @brief Render Config dock panel exposing live-adjustable rendering settings.
 *
 * Provides Qt widgets (sliders, combo boxes, checkboxes) for all fields in
 * RenderConfig plus additional runtime toggles (shadow PCF mode, IBL toggle,
 * exposure). Changes emit configValueChanged() so the new settings can be
 * applied in real time.
 */

#pragma once

#include "UIPanel.h"
#include "editor/events/ConfigEvents.h"

// Forward declarations
class QCheckBox;
class QComboBox;
class QGroupBox;
class QSpinBox;

namespace neurus
{

class ScalarSlider;

class RenderConfigPanel : public UIPanel
{
	Q_OBJECT

public:
	static constexpr PanelType kType = PanelType::RenderConfig;

	explicit RenderConfigPanel(QWidget* parent = nullptr);
	~RenderConfigPanel() override = default;

	RenderConfigPanel(const RenderConfigPanel&) = delete;
	RenderConfigPanel& operator=(const RenderConfigPanel&) = delete;

	/**
	 * @brief Refreshes the panel from a UIContext snapshot.
	 *
	 * Populates all controls with the current values without emitting signals.
	 *
	 * @param ctx Read-only UI context.
	 */
	void Refresh(const UIContext& ctx) override;

	/**
	 * @brief Re-applies all section titles / row labels / combo items in the
	 *        active language.
	 */
	void Retranslate() override;

	/**
	 * @brief Builds and returns a RenderConfig from current UI values.
	 * @return RenderConfig populated from all controls.
	 */
	RenderConfig Save() const;

signals:
	/** @brief Emitted whenever any control value changes, carrying the new config. */
	void configValueChanged(const RenderConfigChangedEvent& e);

	/** @brief Emitted when a bounded slider edit begins (slider press).
	 *  Bounds one undo entry for continuous slider drags. */
	void editBegin(const ConfigEditBegin& e);

	/** @brief Emitted when a bounded slider edit ends (slider release).
	 *  Bounds one undo entry for continuous slider drags. */
	void editEnd(const ConfigEditEnd& e);

private:
	// --- Section builders ---
	void BuildShadowsSection();
	void BuildAmbientOcclusionSection();
	void BuildLightingSection();
	void BuildPostProcessingSection();
	void BuildPipelineSection();

	// --- Signal forwarders ---
	void ConnectAllSignals();

	// --- Shadows ---
	QGroupBox*  m_shadowsGroup      = nullptr;
	QComboBox*  m_shadowAlgCombo    = nullptr;
	QComboBox*  m_shadowPCFCombo    = nullptr;  ///< PCF filter mode (Hard/Soft16/Soft64)
	ScalarSlider* m_shadowBiasSlider = nullptr;
	QComboBox*  m_samplingModeCombo  = nullptr;  ///< Temporal sampling mode (Fixed EMA / Moving Avg)

	// --- Ambient Occlusion ---
	QGroupBox*  m_aoGroup           = nullptr;
	QComboBox*  m_aoAlgCombo        = nullptr;
	QSpinBox*   m_aoKernelSpin      = nullptr;
	ScalarSlider* m_aoRadiusSlider   = nullptr;

	// --- Lighting ---
	QGroupBox*  m_lightingGroup     = nullptr;
	QCheckBox*  m_iblCheckBox       = nullptr;
	QCheckBox*  m_transCheckBox     = nullptr;
	ScalarSlider* m_exposureSlider   = nullptr;

	// --- Post-Processing ---
	QGroupBox*  m_postGroup         = nullptr;
	QComboBox*  m_aaCombo           = nullptr;
	ScalarSlider* m_gammaSlider      = nullptr;
	ScalarSlider* m_fxaaSubpixSlider  = nullptr;
	ScalarSlider* m_fxaaEdgeSlider    = nullptr;
	ScalarSlider* m_fxaaEdgeMinSlider = nullptr;

	// --- Pipeline ---
	QGroupBox*  m_pipelineGroup     = nullptr;
	QComboBox*  m_pipelineCombo     = nullptr;
	QComboBox*  m_ssrCombo          = nullptr;
	QSpinBox*   m_samplesPerFrameSpin = nullptr;
};

} // namespace neurus
