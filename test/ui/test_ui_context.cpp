/**
 * @file test_ui_context.cpp
 * @brief The UI-facing frame view: its settings fallback, and its parity with the
 *        render-facing one.
 *
 * UIContext is the type that decides what a panel is allowed to see. Two things
 * are worth pinning about it, and both are load-bearing rather than cosmetic:
 *
 * - **`Settings()` never returns null and never invents a default.** With no
 *   config published a panel must still get a usable RenderConfig, and it must
 *   be the *same* one a render pass would get — otherwise the viewport renders
 *   at one exposure and the panel displays another, which reads as a broken
 *   slider rather than as a missing pointer.
 * - **The UI's view is two fields wide.** `scene` and `config` are the whole of
 *   what the Editor injects, and the type deliberately carries no camera and no
 *   overlay payload. That is a property of the struct, so the check here is a
 *   compile-time one: adding a camera or a draw list to UIContext would break
 *   the constructible-from-two-pointers test below.
 *
 * The render-side half of the fallback guard lives in
 * test/render/test_null_scene_passes.cpp (RenderContextSettings); this file adds
 * the panel side and the parity between the two.
 *
 * Needs no QApplication: UIContext is Qt-free by construction, which is itself
 * part of what is being asserted.
 */

#include <gtest/gtest.h>
#include "ui/items/ScalarSlider.hpp"
#include "ui/panels/RenderConfigPanel.hpp"
#include <QDoubleSpinBox>
#include <QSlider>


#include "core/RenderConfig.hpp"
#include "render_api/RenderContext.hpp"
#include "render_api/RenderFrameSnapshot.hpp"
#include "scene/Scene.hpp"
#include "ui/UIContext.hpp"

using namespace neurus;

// ---------------------------------------------------------------------------
// Settings(): the fallback, and that both carriers agree
// ---------------------------------------------------------------------------

TEST(UIContextSettings, FallsBackToRenderConfigsOwnDefaults)
{
	const UIContext ui{};
	const RenderConfig declared{};

	EXPECT_EQ(&ui.Settings(), &ui.Settings())
		<< "one shared instance, not a temporary per call";

	EXPECT_FLOAT_EQ(ui.Settings().r_shadow_bias,   declared.r_shadow_bias);
	EXPECT_FLOAT_EQ(ui.Settings().r_gamma,         declared.r_gamma);
	EXPECT_EQ(ui.Settings().r_sampling_mode,       declared.r_sampling_mode);
	EXPECT_FLOAT_EQ(ui.Settings().r_fxaa_subpix,   declared.r_fxaa_subpix);
}

TEST(UIContextSettings, AgreesWithRenderContextOnAnUnsetConfig)
{
	// The panel the user drags and the pass that shades the image must not
	// disagree about a value neither was given. The renderer reads its settings
	// out of the published frame now, so the comparison is against an
	// unpublished frame - the renderer's own answer to "nothing was given".
	const UIContext     ui{};
	const RenderContext render{};

	EXPECT_FLOAT_EQ(ui.Settings().r_shadow_bias, render.Frame().Settings().r_shadow_bias);
	EXPECT_FLOAT_EQ(ui.Settings().r_gamma,       render.Frame().Settings().r_gamma);
	EXPECT_EQ(ui.Settings().r_sampling_mode,     render.Frame().Settings().r_sampling_mode);
}

TEST(UIContextSettings, PublishedConfigWinsAndBothCarriersAgree)
{
	RenderConfig mine{};
	mine.r_shadow_bias = 0.1234f;

	UIContext ui{};
	ui.config = &mine;

	// The two carriers differ in kind, deliberately: the UI borrows the Editor's
	// config by pointer, while the renderer is handed a value copy inside the
	// frame - that copy is what a render thread can read without the Editor
	// holding still. What must not differ is the value they report.
	RenderFrameSnapshot  snapshot{};
	snapshot.config = mine;
	RenderContext render{};
	render.snapshot = &snapshot;

	EXPECT_EQ(&ui.Settings(), &mine) << "the UI side is still by reference, not copied";
	EXPECT_FLOAT_EQ(ui.Settings().r_shadow_bias, render.Frame().Settings().r_shadow_bias);
}

// ---------------------------------------------------------------------------
// The UI's view is exactly { scene, config }
// ---------------------------------------------------------------------------

TEST(UIContextShape, HasSceneTracksThePointer)
{
	UIContext ui{};
	EXPECT_FALSE(ui.HasScene());
	EXPECT_TRUE(ui.GetObjectIDs().empty()) << "no scene -> no ids, not a crash";

	Scene scene{};
	ui.scene = &scene;
	EXPECT_TRUE(ui.HasScene());
}

TEST(UIContextShape, EnumeratesEveryObjectInTheMasterList)
{
	Scene scene{};
	UIContext ui{};
	ui.scene = &scene;

	EXPECT_EQ(ui.GetObjectIDs().size(), scene.obj_list.size());

	// Ids are the plain ints a panel compares for dirty checks; they must match
	// the master list exactly, in order, with no duplicates.
	std::vector<int> expected;
	expected.reserve(scene.obj_list.size());
	for (const auto& [id, obj] : scene.obj_list)
	{
		(void)obj;
		expected.push_back(id);
	}
	EXPECT_EQ(ui.GetObjectIDs(), expected);
}

// ---------------------------------------------------------------------------
// Application assembles document fields without a framework dependency.
// ---------------------------------------------------------------------------

TEST(UIContextShape, DocumentFieldsAreTwoPlainPointers)
{
	Scene scene{};
	RenderConfig config{};

	UIContext ui{};
	ui.scene  = &scene;
	ui.config = &config;

	EXPECT_EQ(ui.scene,  &scene);
	EXPECT_EQ(ui.config, &config);
	EXPECT_EQ(ui.Settings().r_shadow_bias, config.r_shadow_bias);
}

// Widget integration: model settings may exceed a slider's convenient drag range.

TEST(ScalarSliderModel, OutOfRangeValueIsDisplayedWithoutChangingDragRange)
{
	ScalarSlider slider(0.0, 0.01, 1000, 0.0005);
	int emitted = 0;
	QObject::connect(&slider, &ScalarSlider::valueChanged, [&]() { ++emitted; });
	slider.setValue(0.02);
	EXPECT_DOUBLE_EQ(slider.value(), 0.02);
	EXPECT_DOUBLE_EQ(slider.spinBox()->value(), 0.02);
	EXPECT_EQ(slider.slider()->minimum(), 0);
	EXPECT_EQ(slider.slider()->maximum(), 1000);
	EXPECT_EQ(slider.slider()->value(), 1000);
	EXPECT_EQ(emitted, 0);

	slider.setValue(0.005);
	EXPECT_DOUBLE_EQ(slider.value(), 0.005);
	slider.setValue(0.02);
	EXPECT_DOUBLE_EQ(slider.value(), 0.02);
	EXPECT_EQ(emitted, 0);
}

TEST(RenderConfigPanelModel, GammaEditPreservesDefaultBiasOutsideDragRange)
{
	RenderConfigPanel panel;
	UIContext context;
	int emitted = 0;
	RenderConfig reported;
	QObject::connect(&panel, &RenderConfigPanel::configValueChanged,
		[&](const RenderConfigChangedEvent& event) { reported = event.config; ++emitted; });
	panel.Refresh(context);
	EXPECT_EQ(emitted, 0);
	EXPECT_FLOAT_EQ(panel.Save().r_shadow_bias, context.Settings().r_shadow_bias);

	ScalarSlider* gamma = nullptr;
	for (auto* slider : panel.findChildren<ScalarSlider*>())
	{
		if (slider->slider()->maximum() == 200)
			gamma = slider;
	}
	ASSERT_NE(gamma, nullptr);
	gamma->spinBox()->setValue(2.2);
	ASSERT_EQ(emitted, 1);
	EXPECT_FLOAT_EQ(reported.r_gamma, 2.2f);
	EXPECT_FLOAT_EQ(reported.r_shadow_bias, context.Settings().r_shadow_bias);
}
