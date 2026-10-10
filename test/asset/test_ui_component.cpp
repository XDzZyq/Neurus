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

#include <cstdio>   // std::remove

#include "asset/Project.h"
#include "asset/components/UIComponent.h"
#include "asset/components/SceneComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/ConfigComponent.h"
#include "core/ResourceManager.h"
#include "core/RenderConfig.h"
#include "scene/Scene.h"

using namespace neurus;

namespace {

struct TempFile
{
	std::string path;
	explicit TempFile(std::string p) : path(std::move(p)) {}
	~TempFile() { std::remove(path.c_str()); }
};

} // namespace

// -----------------------------------------------------------------------
// Roundtrip: opaque UI blob preserved exactly
// -----------------------------------------------------------------------

TEST(UIComponentRoundtrip, PreservesBlob)
{
	TempFile tmp("test_rt_ui.neurus.json");

	// A blob containing newlines, base64 padding, and XML-ish content,
	// mimicking UIManager's packed geometry\ndockState format.
	const std::string blob = "AAAAgQ==\n<Docking><Central/></Docking>";

	{
		std::string ui = blob;
		project::Project p;
		p.Register<project::UIComponent>(ui);
		p.Save(tmp.path);
	}

	std::string loaded;
	{
		project::Project p;
		p.Register<project::UIComponent>(loaded);
		p.Load(tmp.path);
	}

	EXPECT_EQ(loaded, blob);
}

// -----------------------------------------------------------------------
// Roundtrip: empty blob
// -----------------------------------------------------------------------

TEST(UIComponentRoundtrip, EmptyBlob)
{
	TempFile tmp("test_rt_ui_empty.neurus.json");

	{
		std::string ui;   // empty
		project::Project p;
		p.Register<project::UIComponent>(ui);
		p.Save(tmp.path);
	}

	std::string loaded = "not-empty";
	{
		project::Project p;
		p.Register<project::UIComponent>(loaded);
		p.Load(tmp.path);
	}

	EXPECT_TRUE(loaded.empty());
}

// -----------------------------------------------------------------------
// Roundtrip: coexists with Scene + Config components
// -----------------------------------------------------------------------

TEST(UIComponentRoundtrip, CoexistsWithSceneAndConfig)
{
	TempFile tmp("test_rt_ui_full.neurus.json");

	const std::string blob = "GEOM==\nSTATE==";

	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;
		std::string ui = blob;
		project::Project p;
		p.Register<project::ResourceComponent>(resources);
		p.Register<project::SceneComponent>(scene, resources);
		p.Register<project::ConfigComponent>(config);
		p.Register<project::UIComponent>(ui);
		p.Save(tmp.path);
	}

	Scene loadedScene;
	RenderConfig loadedConfig;
	ResourceManager loadedResources;
	std::string loadedUi;
	{
		project::Project p;
		p.Register<project::ResourceComponent>(loadedResources);
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
	TempFile tmp("test_rt_ui_oldfmt.neurus.json");

	// Save a project WITHOUT a UIComponent (simulates an old file).
	{
		Scene scene;
		RenderConfig config;
		ResourceManager resources;
		project::Project p;
		p.Register<project::ResourceComponent>(resources);
		p.Register<project::SceneComponent>(scene, resources);
		p.Register<project::ConfigComponent>(config);
		p.Save(tmp.path);
	}

	// Load WITH a UIComponent registered; the missing "ui" node must not
	// throw and must leave the blob empty (defaulted).
	std::string loadedUi = "stale";
	{
		Scene loadedScene;
		RenderConfig loadedConfig;
		ResourceManager loadedResources;
		project::Project p;
		p.Register<project::ResourceComponent>(loadedResources);
		p.Register<project::SceneComponent>(loadedScene, loadedResources);
		p.Register<project::ConfigComponent>(loadedConfig);
		p.Register<project::UIComponent>(loadedUi);
		EXPECT_NO_THROW(p.Load(tmp.path));
	}

	EXPECT_TRUE(loadedUi.empty());
}
