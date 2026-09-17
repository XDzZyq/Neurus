/**
 * @file UIPanel.cpp
 * @brief Out-of-line UIPanel definitions that need the complete I18n type.
 *
 * PanelName() resolves the dock-title key through I18n; keeping it here (not
 * in the header) lets every panel header stay free of the I18n.h include.
 */

#include "panels/UIPanel.h"

#include "ui/utils/I18n.h"

namespace neurus {

QString UIPanel::PanelName() const
{
	return I18n::instance().translateCtx(m_nameKey, "Dock");
}

} // namespace neurus
