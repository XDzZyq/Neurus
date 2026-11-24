#pragma once

#include "core/IResourceLookup.hpp"
#include "core/Serialize.hpp"

#include <functional>
#include <memory>
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

/// Shared ownership in memory, a scalar UID in a project. A field can supply
/// an acceptance predicate for restoring a narrower kind of resource.
template<class T, bool (*Accept)(const T&) = nullptr>
class ResourceRef
{
public:
	ResourceRef& operator=(std::shared_ptr<T> value)
	{
		m_value = std::move(value);
		return *this;
	}

	T* get() const { return m_value.get(); }
	T* operator->() const { return get(); }
	T& operator*() const { return *m_value; }
	explicit operator bool() const { return static_cast<bool>(m_value); }
	int id() const { return m_value ? m_value->GetObjectID() : 0; }

	template<class Archive>
	int save_minimal(const Archive&) const { return id(); }

	template<class Archive>
	void load_minimal(const Archive& ar, const int& uid)
	{
		m_value.reset();
		if (uid == 0) return;
		ArchiveResourceRefs::Bind(ar, uid, [this](const std::shared_ptr<UID>& resource) {
			auto value = std::dynamic_pointer_cast<T>(resource);
			if constexpr (Accept != nullptr)
				if (value && !Accept(*value)) value.reset();
			m_value = std::move(value);
		});
	}

private:
	std::shared_ptr<T> m_value;
};

namespace archive
{
/// Omit a null reference on save and restore a missing field as null on load.
template<class Archive, class T, bool (*Accept)(const T&)>
void OptionalRef(Archive& ar, const char* key, ResourceRef<T, Accept>& ref)
{
	if constexpr (Archive::is_saving::value)
	{
		if (ref) ar(cereal::make_nvp(key, ref));
	}
	else if (!OptionalField(ar, cereal::make_nvp(key, ref)))
		ref = nullptr;
}
} // namespace archive
} // namespace neurus
