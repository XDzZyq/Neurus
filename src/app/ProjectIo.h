#pragma once

#include <filesystem>
#include <string>

namespace neurus
{

class Editor;

namespace project
{
class Project;
}

/**
 * @brief Registers the live editor components on a project serializer.
 *
 * Order matters: the resource pool is registered first, before the scene
 * resolves its references against it. Shared by the Qt application and the
 * headless renderer so both read and write the same file format.
 *
 * @param uiLayout Opaque UI layout blob, carried through unchanged. A headless
 *                 caller passes an empty string.
 */
void RegisterProjectComponents(project::Project& proj, Editor& editor, std::string& uiLayout);

/**
 * @brief Loads a project into @p editor.
 *
 * The editor is emptied first (BeginLoad), so a failure leaves no document
 * behind: the caller decides whether to create a default scene.
 *
 * @param uiLayout Receives the stored layout blob.
 * @param error    Receives the reason on failure.
 */
bool LoadProject(const std::filesystem::path& path, Editor& editor,
                 std::string& uiLayout, std::string& error);

/**
 * @brief Saves the editor state to @p path and clears the dirty flag.
 * @param error Receives the reason on failure.
 */
bool SaveProject(const std::filesystem::path& path, Editor& editor,
                 const std::string& uiLayout, std::string& error);

} // namespace neurus
