/**
 * @file CameraProperties.h
 * @brief Camera-specific property editor subpanel.
 *
 * Displays cam_tar (look-at target) as a Vec3Spin, cam_pers (FOV) as a
 * ScalarSlider, and the scene's *activation* state as a checkbox. All edits emit
 * signals carrying the object ID for routing through the Editor event system.
 *
 * Activation is scene state, not a camera property: it says which camera the
 * viewport looks through, and with none activated the Editor uses its own
 * camera. It lives here because a camera is what the checkbox is *about*, but it
 * is read from and written to the Scene, never to the Camera object. Selecting a
 * camera therefore does not change the view — only ticking this does.
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

class QCheckBox;
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

	/**
	 * @brief Updates the "Active Camera" checkbox from the Scene's activation.
	 *
	 * @param active True when this camera is the one the Scene has activated.
	 *
	 * Dirty-checked like the others and written with signals blocked, so pushing
	 * the current state back never re-emits the edit that produced it.
	 */
	void setActive(bool active);

	/** @brief Re-applies group/label texts in the active language. */
	void Retranslate();

signals:
	/** @brief Emitted when the look-at target changes. */
	void targetChanged(int objectId, float x, float y, float z);

	/** @brief Emitted when the FOV changes. */
	void fovChanged(int objectId, float fov);

	/**
	 * @brief Emitted when the activation checkbox is toggled by the user.
	 *
	 * @param active True to make this camera the view camera, false to return the
	 *               view to the editor camera (the Scene's "none" state).
	 */
	void activeCameraChanged(int objectId, bool active);

private:
	int m_objectId = -1;

	// --- Widgets ---
	QGroupBox*    m_group     = nullptr;
	QLabel*       m_tarLabel  = nullptr;
	QLabel*       m_fovLabel  = nullptr;
	Vec3Spin*     m_tarSpin   = nullptr;
	ScalarSlider* m_fovSlider = nullptr;
	QCheckBox*    m_activeChk = nullptr;

	// --- Cached values for dirty-check ---
	glm::vec3 m_cachedTarget{FLT_MAX, FLT_MAX, FLT_MAX};
	float     m_cachedFov = -1.0f;

	/// Tri-state: -1 = unknown (forces the next setActive() through), 0/1 = value.
	int       m_cachedActive = -1;
};

} // namespace neurus
