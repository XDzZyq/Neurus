#pragma once

#include "core/IResourceLookup.hpp"
#include "core/Serialize.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace cereal { class JSONInputArchive; }

namespace neurus
{

/// Project-owned services used by resource types while reading their fields.
struct ResourceReadServices
{
	virtual ~ResourceReadServices() = default;
};

/// Collects UID references while cereal constructs pooled objects. The reader
/// resolves the fields once all targets are available. Fields must belong to
/// objects with stable addresses until Resolve returns (as pooled UIDs do).
class ArchiveResourceRefs
{
public:
	explicit ArchiveResourceRefs(const cereal::JSONInputArchive& archive, const ResourceReadServices* services = nullptr);
	~ArchiveResourceRefs();
	ArchiveResourceRefs(const ArchiveResourceRefs&) = delete;
	ArchiveResourceRefs& operator=(const ArchiveResourceRefs&) = delete;

	using Assign = std::function<void(const std::shared_ptr<UID>&)>;
	static void Bind(const cereal::JSONInputArchive& archive, int id, Assign assign);
	static bool IsBound(const cereal::JSONInputArchive& archive);
	static void BindOptional(const cereal::JSONInputArchive& archive, int id, Assign assign);
	using Complete = std::function<bool(const IResourceLookup&, const ResourceReadServices*)>;
	static void AfterLoad(const cereal::JSONInputArchive& archive, int ownerUid, Complete complete);
	std::vector<int> Resolve(const IResourceLookup& pool);

private:
	const cereal::JSONInputArchive& m_archive;
	ArchiveResourceRefs* m_previous;
	struct Binding
	{
		int id;
		Assign assign;
		bool optional;
	};
	static ArchiveResourceRefs& For(const cereal::JSONInputArchive& archive);
	std::vector<Binding> m_bindings;
	std::vector<std::pair<int, Complete>> m_completions;
	const ResourceReadServices* m_services;
};

/// Shared ownership in memory, a scalar UID in a project. A field can supply
/// an acceptance predicate for restoring a narrower kind of resource.
template<class T, bool (*Accept)(const T&) = nullptr>
class ResourceRef
{
public:
	ResourceRef() = default;
	ResourceRef(std::nullptr_t) {}
	template<class U> requires std::is_convertible_v<U*, T*>
	ResourceRef(std::shared_ptr<U> value) { *this = std::move(value); }

	ResourceRef& operator=(std::shared_ptr<T> value)
	{
		if (value && value->GetObjectID() < 0)
			throw std::invalid_argument("A resource reference requires a pooled object");
		m_value = std::move(value);
		return *this;
	}
	template<class U> requires std::is_convertible_v<U*, T*>
	ResourceRef& operator=(std::shared_ptr<U> value)
	{
		return *this = std::shared_ptr<T>(std::move(value));
	}
	ResourceRef& operator=(std::nullptr_t) { reset(); return *this; }
	void reset() { m_value.reset(); }
	const std::shared_ptr<T>& shared() const { return m_value; }

	T* get() const { return m_value.get(); }
	T* operator->() const { return get(); }
	T& operator*() const { return *m_value; }
	explicit operator bool() const { return static_cast<bool>(m_value); }
	std::optional<int> identity() const
	{
		if (!m_value) return std::nullopt;
		const int id = m_value->GetObjectID();
		if (id < 0) return std::nullopt;
		return id;
	}
	int id() const { return identity().value_or(0); }
	bool operator==(const ResourceRef& other) const
	{
		return identity() == other.identity();
	}
	bool operator==(std::nullptr_t) const { return !m_value; }

	template<class Archive>
	int save_minimal(const Archive&) const { return identity().value_or(-1); }

	template<class Archive>
	void load_minimal(const Archive& ar, const int& uid)
	{
		reset();
		if (uid < 0) return;
		if (!ArchiveResourceRefs::IsBound(ar)) return;
		ArchiveResourceRefs::Bind(ar, uid, [this](const std::shared_ptr<UID>& resource) {
			auto value = std::dynamic_pointer_cast<T>(resource);
			if constexpr (Accept != nullptr)
				if (value && !Accept(*value)) value.reset();
			m_value = std::move(value);
		});
	}

	bool HasIdentity() const { return identity().has_value(); }

private:
	std::shared_ptr<T> m_value;
};

namespace archive
{
/// Omit a null reference on save and restore a missing field as null on load.
template<class Archive, class Ref>
void OptionalRef(Archive& ar, const char* key, Ref& ref)
{
	if constexpr (Archive::is_saving::value)
	{
		if (ref.HasIdentity()) ar(cereal::make_nvp(key, ref));
	}
	else if (!OptionalField(ar, cereal::make_nvp(key, ref)))
		ref = nullptr;
}

} // namespace archive
} // namespace neurus
