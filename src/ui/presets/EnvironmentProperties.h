/**
 * @file EnvironmentProperties.h
 * @brief Environment (IBL) property editor subpanel.
 *
 * Displays o_intensity and o_rotation as ScalarSliders, and the pooled
 * ImageData source path as a read-only label (fed via setEquirectPath()).
 * All edits emit signals carrying the object ID for routing through the
 * Editor event system.
 *
 * Architecture:
 * - QWidget subclass with two QGroupBox sections: "Equirectangular Map"
 *   (read-only path label) and "IBL Parameters" (intensity + rotation)
 * - Lazy update via dirty-check: each setter caches the last value and
 *   no-ops if unchanged
 * - setObjectId() resets all caches when object changes (forces full refresh)
 * - No Vulkan or Renderer dependencies — pure Qt UI layer
 * - Lives in src/ui/items/ alongside CameraProperties, MeshProperties, and
 *   ScalarSlider
 */

#pragma once

#include <QWidget>

#include <string>

class QGroupBox;
class QLabel;

namespace neurus {

class ScalarSlider;

/**
 * @brief Environment-specific property editor subpanel.
 *
 * Layout:
 *   [Equirectangular Map group]
 *     Source path   (readonly QLabel, gray, smaller font, word-wrapped,
 *                    text-selectable; path owned by the pooled ImageData)
 *   [IBL Parameters group]
 *     Intensity       (ScalarSlider: 0–10, initial 1.0)
 *     Rotation (°)    (ScalarSlider: 0–360, initial 0.0)
 */
class EnvironmentProperties : public QWidget
{
	Q_OBJECT

public:
	explicit EnvironmentProperties(QWidget* parent = nullptr);
	~EnvironmentProperties() override = default;

	EnvironmentProperties(const EnvironmentProperties&) = delete;
	EnvironmentProperties& operator=(const EnvironmentProperties&) = delete;

	/** @brief Sets the editing object ID (resets caches when ID changes). */
	void setObjectId(int id);

	/** @brief Updates the intensity slider (dirty-checked). */
	void setIntensity(float intensity);

	/** @brief Updates the rotation slider (dirty-checked). */
	void setRotation(float rotation);

	/** @brief Updates the equirect path label (dirty-checked). */
	void setEquirectPath(const std::string& path);

	/** @brief Re-applies group/label texts in the active language. */
	void Retranslate();

signals:
	/** @brief Emitted when the intensity slider changes. */
	void intensityChanged(int objectId, float intensity);

	/** @brief Emitted when the rotation slider changes. */
	void rotationChanged(int objectId, float rotation);

private:
	int m_objectId = -1;

	// --- Widgets ---
	QGroupBox*    m_group           = nullptr;
	QLabel*       m_mapLabel        = nullptr;
	QLabel*       m_pathPrefix      = nullptr;
	QLabel*       m_iblLabel        = nullptr;
	QLabel*       m_intensityLabel  = nullptr;
	QLabel*       m_rotationLabel   = nullptr;
	ScalarSlider* m_intensitySlider = nullptr;
	ScalarSlider* m_rotationSlider  = nullptr;
	QLabel*       m_pathLabel       = nullptr;

	// --- Cached values for dirty-check ---
	float       m_cachedIntensity = -1.0f;
	float       m_cachedRotation  = -999.0f;
	std::string m_cachedPath;
};

} // namespace neurus
