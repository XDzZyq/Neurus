/**
 * @file UIPanel.h
 * @brief Base class for all dock panel widgets in the UI layer.
 *
 * UIPanel provides the common QWidget base for dockable panels (Outliner,
 * PropertyPanel, RenderConfigPanel). All panels inherit UIPanel so the
 * dock system can treat them uniformly.
 *
 * Architecture:
 * - Inherits QWidget — panels are embeddable in ads::CDockWidget
 * - Q_OBJECT macro for MOC signal/slot generation
 * - Each panel has a PanelType enum value for O(1) lookup in the panel registry
 * - PanelName() provides the human-readable dock title (translated via I18n)
 * - PanelId() provides the stable serialization key (never translated)
 *
 * Identity vs display (IMPORTANT):
 * - PanelId() is the panel's IDENTITY. It is a fixed ASCII string that must
 *   never change: ads::CDockWidget uses its objectName as the key when saving
 *   and restoring layouts, so a project file is only readable as long as these
 *   ids are stable. It is deliberately independent of the display text — not
 *   derived from the translation key — so renaming a UI string cannot break
 *   saved layouts.
 * - PanelName() is DISPLAY ONLY. It follows the active language and may change
 *   at any moment. It must never be persisted or used as a lookup key.
 *
 * i18n:
 * - The dock title is stored as a translation KEY (English by default);
 *   PanelName() resolves it through I18n at call time, so dock titles follow
 *   the active language without any per-panel code.
 * - Panels with additional user-visible text override Retranslate() to
 *   re-apply their strings; UIManager calls it on every language change.
 *
 * @note UI Layer — no Vulkan or Renderer dependencies.
 */

#pragma once

#include <QString>
#include <QWidget>
#include <cstdint>

#include "UIContext.h"

namespace neurus
{

/** @brief Enum identifying each dock panel type for registry lookup. */
enum class PanelType : uint8_t
{
	Viewport,
	Outliner,
	PropertyPanel,
	RenderConfig,
	ShaderEditor,
	Profiling,
	Log,
	Count
};

class UIPanel : public QWidget
{
	Q_OBJECT

public:
	/**
	 * @brief Constructs a UIPanel.
	 * @param type PanelType enum identifying this panel's role.
	 * @param nameKey I18n key for the dock title (English default string).
	 *                Empty/null → default key derived from @p type.
	 * @param parent Parent widget.
	 */
	explicit UIPanel(PanelType type, const char* nameKey = nullptr, QWidget* parent = nullptr)
		: QWidget(parent)
		, m_type(type)
		, m_nameKey(nameKey && nameKey[0] ? nameKey : DefaultNameKey(type)) {}
	~UIPanel() override = default;

	UIPanel(const UIPanel&) = delete;
	UIPanel& operator=(const UIPanel&) = delete;

	/** @brief Returns the panel type enum. */
	PanelType GetPanelType() const { return m_type; }

	/** @brief Returns the human-readable (translated) panel name. */
	QString PanelName() const;  // Defined in UIPanel.cpp (needs I18n).

	/**
	 * @brief Returns the stable, language-independent id for a PanelType.
	 *
	 * Used as the ads::CDockWidget objectName, which is the key ADS writes to
	 * and reads from saved layouts. These strings are part of the project file
	 * format: changing one invalidates every saved layout, so treat them as
	 * frozen. They are intentionally NOT the translation keys — display text
	 * must be free to change without breaking persistence.
	 */
	static const char* PanelIdFor(PanelType type)
	{
		switch (type)
		{
		case PanelType::Viewport:      return "dock.viewport";
		case PanelType::Outliner:      return "dock.outliner";
		case PanelType::PropertyPanel: return "dock.propertyPanel";
		case PanelType::RenderConfig:  return "dock.renderConfig";
		case PanelType::ShaderEditor:  return "dock.shaderEditor";
		case PanelType::Profiling:     return "dock.profiling";
		case PanelType::Log:           return "dock.log";
		case PanelType::Count:         break;
		}
		return "dock.unknown";
	}

	/** @brief Returns this panel's stable serialization id. */
	QString PanelId() const { return QString::fromLatin1(PanelIdFor(m_type)); }

	/**
	 * @brief Refreshes the panel's display from a UIContext snapshot.
	 *
	 * Called by UIManager::Refresh() each frame. Each subclass implements
	 * this to extract the data it needs from the context (RenderConfig,
	 * Scene, etc.) and update its widgets.
	 *
	 * @param ctx Read-only UI context carrying Editor/Project state.
	 */
	virtual void Refresh(const neurus::UIContext& ctx) = 0;

	/**
	 * @brief Re-applies all user-visible texts in the current language.
	 *
	 * Called by UIManager on every I18n::languageChanged() so panels switch
	 * language instantly without a restart. Panels without translatable
	 * text (e.g. Viewport) keep the default no-op.
	 */
	virtual void Retranslate() {}

private:
	/** @brief Returns the default dock-title key for a given PanelType. */
	static const char* DefaultNameKey(PanelType type)
	{
		switch (type)
		{
		case PanelType::Viewport:       return "Viewport";
		case PanelType::Outliner:       return "Outliner";
		case PanelType::PropertyPanel: return "Property Panel";
		case PanelType::RenderConfig:   return "Render Config";
		case PanelType::ShaderEditor:   return "Shader Editor";
		case PanelType::Profiling:      return "Profiling";
		case PanelType::Log:            return "Log";
		case PanelType::Count:          break;
		}
		// Count is the enumerator bound, never a panel's type — unreachable, so
		// this string is deliberately not a translation key (it would otherwise
		// need a "Dock"-context catalog entry no user can ever see). Listing
		// every enumerator instead of `default:` makes the compiler flag a new
		// panel type that forgot its name key.
		return "Unknown";
	}

	PanelType m_type;
	const char* m_nameKey;
};

} // namespace neurus
