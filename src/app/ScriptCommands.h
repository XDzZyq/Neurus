#pragma once

#include "command/CommandTypes.h"

#include <string>

namespace neurus
{

class Editor;
class Preferences;
class ScriptEngine;

/**
 * @brief Executes the `script.*` command family against a live engine.
 *
 * Shared by the Qt application and the headless renderer so the CLI sees one
 * behaviour on every platform. Covers `script.info`, `script.console`,
 * `script.clear`, `script.run` and `script.eval`, including the host sandbox
 * policy: an external command may never choose trusted mode.
 *
 * @param projectRoot Directory substituted when a sandbox root list is empty.
 */
command::CommandResult ExecuteScriptCommand(const command::CommandRequest& request,
                                            ScriptEngine& script,
                                            Editor& editor,
                                            const Preferences& preferences,
                                            const std::string& projectRoot);

} // namespace neurus
