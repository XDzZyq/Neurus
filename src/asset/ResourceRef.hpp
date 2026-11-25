#pragma once

#include "asset/ResourceInputArchive.hpp"
#include "core/IResourceLookup.hpp"
#include "core/Serialize.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace neurus
{

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
		// 0 and -1 both mean "no reference": old files wrote 0, and a null
		// reference saves as -1.
		if (uid <= 0) return;
		const auto* input = dynamic_cast<const ResourceInputArchive*>(&ar);
		if (!input)
			throw std::logic_error("Reading resource reference " + std::to_string(uid) +
				" requires a ResourceInputArchive");
		auto value = input->Pool().Get<T>(uid);
		if constexpr (Accept != nullptr)
			if (value && !Accept(*value)) value.reset();
		m_value = std::move(value);
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
