/**
 * @file MeshProperties.h
 * @brief Reusable QWidget subpanel for editing Mesh properties.
 *
 * Displays the pooled MeshData source path (read-only label, fed via
 * setMeshPath()), using_shadow (checkbox), and using_material (checkbox).
 * All edits emit signals carrying the object ID for routing through the
 * Editor event system.
 *
 * Supports lazy update via dirty-check: each setter caches the last
 * value and no-ops if unchanged. setObjectId() resets all caches when
 * the editing target changes.
 *
 * Architecture:
 * - QWidget subclass with two QGroupBox sections: "Mesh Asset" (path) and
 *   "Mesh Flags" (checkboxes)
 * - No Vulkan or Renderer dependencies — pure Qt UI layer
 * - Lives in src/ui/items/ alongside ScalarSlider, OutlinerRow, and Vec3Spin
 */

#pragma once

#include <QWidget>

#include <string>

class QCheckBox;
class QGroupBox;
class QLabel;

namespace neurus
{

/**
 * @brief Mesh-specific property editor subpanel.
 *
 * Layout:
 *   [Mesh Asset group]
 *     Source path   (readonly QLabel, gray, word-wrapped, text-selectable;
 *                    path owned by the pooled MeshData)
 *   [Mesh Flags group]
 *     Cast Shadow   (QCheckBox)
 *     Use Material  (QCheckBox)
 */
class MeshProperties : public QWidget
{
	Q_OBJECT

public:
	explicit MeshProperties(QWidget* parent = nullptr);
	~MeshProperties() override = default;

	MeshProperties(const MeshProperties&) = delete;
	MeshProperties& operator=(const MeshProperties&) = delete;

	/** @brief Sets the editing object ID (resets caches when ID changes). */
	void setObjectId(int id);

	/** @brief Updates the mesh path label (dirty-checked). */
	void setMeshPath(const std::string& path);

	/** @brief Updates the shadow checkbox without emitting signals (dirty-checked). */
	void setShadowEnabled(bool enabled);

	/** @brief Updates the material checkbox without emitting signals (dirty-checked). */
	void setMaterialEnabled(bool enabled);

	/** @brief Re-applies group/label/checkbox texts in the active language. */
	void Retranslate();

signals:
	/** @brief Emitted when the shadow checkbox is toggled by the user. */
	void shadowChanged(int objectId, bool enabled);

	/** @brief Emitted when the material checkbox is toggled by the user. */
	void materialChanged(int objectId, bool enabled);

private:
	int m_objectId = -1;

	// --- Widgets ---
	QGroupBox* m_group       = nullptr;
	QLabel*    m_assetLabel  = nullptr;
	QLabel*    m_pathPrefix  = nullptr;
	QLabel*    m_flagsLabel  = nullptr;
	QLabel*    m_pathLabel   = nullptr;
	QCheckBox* m_shadowChk   = nullptr;
	QCheckBox* m_materialChk = nullptr;

	// --- Cached values for dirty-check ---
	std::string m_cachedPath;
	int         m_cachedShadow   = -1;  // -1 = uninitialized
	int         m_cachedMaterial = -1;
};

} // namespace neurus
