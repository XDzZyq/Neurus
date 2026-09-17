/**
 * @file CameraProperties.h
 * @brief Camera-specific property editor subpanel.
 *
 * Displays cam_tar (look-at target) as a Vec3Spin and cam_pers (FOV)
 * as a ScalarSlider. All edits emit signals carrying the object ID
 * for routing through the Editor event system.
 *
 * Architecture:
 * - QWidget subclass with internal QVBoxLayout
 * - Owns Vec3Spin and ScalarSlider child widgets in QGroupBox containers
 * - Lazy update via dirty-check: each setter caches the last value and
 *   no-ops if unchanged
 * - setObjectId() resets all caches when object changes (forces full refresh)
 * - No Vulkan or Renderer dependencies — pure Qt UI layer
 * - Lives in src/ui/items/ alongside Vec3Spin, ScalarSlider, and OutlinerRow
 */

#pragma once

#include <QWidget>
#include <glm/glm.hpp>

class QGroupBox;
class QLabel;

namespace neurus {

class Vec3Spin;
class ScalarSlider;

class CameraProperties : public QWidget
{
	Q_OBJECT

public:
	explicit CameraProperties(QWidget* parent = nullptr);
	~CameraProperties() override = default;

	CameraProperties(const CameraProperties&) = delete;
	CameraProperties& operator=(const CameraProperties&) = delete;

	/**
	 * @brief Sets the editing object ID.
	 *
	 * When the ID changes, all cached values are reset to sentinel values
	 * so the next setTarget() / setFov() call always applies the full state.
	 */
	void setObjectId(int id);

	/**
	 * @brief Updates the look-at target Vec3Spin.
	 *
	 * Dirty-checks against the cached target — no-op if unchanged.
	 * Uses Vec3Spin::setValue() which internally blocks signals.
	 */
	void setTarget(const glm::vec3& target);

	/**
	 * @brief Updates the FOV ScalarSlider.
	 *
	 * Dirty-checks against the cached FOV — no-op if unchanged.
	 * Uses ScalarSlider::setValue() which internally blocks signals.
	 */
	void setFov(float fov);

	/** @brief Re-applies group/label texts in the active language. */
	void Retranslate();

signals:
	/** @brief Emitted when the look-at target changes. */
	void targetChanged(int objectId, float x, float y, float z);

	/** @brief Emitted when the FOV changes. */
	void fovChanged(int objectId, float fov);

private:
	int m_objectId = -1;

	// --- Widgets ---
	QGroupBox*    m_group     = nullptr;
	QLabel*       m_tarLabel  = nullptr;
	QLabel*       m_fovLabel  = nullptr;
	Vec3Spin*     m_tarSpin   = nullptr;
	ScalarSlider* m_fovSlider = nullptr;

	// --- Cached values for dirty-check ---
	glm::vec3 m_cachedTarget{FLT_MAX, FLT_MAX, FLT_MAX};
	float     m_cachedFov = -1.0f;
};

} // namespace neurus
