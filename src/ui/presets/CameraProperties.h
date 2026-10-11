/**
 * @file CameraProperties.h
 * @brief Camera-specific property editor subpanel.
 *
 * Displays cam_tar (look-at target) as a Vec3Spin, cam_pers (FOV) as a
 * ScalarSlider, and the scene's activation state as a checkbox. All edits emit
 * signals carrying the object ID.
 *
 * Activation is scene state, not a camera property: it says which camera the
 * viewport looks through. Selecting a camera does not change the view - only
 * ticking this does.
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
	 * Dirty-checked against the cached target; writes with signals blocked.
	 */
	void setTarget(const glm::vec3& target);

	/**
	 * @brief Updates the FOV ScalarSlider.
	 *
	 * Dirty-checked against the cached FOV; writes with signals blocked.
	 */
	void setFov(float fov);

	/**
	 * @brief Updates the "Active Camera" checkbox from the Scene's activation.
	 *
	 * @param active True when this camera is the one the Scene has activated.
	 *
	 * Dirty-checked and written with signals blocked, so pushing the current
	 * state back never re-emits the edit that produced it.
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
	 *               view to the viewport's own camera (the Scene's "none" state).
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
