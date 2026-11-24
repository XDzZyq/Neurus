#pragma once

#include <memory>
#include <optional>

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

	std::optional<int> GetObjectID() const noexcept;
	/// Returns the pool key or throws if the caller supplied an ephemeral object.
	int RequireObjectID() const;

	// The owning pool persists identity in its record key.
	template<class Archive>
	void serialize(Archive&) {}
};

} // namespace neurus
