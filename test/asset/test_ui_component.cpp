/**
 * @file test_ui_component.cpp
 * @brief Roundtrip serialization tests for UIComponent (UI-state blob).
 *
 * UIComponent wraps an opaque std::string (produced by the UI layer,
 * bundling window geometry + ADS dock state) and serializes it under the
 * "ui" JSON key. These tests verify Save -> Load preserves the blob and
 * that an old-format project file (no "ui" node) degrades gracefully.
 *
 * Pure CPU -- no GPU, no Qt required.
 */

#include <gtest/gtest.h>
#include <memory>

#include <cstdio>   // std::remove

#include "asset/Project.hpp"
#include "asset/components/UIComponent.hpp"
#include "asset/components/SceneComponent.hpp"
#include "shader/BuiltInRenderData.hpp"
#include "asset/components/ResourceComponent.hpp"
#include "asset/components/ConfigComponent.hpp"
#include "asset/ResourceManager.hpp"
#include "core/RenderConfig.hpp"
#include "scene/Scene.hpp"

using namespace neurus;

namespace {

struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

} // namespace

TEST(UIComponentRoundtrip, PreservesBlob)
{
	TempFile tmp("test_rt_ui.neurus.json");
	for (const char* blob : {"AAAAgQ==\n<Docking><Central/></Docking>", ""})
	{
		SCOPED_TRACE(blob);
		{
			std::string ui = blob;
			project::Project project;
			project.Register<project::UIComponent>(ui);
			project.Save(tmp.path);
		}
		std::string loaded = "stale";
		project::Project project;
		project.Register<project::UIComponent>(loaded);
		project.Load(tmp.path);
		EXPECT_EQ(loaded, blob);
	}
}

// -----------------------------------------------------------------------
// Roundtrip: coexists with Scene + Config components
// -----------------------------------------------------------------------

TEST(UIComponentRoundtrip, CoexistsWithSceneAndConfig)
{
	const neurus::BuiltInRenderData catalog;
	TempFile tmp("test_rt_ui_full.neurus.json");

	const std::string blob = "GEOM==\nSTATE==";

	{
		Scene scene;
		auto config = std::make_shared<RenderConfig>();
		ResourceManager resources;
		std::string ui = blob;
		project::Project p;
		p.Register<project::ResourceComponent>(resources, catalog);
		p.Register<project::SceneComponent>(scene, resources);
		p.Register<project::ConfigComponent>(config);
		p.Register<project::UIComponent>(ui);
		p.Save(tmp.path);
	}

	Scene loadedScene;
	auto loadedConfig = std::make_shared<RenderConfig>();
	ResourceManager loadedResources;
	std::string loadedUi;
	{
		project::Project p;
		p.Register<project::ResourceComponent>(loadedResources, catalog);
		p.Register<project::SceneComponent>(loadedScene, loadedResources);
		p.Register<project::ConfigComponent>(loadedConfig);
		p.Register<project::UIComponent>(loadedUi);
		p.Load(tmp.path);
	}

	EXPECT_EQ(loadedUi, blob);
}

// -----------------------------------------------------------------------
// Backward compat: old project file with no "ui" node clears the blob
// -----------------------------------------------------------------------

TEST(UIComponentRoundtrip, OldFormatMissingUiNode)
{
	const neurus::BuiltInRenderData catalog;
	TempFile tmp("test_rt_ui_oldfmt.neurus.json");

	// Save a project WITHOUT a UIComponent (simulates an old file).
	{
		Scene scene;
		auto config = std::make_shared<RenderConfig>();
		ResourceManager resources;
		project::Project p;
		p.Register<project::ResourceComponent>(resources, catalog);
		p.Register<project::SceneComponent>(scene, resources);
		p.Register<project::ConfigComponent>(config);
		p.Save(tmp.path);
	}

	// Load WITH a UIComponent registered; the missing "ui" node must not
	// throw and must leave the blob empty (defaulted).
	std::string loadedUi = "stale";
	{
		Scene loadedScene;
		auto loadedConfig = std::make_shared<RenderConfig>();
		ResourceManager loadedResources;
		project::Project p;
		p.Register<project::ResourceComponent>(loadedResources, catalog);
		p.Register<project::SceneComponent>(loadedScene, loadedResources);
		p.Register<project::ConfigComponent>(loadedConfig);
		p.Register<project::UIComponent>(loadedUi);
		EXPECT_NO_THROW(p.Load(tmp.path));
	}

	EXPECT_TRUE(loadedUi.empty());
}
