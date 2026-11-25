#pragma once

/**
 * @file Project.hpp
 * @brief Registration-based serializer for project persistence.
 *
 * Project owns no data itself: callers register components via
 * Register<T>(args...) and Save/Load drives them through their virtual
 * methods. No concrete scene or render type is known here.
 */

#include <memory>
#include <string>
#include <vector>

#include <cereal/archives/json.hpp>

#include "asset/Serializable.hpp"

namespace neurus::project
{

/**
 * @brief Registration-based serializer.
 *
 * Owns no data; Save/Load drives the components registered on it.
 */
class Project
{
public:
	Project() = default;

	// --- Component registration ---

	/**
	 * @brief Registers a new Serializable component.
	 * @tparam T The concrete component type (must inherit Serializable).
	 * @tparam Args Constructor argument types for T.
	 * @param args Forwarded to T's constructor.
	 * @return Reference to the newly created component.
	 */
	template<typename T, typename... Args>
	T& Register(Args&&... args)
	{
		auto comp = std::make_unique<T>(std::forward<Args>(args)...);
		T& ref = *comp;
		m_components.push_back(std::move(comp));
		return ref;
	}

	// --- Lifecycle ---

	/**
	 * @brief Removes all registered components.
	 */
	void Clear() { m_components.clear(); }

	// --- Persistence ---

	/**
	 * @brief Saves all components to a .neurus.json file.
	 * @param path Filesystem path for the output file.
	 * @throws std::runtime_error if the file cannot be created.
	 */
	void Save(const std::string& path) const;

	/**
	 * @brief Loads all components from a .neurus.json file.
	 * @param path Filesystem path to the .neurus.json file.
	 * @throws std::runtime_error if the file cannot be opened.
	 */
	void Load(const std::string& path);

private:
	friend class cereal::access;

	// Serializable writes each component to the same JSON output archive.

	template<class Archive>
	void save(Archive& ar) const
	{
		for (auto& comp : m_components)
			comp->Save(static_cast<cereal::JSONOutputArchive&>(ar));
	}

	std::vector<std::unique_ptr<Serializable>> m_components;
};

} // namespace neurus::project


