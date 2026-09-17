#pragma once

#include "UIPanel.h"
#include "editor/events/SceneEvents.h"
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace neurus {

class Vec3Spin;
class CameraProperties;
class MeshProperties;
class LightProperties;
class EnvironmentProperties;

/**
 * @brief Property Panel displaying type/name header, editable transform, and
 *        type-specific property subpanels for the active scene object.
 *
 * Refresh() reads the active object's integer UID from UIContext::scene→
 * selections each frame (comparing the int for lazy header updates), displaying
 * its o_name with type icon and populating the Position / Rotation / Scale
 * Vec3Spin widgets from its Transform3D. Additionally, shows/hides a type-specific
 * subpanel (Camera, Mesh, Light, Environment) based on GOType.
 *
 * Editing any field emits a granular signal routed through ConnectUIEvent to Editor.
 *
 * @note Pure UI layer — no scene mutation. Signals flow through ConnectUIEvent to Editor.
 */
class PropertyPanel : public UIPanel
{
	Q_OBJECT

public:
	static constexpr PanelType kType = PanelType::PropertyPanel;

	explicit PropertyPanel(QWidget* parent = nullptr);
	~PropertyPanel() override = default;

	PropertyPanel(const PropertyPanel&) = delete;
	PropertyPanel& operator=(const PropertyPanel&) = delete;

	/** @brief Reads active object from UIContext, updates header + transform + type subpanel. */
	void Refresh(const UIContext& ctx) override;

	/** @brief Re-applies transform labels / empty-state text in the active language. */
	void Retranslate() override;

signals:
	// --- Transform ---
	void positionChanged(const PositionChanged& e);
	void rotationChanged(const RotationChanged& e);
	void scaleChanged(const ScaleChanged& e);

	// --- Camera ---
	void cameraTargetChanged(const CameraTargetChanged& e);
	void cameraFovChanged(const CameraFovChanged& e);

	// --- Mesh ---
	void meshShadowChanged(const MeshShadowChanged& e);
	void meshMaterialChanged(const MeshMaterialChanged& e);

	// --- Light ---
	void lightPowerChanged(const LightPowerChanged& e);
	void lightRadiusChanged(const LightRadiusChanged& e);
	void lightShadowChanged(const LightShadowChanged& e);
	void lightCutoffChanged(const LightCutoffChanged& e);
	void lightOuterCutoffChanged(const LightOuterCutoffChanged& e);

	// --- Environment ---
	void envIntensityChanged(const EnvironmentIntensityChanged& e);
	void envRotationChanged(const EnvironmentRotationChanged& e);

private:
	void BuildHeader();
	void BuildTransformEditor();
	void BuildTypeSubpanels();

	/** @brief Shows only the subpanel matching the given GOType, hides all others. */
	void ShowTypeSubpanel(int goType);

	/** @brief Enables or disables header + transform group. */
	void SetEnabled(bool enabled);

	// --- State ---
	int m_activeObjectId = 0;  ///< Active object UID (0 = none); compared for lazy updates.

	// --- Header ---
	QWidget* m_headerWidget = nullptr;
	QLabel*  m_iconLabel    = nullptr;
	QLabel*  m_nameLabel    = nullptr;
	QLabel*  m_emptyLabel   = nullptr;

	// --- Transform editor ---
	QGroupBox*  m_transformGroup = nullptr;
	QLabel*     m_posLabel       = nullptr;
	QLabel*     m_rotLabel       = nullptr;
	QLabel*     m_sclLabel       = nullptr;
	QPushButton* m_resetBtn      = nullptr;
	Vec3Spin*   m_posSpin        = nullptr;
	Vec3Spin*   m_rotSpin        = nullptr;
	Vec3Spin*   m_sclSpin        = nullptr;

	// --- Type-specific subpanels (each owns its own QGroupBox) ---
	CameraProperties*      m_cameraProps = nullptr;
	MeshProperties*        m_meshProps   = nullptr;
	LightProperties*       m_lightProps  = nullptr;
	EnvironmentProperties* m_envProps    = nullptr;
};

} // namespace neurus
