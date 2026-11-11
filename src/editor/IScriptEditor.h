#pragma once

#include <string>

namespace neurus
{

/**
 * @brief Narrow editor operations a script may invoke to create content.
 *
 * The script layer already depends on the editor layer; this interface keeps
 * those calls explicit and testable without exposing the whole Editor. Lights
 * are deliberately absent until issue #60 is fixed.
 */
class IScriptEditor
{
public:
	virtual ~IScriptEditor() = default;

	/**
	 * @brief Loads a mesh file and adds it to the scene.
	 * @param path Mesh file path, absolute or relative to the process.
	 * @param name Display name; empty keeps the generated one.
	 * @param error Receives a reason on failure.
	 * @return New object UID, or 0 on failure.
	 */
	virtual int ImportMesh(const std::string& path, const std::string& name,
	                       std::string& error) = 0;

	/**
	 * @brief Creates a camera and adds it to the scene.
	 * @param name Display name; empty keeps the generated one.
	 * @param error Receives a reason on failure.
	 * @return New object UID, or 0 on failure.
	 */
	virtual int AddCamera(const std::string& name, std::string& error) = 0;

	/**
	 * @brief Duplicates a pooled mesh or camera and adds the copy to the scene.
	 * @param sourceUid Pooled source object UID.
	 * @param name Display name for the copy; empty generates "<source> Copy".
	 * @param error Receives a reason on failure.
	 * @return New object UID, or 0 on failure.
	 */
	virtual int DuplicateObject(int sourceUid, const std::string& name,
	                            std::string& error) = 0;
};

} // namespace neurus
