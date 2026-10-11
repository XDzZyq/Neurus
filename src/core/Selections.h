/**
 * @file Selections.h
 * @brief Generic selection state management for objects, using value semantics.
 *
 * Selections provides a type-safe container for tracking user selections.
 * It supports single and multi-selection modes with fast lookup via hash set.
 * The active value is tracked separately from the selection order.
 */

#pragma once

#include <algorithm>
#include <unordered_set>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>

namespace neurus
{

/**
 * @brief Generic selection state manager supporting single and multi-selection,
 *        storing T values directly.
 *
 * Maintains an ordered list of selected values with fast O(1) hash lookup.
 * The active value is tracked explicitly via m_active, separate from the
 * selection list ordering.
 *
 * T must be default-constructible, equality-comparable and hashable via
 * std::hash<T>. T{} is used as the "null" sentinel (nullptr for pointers,
 * 0 for ints).
 *
 * @tparam T Value type to store.
 */

enum class SelectionMode : uint32_t 
{
	None = 0,
	Selected = 1 << 0,
	Active = 1 << 1,
};

template<class T>
class Selections
{
public:

	/** @brief Default constructor. */
	Selections() = default;

	/** @brief Virtual destructor for polymorphic use. */
	virtual ~Selections() = default;

	// Non-copyable (selections are unique per context)
	Selections(const Selections&) = delete;
	Selections& operator=(const Selections&) = delete;

	// Movable
	Selections(Selections&&) noexcept = default;
	Selections& operator=(Selections&&) noexcept = default;

	// -------------------------------------------------------------------
	// Selection API
	// -------------------------------------------------------------------

	/**
	 * @brief Selects a value.
	 * @param obj Value to select.
	 * @param increment false replaces the selection; true adds to it. Either
	 *                  way @p obj becomes the active value.
	 */
	void Select(const T& obj, bool increment)
	{
		if (increment)
		{
			auto it = m_selectionSet.find(obj);
			if (it == m_selectionSet.end())
			{
				m_selectedList.push_back(obj);
				m_selectionSet.insert(obj);
			}
			else
			{
				// Move to end (becomes active) — swap with last element
				auto itVec = std::find(m_selectedList.begin(), m_selectedList.end(), obj);
				auto lastIdx = m_selectedList.size() - 1;
				if (static_cast<size_t>(std::distance(m_selectedList.begin(), itVec)) != lastIdx)
				{
					std::swap(*itVec, m_selectedList.back());
				}
			}
			m_active = obj;
		}
		else
		{
			m_selectedList.clear();
			m_selectedList.push_back(obj);
			m_selectionSet.clear();
			m_selectionSet.insert(obj);
			m_active = obj;
		}
	}

	/**
	 * @brief Deselects a value.
	 * @param obj Value to deselect.
	 * @param increment Unused (legacy parameter, kept for API compatibility).
	 * @note If @p obj was active, the last remaining value becomes active.
	 */
	void Deselect(const T& obj, bool increment)
	{
		(void)increment;
		auto it = m_selectionSet.find(obj);
		if (it == m_selectionSet.end()) return;

		m_selectionSet.erase(it);

		auto itVec = std::find(m_selectedList.begin(), m_selectedList.end(), obj);
		if (itVec != m_selectedList.end())
		{
			m_selectedList.erase(itVec);
		}

		if (m_active == obj)
		{
			m_active = m_selectedList.empty() ? T{} : m_selectedList.back();
		}
	}

	/**
	 * @brief Returns the active (primary) selected value.
	 *
	 * @return Active value, or T{} if no selection
	 *         (nullptr for pointer types, 0 for int types).
	 */
	T GetActiveObject() const
	{
		if (m_selectedList.empty())
			return T{};
		return m_active;
	}

	/**
	 * @brief Returns the first selected value.
	 *
	 * @return First selected value, or T{} if no selection.
	 */
	T GetSelectedObjects() const
	{
		if (m_selectedList.empty())
			return T{};
		return m_selectedList.front();
	}

	/** @brief Checks if a value is selected (O(1) hash lookup). */
	bool IsSelected(const T& obj) const
	{
		return m_selectionSet.find(obj) != m_selectionSet.end();
	}

	bool IsActive(const T& obj) const
	{
		return m_active == obj;
	}

	SelectionMode GetMode(const T& obj) const
	{
		uint32_t mode = 0;
		if (IsSelected(obj))
			mode |= uint32_t(SelectionMode::Selected);
		if (IsActive(obj))
			mode |= uint32_t(SelectionMode::Active);
		return SelectionMode(mode);
	}

	/**
	 * @brief Returns the number of selected values.
	 */
	size_t GetSelectionCount() const
	{
		return m_selectedList.size();
	}

	/**
	 * @brief Returns the ordered list of selected values.
	 */
	const std::vector<T>& GetSelectedList() const
	{
		return m_selectedList;
	}

	/**
	 * @brief Clears all selections and resets the active value to T{}.
	 */
	void ClearSelection()
	{
		m_selectedList.clear();
		m_selectionSet.clear();
		m_active = T{};
	}

	/**
	 * @brief Replaces the entire selection set with an absolute state.
	 * @param list Ordered selected values (becomes the new selection order).
	 * @param active Active value (T{} for none).
	 * @note Absolute setter for replaying a recorded end state, independent of
	 *       the incremental history.
	 */
	void RestoreState(std::vector<T> list, const T& active)
	{
		m_selectedList = std::move(list);
		m_selectionSet.clear();
		m_selectionSet.insert(m_selectedList.begin(), m_selectedList.end());
		m_active = active;
	}

	// -------------------------------------------------------------------
	// Cereal serialization
	// -------------------------------------------------------------------

	/**
	 * @brief Serializes the ordered selection list.
	 *
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 * @note Only the list is stored; on load the hash set is rebuilt and the
	 *       last list element becomes active.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("selectedValues", m_selectedList));

		// Rebuild the hash set and make the last element active; the active
		// value is not stored separately.
		if constexpr (Archive::is_loading::value)
		{
			m_selectionSet.clear();
			m_selectionSet.insert(m_selectedList.begin(), m_selectedList.end());
			m_active = m_selectedList.empty() ? T{} : m_selectedList.back();
		}
	}

private:
	/// Ordered list of selected values (preserves selection order).
	std::vector<T> m_selectedList;

	/// Fast hash lookup set for O(1) IsSelected() queries.
	std::unordered_set<T> m_selectionSet;

	/// Active (primary) selected value, T{} if no selection.
	T m_active = T{};
};

} // namespace neurus
