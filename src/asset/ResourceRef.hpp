#pragma once

#include "core/IResourceLookup.hpp"
#include "core/Serialize.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace cereal { class JSONInputArchive; }

namespace neurus
{

/// Collects UID references while cereal constructs pooled objects. The reader
/// resolves the fields once all targets are available. Fields must belong to
/// objects with stable addresses until Resolve returns (as pooled UIDs do).
class ArchiveResourceRefs
{
public:
	explicit ArchiveResourceRefs(const cereal::JSONInputArchive& archive);
	~ArchiveResourceRefs();
	ArchiveResourceRefs(const ArchiveResourceRefs&) = delete;
	ArchiveResourceRefs& operator=(const ArchiveResourceRefs&) = delete;

	using Assign = std::function<void(const std::shared_ptr<UID>&)>;
	static void Bind(const cereal::JSONInputArchive& archive, int id, Assign assign);
	void Resolve(const IResourceLookup& pool);

private:
	const cereal::JSONInputArchive& m_archive;
	ArchiveResourceRefs* m_previous;
	std::vector<std::pair<int, Assign>> m_bindings;
};

enum class MissingResource { Clear, KeepUid };

/// Shared ownership in memory, a scalar UID in a project. A field can supply
/// an acceptance predicate for restoring a narrower kind of resource.
template<class T, bool (*Accept)(const T&) = nullptr, MissingResource Missing = MissingResource::Clear>
class ResourceRef
{
public:
	ResourceRef() = default;
	ResourceRef(std::nullptr_t) {}
	template<class U> requires std::is_convertible_v<U*, T*>
	ResourceRef(std::shared_ptr<U> value) : m_value(std::move(value)) {}

	ResourceRef& operator=(std::shared_ptr<T> value)
	{
		m_uid.reset();
		m_value = std::move(value);
		return *this;
	}
	template<class U> requires std::is_convertible_v<U*, T*>
	ResourceRef& operator=(std::shared_ptr<U> value)
	{
		return *this = std::shared_ptr<T>(std::move(value));
	}
	ResourceRef& operator=(std::nullptr_t) { reset(); return *this; }
	void reset() { m_uid.reset(); m_value.reset(); }
	const std::shared_ptr<T>& shared() const { return m_value; }

	/// A document or undo record can carry an identity before it has a pool.
	static ResourceRef FromId(int uid)
	{
		ResourceRef ref;
		ref.m_uid = uid;
		return ref;
	}
	bool Resolve(const IResourceLookup& pool)
	{
		if (HasIdentity())
		{
			const int uid = id();
			BindValue(pool.Get(uid), uid);
		}
		return static_cast<bool>(m_value);
	}

	T* get() const { return m_value.get(); }
	T* operator->() const { return get(); }
	T& operator*() const { return *m_value; }
	explicit operator bool() const { return static_cast<bool>(m_value); }
	int id() const { return m_value ? m_value->GetObjectID() : m_uid.value_or(0); }
	bool operator==(const ResourceRef& other) const
	{
		return id() == other.id() && HasIdentity() == other.HasIdentity();
	}
	bool operator==(std::nullptr_t) const { return !m_value; }

	template<class Archive>
	int save_minimal(const Archive&) const { return id(); }

	template<class Archive>
	void load_minimal(const Archive& ar, const int& uid)
	{
		reset();
		if (uid == 0) return;
		m_uid = uid;
		ArchiveResourceRefs::Bind(ar, uid, [this, uid](const std::shared_ptr<UID>& resource) {
			BindValue(resource, uid);
		});
	}

private:
	bool HasIdentity() const { return m_value || m_uid.has_value(); }
	void BindValue(const std::shared_ptr<UID>& resource, int uid)
	{
		auto value = std::dynamic_pointer_cast<T>(resource);
		if constexpr (Accept != nullptr)
			if (value && !Accept(*value)) value.reset();
		m_uid.reset();
		if constexpr (Missing == MissingResource::KeepUid)
		{
			if (!value)
			{
				m_uid = uid;
				NEURUS_ERR("[ResourceRef] Missing or incompatible pooled resource UID " << uid);
			}
		}
		m_value = std::move(value);
	}
	// Only unresolved references store an integer. A live object owns its UID.
	std::optional<int> m_uid;
	std::shared_ptr<T> m_value;
};

namespace archive
{
/// Omit a null reference on save and restore a missing field as null on load.
template<class Archive, class T, bool (*Accept)(const T&), MissingResource Missing>
void OptionalRef(Archive& ar, const char* key, ResourceRef<T, Accept, Missing>& ref)
{
	if constexpr (Archive::is_saving::value)
	{
		if (ref || ref.id() != 0) ar(cereal::make_nvp(key, ref));
	}
	else if (!OptionalField(ar, cereal::make_nvp(key, ref)))
		ref = nullptr;
}

/// Portable records can move before a pool is available. Transfer only the
/// identity here and resolve the record when it is applied to the document.
template<class Archive, class Ref>
bool RefId(Archive& ar, const char* key, Ref& ref)
{
	int uid = ref.id();
	if (!OptionalField(ar, cereal::make_nvp(key, uid))) return false;
	if constexpr (Archive::is_loading::value) ref = Ref::FromId(uid);
	return true;
}

template<class Archive, class Ref>
void OptionalRefId(Archive& ar, const char* key, Ref& ref)
{
	if constexpr (Archive::is_saving::value)
	{
		if (ref.id() != 0) RefId(ar, key, ref);
	}
	else if (!RefId(ar, key, ref) || ref.id() == 0)
		ref = nullptr;
}
} // namespace archive
} // namespace neurus
