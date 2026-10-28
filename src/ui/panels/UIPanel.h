/**
 * @file UIPanel.h
 * @brief Base class for all dock panel widgets in the UI layer.
 *
 * UIPanel provides the common QWidget base for dockable panels so the dock
 * system can treat them uniformly: a PanelType for registry lookup, a
 * human-readable dock title, and a stable serialization id.
 *
 * Identity vs display: PanelId() is a fixed ASCII string that is persisted in
 * saved layouts and must never change; PanelName() follows the active language
 * and must never be persisted or used as a lookup key.
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
	ScriptConsole,
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
	 * These strings are the ads::CDockWidget objectName and are part of the
	 * saved layout format: changing one invalidates every saved layout, so
	 * treat them as frozen. They are intentionally NOT the translation keys.
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
		case PanelType::ScriptConsole: return "dock.scriptConsole";
		case PanelType::Count:         break;
		}
		return "dock.unknown";
	}

	/** @brief Returns this panel's stable serialization id. */
	QString PanelId() const { return QString::fromLatin1(PanelIdFor(m_type)); }

	/**
	 * @brief Refreshes the panel's display from a UIContext snapshot.
	 *
	 * @param ctx Read-only UI context.
	 */
	virtual void Refresh(const neurus::UIContext& ctx) = 0;

	/**
	 * @brief Re-applies all user-visible texts in the current language.
	 *
	 * Panels without translatable text (e.g. Viewport) keep the default no-op.
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
		case PanelType::ScriptConsole:  return "Script Console";
		case PanelType::Count:          break;
		}
		// Unreachable: Count is the enumerator bound, never a panel's type.
		// Listed explicitly (no `default:`) so the compiler flags a new panel
		// type that forgot its name key.
		return "Unknown";
	}

	PanelType m_type;
	const char* m_nameKey;
};

} // namespace neurus
