#pragma once

#include <string>

namespace neurus {

/** @brief Emitted when a new project is requested (Ctrl+N). */
struct ProjectNewEvent {};

/** @brief Emitted when an existing project file should be opened (Ctrl+O). */
struct ProjectOpenEvent { std::string path; };

/** @brief Emitted when the current project should be saved (Ctrl+S). */
struct ProjectSaveEvent {};

/** @brief Emitted when the current project should be saved to a new path (Ctrl+Shift+S). */
struct ProjectSaveAsEvent { std::string path; };

} // namespace neurus
