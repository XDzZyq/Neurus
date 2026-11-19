/**
 * @file PassResources.cpp
 * @brief Port-name image resolution for one node execution.
 */

#include "render_graph/PassResources.hpp"

#include <utility>

namespace neurus {

void PassResources::SetInput(std::string name, Image* image)
{
	m_inputs.insert_or_assign(std::move(name), image);
}

void PassResources::SetOutput(std::string name, Image* image)
{
	m_outputs.insert_or_assign(std::move(name), image);
}

Image* PassResources::Input(const std::string_view port) const
{
	const auto it = m_inputs.find(std::string(port));
	return it == m_inputs.end() ? nullptr : it->second;
}

Image* PassResources::Output(const std::string_view port) const
{
	const auto it = m_outputs.find(std::string(port));
	return it == m_outputs.end() ? nullptr : it->second;
}

} // namespace neurus
