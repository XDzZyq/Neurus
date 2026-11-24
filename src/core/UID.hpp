/**
 * @file UID.hpp
 * @brief Generic unique identifier primitive (core layer).
 *
 * Provides the base UID class for unique ID generation. Every object that
 * needs identity tracking inherits from UID: scene graph objects via
 * ObjectID (scene layer), and data resources (MeshData, ImageData, Shader)
 * directly.
 */

#pragma once

#include <cereal/cereal.hpp>

namespace neurus
{

/// An identity supplied by a persisted resource record.
struct RestoredId
{
	int value;
};

/**
 * @brief Base class providing globally unique integer IDs.
 *
 * UID generates sequential unique IDs for all instances from a monotonically
 * increasing counter, so IDs never collide.
 *
 * @note Not thread-safe. IDs should be allocated on the main thread.
 * @note IDs are never reused, even after object destruction.
 * @note Non-copyable and non-movable - identity must not be duplicated or
 *       transferred. Objects are held via shared_ptr.
 */
class UID
{
private:
	int o_id;            ///< Unique identifier for this instance
	static int s_count;  ///< Global counter for ID generation

public:
	/**
	 * @brief Constructs a UID and assigns a unique ID.
	 */
	UID();
	explicit UID(RestoredId id);

	/**
	 * @brief Virtual destructor for polymorphic use.
	 */
	virtual ~UID() = default;

	/** @brief Deleted copy constructor - UIDs must be unique. */
	UID(const UID&) = delete;

	/** @brief Deleted copy assignment - UIDs must be unique. */
	UID& operator=(const UID&) = delete;

	/** @brief Deleted move constructor - UIDs must be unique. */
	UID(UID&&) = delete;

	/** @brief Deleted move assignment - UIDs must be unique. */
	UID& operator=(UID&&) = delete;

	/**
	 * @brief Returns the unique ID of this object.
	 * @return Unique integer identifier.
	 */
	inline int GetObjectID() const
	{
		return o_id;
	}

	/**
	 * @brief Returns total number of UIDs allocated.
	 * @return Total allocation count (includes destroyed objects).
	 */
	static int GetTotalAllocated()
	{
		return s_count;
	}

	/** @brief Advances the allocator past a valid persisted identity. */
	static void ReserveThrough(int id);

	/**
	 * @brief Cereal serialization for the unique ID.
	 *
	 * On load, bumps s_count past the restored ID so newly created objects do
	 * not collide with restored ones.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(CEREAL_NVP(o_id));
		if constexpr (Archive::is_loading::value)
		{
			ReserveThrough(o_id);
		}
	}
};

} // namespace neurus
