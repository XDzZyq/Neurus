/**
 * @file ScriptPreferences.h
 * @brief Plain UI-side values for the Script Sandbox and Script Preload rows.
 *
 * The Preferences dialog edits text; the Application parses these strings into
 * its persisted vectors. Keeping the carrier plain separates the widget from
 * the app-layer Preferences type, which the UI layer never includes.
 */

#pragma once

#include <QString>

namespace neurus
{

/** @brief Script settings shown in the Preferences dialog. */
struct ScriptPreferencesUi
{
	QString readRoots;      ///< Semicolon-separated sandbox read roots.
	QString writeRoots;     ///< Semicolon-separated sandbox write roots.
	QString blockedModules; ///< Semicolon-separated blocked module names.
	QString preload;        ///< Comma- or semicolon-separated startup packages.
	bool    allowNetwork = false;    ///< Sandboxed runs may open sockets.
	bool    allowSubprocess = false; ///< Sandboxed runs may create processes.
	bool    allowCtypes = false;     ///< Sandboxed runs may use ctypes.
	int     timeLimitSeconds = 30;   ///< Wall-clock limit; 0 = unlimited.
	int     memoryLimitMb = 0;       ///< Address-space limit; 0 = unlimited.
};

} // namespace neurus
