/**
 * @file DebugProperties.h
 * @brief Property subpanel for the three debug object types.
 *
 * One preset serves DebugLine, DebugPoints and DebugMesh: they share the
 * color / opacity / x-ray knobs, and the type-specific rows are shown or
 * hidden by setDebugType().
 *
 * The position table is edited absolutely - a cell edit, an Add and a Remove
 * all emit the whole list - because a structural change cannot be expressed as
 * a per-index update, and one event shape keeps the undo story to a single op.
 */

#pragma once

#include <QWidget>

#include <vector>

#include <glm/glm.hpp>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

namespace neurus
{

class ColorButton;
class ScalarSlider;

class DebugProperties : public QWidget
{
	Q_OBJECT

public:
	explicit DebugProperties(QWidget* parent = nullptr);
	~DebugProperties() override = default;

	DebugProperties(const DebugProperties&) = delete;
	DebugProperties& operator=(const DebugProperties&) = delete;

	/** @brief Re-applies every label in the active language. */
	void Retranslate();

	/** @brief Binds the panel to an object UID, invalidating all caches. */
	void setObjectId(int id);

	/**
	 * @brief Shows the rows that apply to this debug type, hides the rest.
	 * @param goType ObjectID::GOType value (GO_DL, GO_DP or GO_DM) as an int.
	 *
	 * Takes an int so the UI layer does not need the scene header just to
	 * switch rows.
	 */
	void setDebugType(int goType);

	// --- Shared properties (all three types) ---
	void setColor(const glm::vec4& color);
	void setOpacity(float opacity);
	void setXRay(bool xray);

	// --- DebugLine only ---
	void setLineWidth(float width);
	void setStipple(bool stipple);

	// --- DebugPoints only ---
	void setPointType(int pointType);
	void setPointScale(float scale);
	void setProjectionMode(int mode);

	// --- DebugMesh only ---
	void setMeshPath(const QString& path);

	/**
	 * @brief Fills the position table from the object's list.
	 *
	 * Rows past kMaxRows are not shown; the count label still says how many
	 * exist either way, and the table turns read-only in that state so a
	 * truncated edit cannot silently delete the hidden tail.
	 */
	void setPositions(const std::vector<glm::vec3>& positions);

signals:
	void colorChanged(int objectId, const glm::vec4& color);
	void opacityChanged(int objectId, float opacity);
	void xrayChanged(int objectId, bool xray);
	void lineWidthChanged(int objectId, float width);
	void stippleChanged(int objectId, bool stipple);
	void pointTypeChanged(int objectId, int pointType);
	void pointScaleChanged(int objectId, float scale);
	void projectionModeChanged(int objectId, int mode);
	void positionsChanged(int objectId, const std::vector<glm::vec3>& positions);

private:
	/** @brief Upper bound on table rows; see setPositions(). */
	static constexpr int kMaxRows = 512;

	void BuildSharedRows(QVBoxLayout* layout);
	void BuildLineRows(QVBoxLayout* layout);
	void BuildPointRows(QVBoxLayout* layout);
	void BuildPositionTable(QVBoxLayout* layout);
	void ConnectSignals();

	/** @brief Reads the table back into a vector and emits positionsChanged(). */
	void EmitPositions();

	/** @brief Appends a row seeded from the last one (or the origin) and emits. */
	void AddPosition();

	/** @brief Removes the selected rows (or the last one) and emits. */
	void RemovePosition();

	// --- State ---
	int m_objectId = -1;

	// --- Widgets: group + shared rows ---
	QGroupBox*    m_group        = nullptr;
	QLabel*       m_colorLabel   = nullptr;
	ColorButton*  m_colorBtn     = nullptr;
	QLabel*       m_opacityLabel = nullptr;
	ScalarSlider* m_opacitySlider = nullptr;
	QCheckBox*    m_xrayChk      = nullptr;

	// --- Widgets: DebugLine rows ---
	QWidget*      m_widthRow    = nullptr;
	QLabel*       m_widthLabel  = nullptr;
	ScalarSlider* m_widthSlider = nullptr;
	QCheckBox*    m_stippleChk  = nullptr;

	// --- Widgets: DebugPoints rows ---
	QWidget*      m_pointTypeRow   = nullptr;
	QLabel*       m_pointTypeLabel = nullptr;
	QComboBox*    m_pointTypeCombo = nullptr;
	QWidget*      m_scaleRow       = nullptr;
	QLabel*       m_scaleLabel     = nullptr;
	ScalarSlider* m_scaleSlider    = nullptr;
	QWidget*      m_projectionRow   = nullptr;
	QLabel*       m_projectionLabel = nullptr;
	QComboBox*    m_projectionCombo = nullptr;

	// --- Widgets: DebugMesh rows ---
	QWidget* m_meshRow      = nullptr;
	QLabel*  m_meshCaption  = nullptr;
	QLabel*  m_meshPathLabel = nullptr;

	// --- Widgets: position table ---
	QWidget*      m_positionsBlock = nullptr;
	QLabel*       m_positionsLabel = nullptr;
	QTableWidget* m_positionsTable = nullptr;
	QPushButton*  m_addBtn         = nullptr;
	QPushButton*  m_removeBtn      = nullptr;
	QLabel*       m_countLabel     = nullptr;

	// --- Dirty-check caches (sentinels chosen outside each valid range) ---
	glm::vec4 m_cachedColor{ -1.0f };
	float     m_cachedOpacity = -1.0f;
	int       m_cachedXRay = -1;
	float     m_cachedWidth = -1.0f;
	int       m_cachedStipple = -1;
	int       m_cachedPointType = -1;
	float     m_cachedScale = -1.0f;
	int       m_cachedProjection = -1;
	QString   m_cachedMeshPath;
	std::vector<glm::vec3> m_cachedPositions;

	/** @brief Guards EmitPositions() while setPositions() repopulates the table. */
	bool m_populating = false;
};

} // namespace neurus
