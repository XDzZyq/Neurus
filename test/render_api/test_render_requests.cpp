#include "render_api/RenderRequests.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>

namespace neurus {

TEST(RenderRequestValues, LightSnapshotSurvivesSourceEditsAndDeletion)
{
	auto light = std::make_shared<Light>(LightType::SPOTLIGHT, 7.0f, glm::vec3(0.2f, 0.4f, 0.6f));
	light->SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	light->spot_cutoff = 0.75f;
	light->spot_outer_cutoff = 0.5f;
	light->use_shadow = false;
	const int uid = light->GetObjectID();
	const UpdateLightRequest request{uid, MakeLightSnapshot(*light)};

	light->SetPosition(glm::vec3(9.0f));
	light->light_power = 99.0f;
	light.reset();

	EXPECT_EQ(request.light.objectId, uid);
	EXPECT_EQ(request.light.type, LightType::SPOTLIGHT);
	EXPECT_EQ(request.light.position, glm::vec3(1.0f, 2.0f, 3.0f));
	EXPECT_EQ(request.light.color, glm::vec3(0.2f, 0.4f, 0.6f));
	EXPECT_FLOAT_EQ(request.light.power, 7.0f);
	EXPECT_FLOAT_EQ(request.light.spotCutoff, 0.75f);
	EXPECT_FLOAT_EQ(request.light.spotOuterCutoff, 0.5f);
	EXPECT_FALSE(request.light.useShadow);
}

TEST(RenderRequestValues, LightingBatchOnlyIncludesVisibleRenderedLights)
{
	auto visible = std::make_shared<Light>(LightType::SUNLIGHT, 2.0f, glm::vec3(1.0f));
	auto hidden = std::make_shared<Light>();
	hidden->is_viewport = false;
	auto excluded = std::make_shared<Light>();
	excluded->is_rendered = false;
	const std::unordered_map<int, std::shared_ptr<Light>> lights = {
		{visible->GetObjectID(), visible},
		{hidden->GetObjectID(), hidden},
		{excluded->GetObjectID(), excluded},
		{-1, nullptr},
	};
	const UpdateLightingRequest request = MakeUpdateLightingRequest(lights);
	ASSERT_EQ(request.lights.size(), 1u);
	EXPECT_EQ(request.lights.at(visible->GetObjectID()).type, LightType::SUNLIGHT);
}

} // namespace neurus
