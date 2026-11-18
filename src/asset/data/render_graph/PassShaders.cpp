/**
 * @file PassShaders.cpp
 * @brief Pool seeding and port extraction for the built-in pass kinds.
 */

#include "asset/data/render_graph/PassShaders.hpp"

#include "asset/data/ShaderData.hpp"
#include "core/ResourceManager.hpp"

#include <algorithm>
#include <utility>

namespace neurus {

PassShaders::PassShaders(ResourceManager& resources, const PassShaderFactory& factory,
                         std::vector<std::string>& problems)
{
	// A restored project already contains the pass shaders; adopt them by name.
	std::unordered_map<std::string, int> byName;
	resources.ForEachUID([&byName](const std::shared_ptr<UID>& resource)
	{
		const auto shader = std::dynamic_pointer_cast<ShaderData>(resource);
		if (shader)
			byName.emplace(shader->GetName(), shader->GetObjectID());
	});

	for (const PassKindInfo& kind : kPassKinds)
	{
		Kind info;
		std::vector<const ShaderData*> shaders;

		for (const PassShaderSource& source : kind.shaders)
		{
			int uid = 0;
			const auto existing = byName.find(std::string(source.name));
			if (existing != byName.end())
				uid = existing->second;

			if (uid == 0)
			{
				std::shared_ptr<ShaderData> created = factory(source);
				if (!created)
				{
					problems.push_back("pass kind '" + std::string(kind.name)
					                   + "' has no shader '" + std::string(source.name) + "'");
					continue;
				}
				uid = resources.Register(std::move(created));
				byName.emplace(std::string(source.name), uid);
			}

			const std::shared_ptr<ShaderData> shader = resources.Get<ShaderData>(uid);
			if (!shader)
			{
				problems.push_back("pass kind '" + std::string(kind.name) + "' shader '"
				                   + std::string(source.name) + "' is not pooled");
				continue;
			}

			info.shaderUids.push_back(uid);
			shaders.push_back(shader.get());
		}

		if (shaders.empty())
			continue;

		std::vector<ShaderPort> declared;
		declared.reserve(kind.declaredOutputs.size());
		for (const DeclaredPort& port : kind.declaredOutputs)
		{
			declared.push_back({std::string(port.name), PortDirection::Output,
			                    std::string(port.type), 0, 0});
		}

		info.ports = ExtractPassPorts(std::string(kind.name), shaders, declared);
		for (const std::string_view external : kind.externalInputs)
			info.externals.emplace_back(external);

		m_kinds.emplace(std::string(kind.name), std::move(info));
	}
}

const PassPorts* PassShaders::FindPassPorts(const std::string_view passKind) const
{
	const auto it = m_kinds.find(std::string(passKind));
	return it == m_kinds.end() ? nullptr : &it->second.ports;
}

std::vector<std::string> PassShaders::ExternalInputs(const std::string_view passKind) const
{
	const auto it = m_kinds.find(std::string(passKind));
	return it == m_kinds.end() ? std::vector<std::string>{} : it->second.externals;
}

int PassShaders::ShaderUid(const std::string_view passKind) const
{
	const auto it = m_kinds.find(std::string(passKind));
	if (it == m_kinds.end() || it->second.shaderUids.empty())
		return 0;
	return it->second.shaderUids.front();
}

std::vector<int> PassShaders::ShaderUids(const std::string_view passKind) const
{
	const auto it = m_kinds.find(std::string(passKind));
	return it == m_kinds.end() ? std::vector<int>{} : it->second.shaderUids;
}

} // namespace neurus
