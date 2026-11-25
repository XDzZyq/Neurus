#pragma once

#include <memory>

namespace neurus
{

/// An object's optional membership in a ResourceManager. Main thread only.
class UID
{
	friend class ResourceManager;
	std::weak_ptr<const int> m_membership;

public:
	UID() = default;
	virtual ~UID() = default;
	UID(const UID&) = delete;
	UID& operator=(const UID&) = delete;
	UID(UID&&) = delete;
	UID& operator=(UID&&) = delete;

	/// Returns the pool key, or logs an error and returns -1 for an unpooled object.
	int GetObjectID() const;

	// The owning pool persists identity in its record key.
	template<class Archive>
	void serialize(Archive&) {}
};

} // namespace neurus
