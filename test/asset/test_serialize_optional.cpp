/**
 * @file test_serialize_optional.cpp
 * @brief Pins the per-field optional-load contract of core/Serialize.h.
 *
 * The motivating case is an *older* build opening a *newer* project or config:
 * a key it does not recognise, or a key the newer build renamed, must cost the
 * load that one field and nothing else. These tests pin the four halves of that
 * contract, in the order they matter:
 *
 *  1. The written format is unchanged. A round-trip alone cannot prove this —
 *     a typo'd NVP key saves and loads under the same wrong name and the
 *     round-trip still passes — so the saved JSON *text* is asserted to carry
 *     every expected key, and `use_shadow` to still precede `light_type`.
 *  2. An absent value field keeps the member at its in-class default, whether
 *     it sits in the middle of an object or at its end. This is also the proof
 *     that cereal's name-keyed lookup leaves the read cursor untouched on a
 *     miss, so field order on disk does not matter.
 *  3. The default is defined exactly once. Every "absent" assertion compares
 *     against a *default-constructed object*, never against a literal, so the
 *     test cannot drift from the member initializer the way a second copy of
 *     the value could.
 *  4. Identity and history stay strict. `UID::o_id`, a resource path, a scene
 *     object's discriminator and an operation's endpoint nodes still throw when
 *     missing, because defaulting them would silently corrupt cross-references
 *     or replay a wrong undo step.
 *
 * Absence is simulated by *renaming* a key rather than deleting its line: from
 * the reader's side a renamed key is absent, the document stays valid JSON
 * (deleting the last member of an object would strand a trailing comma), and
 * the same edit doubles as proof that an unknown field is ignored.
 *
 * All tests are pure CPU — no GPU, no Qt.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include <cereal/archives/json.hpp>

#include "app/Preferences.h"
#include "asset/Project.h"
#include "asset/components/ConfigComponent.h"
#include "asset/components/HistoryComponent.h"
#include "asset/components/ResourceComponent.h"
#include "asset/components/SceneComponent.h"
#include "asset/components/UIComponent.h"
#include "asset/data/ImageData.h"
#include "asset/data/MeshData.h"
#include "core/ResourceManager.h"
#include "core/Serialize.h"
#include "editor/events/EventBus.h"
#include "editor/operations/ConfigOperations.h"
#include "editor/operations/OperationManager.h"
#include "core/RenderConfig.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/GlmSerialization.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

using namespace neurus;

namespace {

/** @brief RAII temporary file: removed even when the test throws. */
struct ScratchFile
{
	std::string path;
	explicit ScratchFile(std::string p) : path(std::move(p)) {}
	~ScratchFile() { std::remove(path.c_str()); }
};

/** @brief Serializes one object under @p key and returns the JSON text. */
template<class T>
std::string SaveOne(const char* key, T& obj)
{
	std::ostringstream os;
	{
		cereal::JSONOutputArchive ar(os);
		ar(cereal::make_nvp(key, obj));
	}
	return os.str();
}

/** @brief Reads one object back out of @p json. Propagates cereal::Exception. */
template<class T>
void LoadOne(const std::string& json, const char* key, T& obj)
{
	std::istringstream is(json);
	cereal::JSONInputArchive ar(is);
	ar(cereal::make_nvp(key, obj));
}

/**
 * @brief Renames a JSON key in place so the reader sees the field as absent.
 *
 * Renaming rather than deleting keeps the document valid (the deleted member
 * might be the last in its object) and makes every "absent field" case also an
 * "unknown field" case, which is the same thing from the reader's side.
 *
 * @return false if the key was not found — the archive layout has drifted and
 *         the test is no longer testing what it claims.
 */
bool RenameKey(std::string& json, const std::string& key)
{
	const std::string from = "\"" + key + "\":";
	const auto        at   = json.find(from);
	if (at == std::string::npos)
		return false;
	json.replace(at, from.size(), "\"" + key + "_unknown_to_this_build\":");
	return true;
}

/** @brief Asserts @p json contains a `"key":` member, with the text on failure. */
void ExpectKey(const std::string& json, const char* key)
{
	EXPECT_NE(json.find("\"" + std::string(key) + "\":"), std::string::npos)
		<< "saved format lost its '" << key << "' key:\n" << json;
}

/** @brief Writes @p text verbatim to @p path (hand-authored legacy archives). */
void WriteText(const std::string& path, const std::string& text)
{
	std::ofstream os(path);
	ASSERT_TRUE(os.is_open()) << "cannot create " << path;
	os << text;
}

/** @brief Reads @p path whole, or returns an empty string if it cannot be opened. */
std::string ReadText(const std::string& path)
{
	std::ifstream     is(path);
	std::stringstream ss;
	ss << is.rdbuf();
	return ss.str();
}

/**
 * @brief Collects every member *name* in a JSON document.
 *
 * A name is a quoted string whose next non-space character is `:`, which is
 * what separates a key from a value without needing a parser — a value string
 * is always followed by `,` or a closing brace. Names, not paths: two objects
 * may legitimately share a member name, and the comparison this feeds only
 * asks whether a key the old file had is still written at all.
 */
std::set<std::string> KeyNames(const std::string& json)
{
	std::set<std::string> keys;
	for (size_t i = 0; (i = json.find('"', i)) != std::string::npos;)
	{
		const auto end = json.find('"', i + 1);
		if (end == std::string::npos)
			break;
		const auto after = json.find_first_not_of(" \t\r\n", end + 1);
		if (after != std::string::npos && json[after] == ':')
			keys.insert(json.substr(i + 1, end - i - 1));
		i = end + 1;
	}
	return keys;
}

} // namespace

// =======================================================================
// 1. The written format is unchanged
// =======================================================================

/**
 * @brief Every key the pre-optional format wrote is still written.
 *
 * A round-trip test cannot catch a mistyped NVP name: the save and the load
 * agree on the wrong spelling and the comparison passes, while every file
 * written by an earlier build silently loses that field. Only the saved text
 * can catch it, so this asserts on the text.
 */
TEST(SerializeOptional, SavedKeysAreUnchanged)
{
	Light             light;
	const std::string lightJson = SaveOne("light", light);
	for (const char* key : {"o_id", "o_name", "o_type", "is_viewport", "is_rendered",
	                        "transform", "m_position", "m_rotation", "m_scale",
	                        "use_shadow", "light_type", "light_power", "light_color",
	                        "light_radius", "spot_cutoff", "spot_outer_cutoff",
	                        "area_ratio"})
		ExpectKey(lightJson, key);

	// Light::serialize splits its call three ways purely to keep this order:
	// use_shadow is written before the strict light_type, as it always was.
	EXPECT_LT(lightJson.find("\"use_shadow\":"), lightJson.find("\"light_type\":"))
		<< "Light's key order changed:\n" << lightJson;

	Mesh              mesh;
	const std::string meshJson = SaveOne("mesh", mesh);
	for (const char* key : {"o_id", "o_name", "o_type", "is_viewport", "is_rendered",
	                        "transform", "m_position", "m_rotation", "m_scale",
	                        "o_meshDataId", "o_shaderId", "using_shadow",
	                        "using_material", "using_sdf", "is_closure"})
		ExpectKey(meshJson, key);

	Camera            camera;
	const std::string cameraJson = SaveOne("camera", camera);
	for (const char* key : {"o_id", "transform", "m_position", "cam_w", "cam_h",
	                        "cam_pers", "cam_near", "cam_far", "cam_tar",
	                        "cam_floatData"})
		ExpectKey(cameraJson, key);

	RenderConfig      config;
	const std::string configJson = SaveOne("proj_config", config);
	for (const char* key : {"r_pipeline", "r_aa", "r_ao", "r_shadow", "r_ssr",
	                        "r_gamma", "r_shadow_bias", "r_ao_ksize", "r_ao_radius",
	                        "r_sample_pf", "r_transparent", "r_fxaa_subpix",
	                        "r_fxaa_edge_threshold", "r_fxaa_edge_threshold_min",
	                        "r_sampling_mode", "r_debug_draw",
	                        "r_sampling_count"})
		ExpectKey(configJson, key);
}

// =======================================================================
// 2. Present fields still round-trip
// =======================================================================

/** @brief Mutates every optional field of a Light and reads them all back. */
TEST(SerializeOptional, RoundTripKeepsEveryField)
{
	Light light;
	light.o_name     = "KeyLight";
	light.is_viewport = false;
	light.is_rendered = false;
	light.SetPosition({1.0f, 2.0f, 3.0f});
	light.SetRotation({10.0f, 20.0f, 30.0f});
	light.SetScale({2.0f, 2.0f, 2.0f});
	light.use_shadow        = false;
	light.light_type        = SUNLIGHT;
	light.light_power       = 42.5f;
	light.light_color       = {0.25f, 0.5f, 0.75f};
	light.light_radius      = 0.123f;
	light.spot_cutoff       = 0.55f;
	light.spot_outer_cutoff = 0.44f;
	light.area_ratio        = 1.75f;

	const std::string json = SaveOne("light", light);

	Light loaded;
	ASSERT_NO_THROW(LoadOne(json, "light", loaded));

	EXPECT_EQ(loaded.o_name, "KeyLight");
	EXPECT_EQ(loaded.o_type, light.o_type);
	EXPECT_FALSE(loaded.is_viewport);
	EXPECT_FALSE(loaded.is_rendered);
	EXPECT_EQ(loaded.GetPosition(), light.GetPosition());
	EXPECT_EQ(loaded.GetRotation(), light.GetRotation());
	EXPECT_EQ(loaded.GetScale(), light.GetScale());
	EXPECT_FALSE(loaded.use_shadow);
	EXPECT_EQ(loaded.light_type, SUNLIGHT);
	EXPECT_FLOAT_EQ(loaded.light_power, 42.5f);
	EXPECT_EQ(loaded.light_color, light.light_color);
	EXPECT_FLOAT_EQ(loaded.light_radius, 0.123f);
	EXPECT_FLOAT_EQ(loaded.spot_cutoff, 0.55f);
	EXPECT_FLOAT_EQ(loaded.spot_outer_cutoff, 0.44f);
	EXPECT_FLOAT_EQ(loaded.area_ratio, 1.75f);
}

// =======================================================================
// 3. An absent value field keeps its one declared default
// =======================================================================

/**
 * @brief A missing field in the middle, at the end, and a missing sub-object.
 *
 * All three positions matter for different reasons: a miss in the middle proves
 * the failed name lookup left the read cursor where it was (so the fields after
 * it still resolve), a miss at the end proves nothing depends on the object
 * being fully consumed, and a missing `m_position` proves a whole nested node
 * can go absent without taking its siblings with it.
 *
 * Every expectation compares against `fresh`, a default-constructed Light, not
 * against a literal — the member initializer stays the only place the value is
 * written down.
 */
TEST(SerializeOptional, AbsentMiddleAndTrailingFieldsKeepDefaults)
{
	Light light;
	light.SetPosition({1.0f, 2.0f, 3.0f});
	light.SetRotation({10.0f, 20.0f, 30.0f});
	light.light_power  = 42.5f;
	light.light_radius = 0.123f;
	light.spot_cutoff  = 0.55f;
	light.area_ratio   = 1.75f;

	std::string json = SaveOne("light", light);
	ASSERT_TRUE(RenameKey(json, "light_radius")) << "stale anchor:\n" << json;
	ASSERT_TRUE(RenameKey(json, "area_ratio")) << "stale anchor:\n" << json;
	ASSERT_TRUE(RenameKey(json, "m_position")) << "stale anchor:\n" << json;

	const Light fresh;  // the single source of every default asserted below
	Light       loaded;
	ASSERT_NO_THROW(LoadOne(json, "light", loaded));

	// Absent: back to the in-class defaults.
	EXPECT_FLOAT_EQ(loaded.light_radius, fresh.light_radius);
	EXPECT_FLOAT_EQ(loaded.area_ratio, fresh.area_ratio);
	EXPECT_EQ(loaded.GetPosition(), fresh.GetPosition());

	// Present: untouched, including the field right after each hole.
	EXPECT_FLOAT_EQ(loaded.light_power, 42.5f);
	EXPECT_FLOAT_EQ(loaded.spot_cutoff, 0.55f);
	EXPECT_EQ(loaded.GetRotation(), light.GetRotation());
	EXPECT_EQ(loaded.GetScale(), fresh.GetScale());
}

/**
 * @brief Every optional field absent at once must reproduce a default object.
 *
 * This is the mechanical half of the one-source-of-truth claim in
 * core/Serialize.h: a Light with every value field set is saved, every optional
 * key is then renamed away, and the result is compared against a
 * default-constructed Light field by field. If a load path ever grows a second
 * copy of a default — a fallback argument, a hand-written reset — and that copy
 * disagrees with the member initializer, this fails.
 *
 * The archive is produced by a real save rather than hand-written, so the test
 * does not depend on the nesting cereal gives `base_class` (`o_id` actually
 * lives two "value0" nodes deep).
 */
TEST(SerializeOptional, EveryOptionalFieldAbsentEqualsDefaultConstructed)
{
	Light light;
	light.o_name            = "KeyLight";
	light.is_viewport       = false;
	light.is_rendered       = false;
	light.SetPosition({1.0f, 2.0f, 3.0f});
	light.SetRotation({10.0f, 20.0f, 30.0f});
	light.SetScale({2.0f, 2.0f, 2.0f});
	light.use_shadow        = false;
	light.light_type        = SUNLIGHT;
	light.light_power       = 42.5f;
	light.light_color       = {0.25f, 0.5f, 0.75f};
	light.light_radius      = 0.123f;
	light.spot_cutoff       = 0.55f;
	light.spot_outer_cutoff = 0.44f;
	light.area_ratio        = 1.75f;

	// Everything except the three strict keys: UID's o_id, the transform node
	// (its own contents are renamed below) and Light's discriminator.
	std::string json = SaveOne("light", light);
	for (const char* key : {"o_name", "o_type", "is_viewport", "is_rendered",
	                        "m_position", "m_rotation", "m_scale", "use_shadow",
	                        "light_power", "light_color", "light_radius",
	                        "spot_cutoff", "spot_outer_cutoff", "area_ratio"})
		ASSERT_TRUE(RenameKey(json, key)) << "stale anchor '" << key << "':\n" << json;

	const Light fresh;
	Light       loaded;
	ASSERT_NO_THROW(LoadOne(json, "light", loaded));

	EXPECT_EQ(loaded.o_name, fresh.o_name);
	EXPECT_EQ(loaded.o_type, fresh.o_type);
	EXPECT_EQ(loaded.is_viewport, fresh.is_viewport);
	EXPECT_EQ(loaded.is_rendered, fresh.is_rendered);
	EXPECT_EQ(loaded.GetPosition(), fresh.GetPosition());
	EXPECT_EQ(loaded.GetRotation(), fresh.GetRotation());
	EXPECT_EQ(loaded.GetScale(), fresh.GetScale());
	EXPECT_EQ(loaded.use_shadow, fresh.use_shadow);
	EXPECT_FLOAT_EQ(loaded.light_power, fresh.light_power);
	EXPECT_EQ(loaded.light_color, fresh.light_color);
	EXPECT_FLOAT_EQ(loaded.light_radius, fresh.light_radius);
	EXPECT_FLOAT_EQ(loaded.spot_cutoff, fresh.spot_cutoff);
	EXPECT_FLOAT_EQ(loaded.spot_outer_cutoff, fresh.spot_outer_cutoff);
	EXPECT_FLOAT_EQ(loaded.area_ratio, fresh.area_ratio);

	// The two keys the archive kept still land.
	EXPECT_EQ(loaded.GetObjectID(), light.GetObjectID());
	EXPECT_EQ(loaded.light_type, SUNLIGHT);
}

// =======================================================================
// 4. Identity and history stay strict
// =======================================================================

/**
 * @brief `UID::o_id` is never defaulted — a pool reference depends on it.
 *
 * Every cross-reference in a project (Scene's ID lists, Mesh::o_meshDataId,
 * the viewport camera's UID) is resolved by this number. Defaulting it would
 * produce an object that loads cleanly and then fails to resolve, which is far
 * worse to diagnose than a refused load.
 */
TEST(SerializeOptional, AbsentIdentityFieldStillThrows)
{
	Light       light;
	std::string json = SaveOne("light", light);
	ASSERT_TRUE(RenameKey(json, "o_id")) << "stale anchor:\n" << json;

	Light loaded;
	EXPECT_THROW(LoadOne(json, "light", loaded), cereal::Exception);
}

/** @brief A pooled resource's source path is identity too, not a setting. */
TEST(SerializeOptional, AbsentResourcePathStillThrows)
{
	MeshData    data;
	std::string json = SaveOne("mesh_data", data);
	ASSERT_TRUE(RenameKey(json, "m_path")) << "stale anchor:\n" << json;

	MeshData loaded;
	EXPECT_THROW(LoadOne(json, "mesh_data", loaded), cereal::Exception);
}

/**
 * @brief `light_type` discriminates point from sun, so it cannot be defaulted.
 *
 * `Light()` leaves it at NONELIGHT, which is a dead light the renderer skips —
 * defaulting it would turn a newer file's light into an invisible one with no
 * error anywhere.
 */
TEST(SerializeOptional, AbsentLightTypeStillThrows)
{
	Light light;
	light.light_type = POINTLIGHT;
	std::string json = SaveOne("light", light);
	ASSERT_TRUE(RenameKey(json, "light_type")) << "stale anchor:\n" << json;

	Light loaded;
	EXPECT_THROW(LoadOne(json, "light", loaded), cereal::Exception);
}

/**
 * @brief An undo entry's endpoint nodes stay strict.
 *
 * A half-read operation is worse than a refused one: it replays a `m_before`
 * nobody ever had, so undo walks the scene to a state that never existed. The
 * *contents* of an endpoint do follow RenderConfig's per-field rule — an older
 * build replaying a newer build's config op defaults the settings it does not
 * know, which is the intended graceful case and is pinned by
 * ConfigLoadDefaultsAbsentFields below. The node itself is not optional.
 */
TEST(SerializeOptional, OperationEndpointNodeStaysStrict)
{
	SetRenderConfigOp op(RenderConfig{}, RenderConfig{});
	std::string       json = SaveOne("op", op);
	ASSERT_TRUE(RenameKey(json, "before")) << "stale anchor:\n" << json;

	SetRenderConfigOp loaded;
	EXPECT_THROW(LoadOne(json, "op", loaded), cereal::Exception);
}

// =======================================================================
// 5. ConfigComponent: absent means "default", not "keep the live session"
// =======================================================================

/**
 * @brief A `proj_config` with one key leaves the rest at RenderConfig{}.
 *
 * The live RenderConfig here holds *session* values, deliberately different
 * from the defaults. Reading straight into it would make "absent" mean "inherit
 * whatever the last project the user had open set", which is why
 * ConfigComponent::Load goes through `archive::LoadFresh`. This pins that: the
 * single supplied key lands, everything else equals a default-constructed
 * config rather than the session values.
 */
TEST(SerializeOptional, ConfigLoadDefaultsAbsentFields)
{
	ScratchFile tmp("test_serialize_optional_config.neurus.json");
	WriteText(tmp.path, R"({
	"project": {
		"proj_config": {
			"r_gamma": 2.5
		}
	}
})");

	RenderConfig live;
	live.r_gamma       = 9.0f;   // all three are session values that must NOT
	live.r_ao_ksize    = 99;     // survive a load that does not mention them
	live.r_transparent = true;

	project::Project p;
	p.Register<project::ConfigComponent>(live);
	ASSERT_NO_THROW(p.Load(tmp.path));

	const RenderConfig fresh;
	EXPECT_FLOAT_EQ(live.r_gamma, 2.5f);
	EXPECT_EQ(live.r_ao_ksize, fresh.r_ao_ksize);
	EXPECT_EQ(live.r_transparent, fresh.r_transparent);
	EXPECT_EQ(live.r_shadow, fresh.r_shadow);
	EXPECT_FLOAT_EQ(live.r_shadow_bias, fresh.r_shadow_bias);
	EXPECT_EQ(live.r_debug_draw, fresh.r_debug_draw);
}

/**
 * @brief No `proj_config` node at all is a fully defaulted config, not a throw.
 *
 * This is `LoadFresh`'s false branch: a project written before the component
 * existed still opens, with every setting at its declared default and one
 * reported line explaining why.
 */
TEST(SerializeOptional, ConfigWithoutNodeIsFullyDefaulted)
{
	ScratchFile tmp("test_serialize_optional_noconfig.neurus.json");
	WriteText(tmp.path, R"({
	"project": {}
})");

	RenderConfig live;
	live.r_gamma       = 9.0f;
	live.r_transparent = true;

	project::Project p;
	p.Register<project::ConfigComponent>(live);
	ASSERT_NO_THROW(p.Load(tmp.path));

	EXPECT_TRUE(live == RenderConfig{})
		<< "an absent proj_config node must default the whole config";
}

// =======================================================================
// 6. What a *newer* file adds is ignored, wherever it sits
// =======================================================================

/**
 * @brief An unknown field is skipped, before, between and after known ones.
 *
 * This is the half of forward compatibility cereal already gave us — on input,
 * finishing a node only pops it, with no check that every member was consumed.
 * The test pins it so a cereal upgrade that started enforcing consumption could
 * not land silently.
 */
TEST(SerializeOptional, UnknownFieldIsIgnored)
{
	const std::string json = R"({
	"proj_config": {
		"r_future_leading": 1,
		"r_gamma": 2.5,
		"r_future_middle": {"nested": [1, 2, 3]},
		"r_ao_ksize": 8,
		"r_future_trailing": "text"
	}
})";

	const RenderConfig fresh;
	RenderConfig       loaded;
	ASSERT_NO_THROW(LoadOne(json, "proj_config", loaded));

	EXPECT_FLOAT_EQ(loaded.r_gamma, 2.5f);
	EXPECT_EQ(loaded.r_ao_ksize, 8);
	EXPECT_FLOAT_EQ(loaded.r_shadow_bias, fresh.r_shadow_bias);
}

/**
 * @brief A whole top-level component this build does not know is skipped.
 *
 * The same property one level up: a newer build that adds a component writes an
 * extra node into "project", and an older build's registered components each
 * find their own node by name regardless of what sits before it.
 */
TEST(SerializeOptional, UnknownTopLevelComponentIsIgnored)
{
	ScratchFile tmp("test_serialize_optional_unknown.neurus.json");
	WriteText(tmp.path, R"({
	"project": {
		"proj_future_component": {
			"something": 1
		},
		"proj_config": {
			"r_gamma": 2.5
		}
	}
})");

	RenderConfig     live;
	project::Project p;
	p.Register<project::ConfigComponent>(live);
	ASSERT_NO_THROW(p.Load(tmp.path));

	EXPECT_FLOAT_EQ(live.r_gamma, 2.5f);
}

// =======================================================================
// 7. Preferences: the same rule, outside the project file
// =======================================================================

/**
 * @brief One key present, one absent: the present one lands, the other defaults.
 *
 * Preferences::Load reads each key independently into a default-constructed
 * Preferences, so a file written by a build that renamed `target_fps` still
 * restores the language instead of reporting Corrupt and losing both.
 */
TEST(SerializeOptional, PreferencesKeepsPresentKeyWhenOtherAbsent)
{
	ScratchFile tmp("test_serialize_optional_prefs.json");
	WriteText(tmp.path, R"({
	"language": "zh_CN"
})");

	Preferences prefs;
	prefs.language  = "en";  // session values: neither may survive as a default
	prefs.targetFps = 1;

	EXPECT_EQ(prefs.Load(tmp.path), Preferences::LoadResult::Ok);
	EXPECT_EQ(prefs.language, "zh_CN");

	const Preferences fresh;
	EXPECT_EQ(prefs.targetFps, fresh.targetFps);
}

/**
 * @brief A file with no key this build recognises is Ok with defaults.
 *
 * It parses, so it is not Corrupt, and it exists, so it is not Missing — the
 * caller must not treat it as either. Every field comes from the member
 * initializers, compared here against a default-constructed Preferences.
 */
TEST(SerializeOptional, PreferencesWithNoKnownKeyIsOkWithDefaults)
{
	ScratchFile tmp("test_serialize_optional_prefs_future.json");
	WriteText(tmp.path, R"({
	"theme": "dark",
	"enable_cuda": true
})");

	Preferences prefs;
	prefs.language  = "en";
	prefs.targetFps = 1;

	EXPECT_EQ(prefs.Load(tmp.path), Preferences::LoadResult::Ok);

	const Preferences fresh;
	EXPECT_EQ(prefs.language, fresh.language);
	EXPECT_EQ(prefs.targetFps, fresh.targetFps);
}

/** @brief A file that is not JSON at all is still Corrupt, and changes nothing. */
TEST(SerializeOptional, PreferencesCorruptFileKeepsLiveValues)
{
	ScratchFile tmp("test_serialize_optional_prefs_corrupt.json");
	WriteText(tmp.path, "{ this is not json");

	Preferences prefs;
	prefs.language  = "en";
	prefs.targetFps = 144;

	EXPECT_EQ(prefs.Load(tmp.path), Preferences::LoadResult::Corrupt);
	EXPECT_EQ(prefs.language, "en");
	EXPECT_EQ(prefs.targetFps, 144);
}

// =======================================================================
// 8. A real file written by an earlier build survives a write round trip
// =======================================================================

/**
 * @test Re-saving the shipped project drops none of the keys it arrived with.
 *
 * Every other test here builds its archive in memory, so none of them can tell
 * whether a *file on disk* — `res/default.neurus.json`, written before any of
 * this existed and still missing `r_debug_draw` — comes back out intact. That
 * is the whole backward-compatibility claim: open an older project, save it,
 * and the file is still the same format rather than one quietly short a field.
 *
 * The comparison is one-directional on purpose. The new file is allowed to have
 * *more* keys (it gains `r_debug_draw`, and `activeCamUid` if the old file
 * predates it); it may not have fewer. EditorComponent is deliberately left
 * unregistered — the shipped file has no `m_editor` node, so registering it
 * would only add a key to the right-hand side.
 */
TEST(SerializeOptional, ShippedProjectResavesWithEveryOldKey)
{
	const std::string src = std::string(TEST_SOURCE_DIR) + "/res/default.neurus.json";
	const std::string before = ReadText(src);
	ASSERT_FALSE(before.empty()) << "cannot read " << src;

	Scene            scene;
	RenderConfig     config;
	ResourceManager  resources;
	std::string      ui;
	EventQueue       bus;
	OperationManager ops(bus);

	ScratchFile out("test_serialize_optional_resave.neurus.json");
	{
		project::Project p;
		p.Register<project::ResourceComponent>(resources);
		p.Register<project::SceneComponent>(scene, resources);
		p.Register<project::ConfigComponent>(config);
		p.Register<project::UIComponent>(ui);
		p.Register<project::HistoryComponent>(ops);
		ASSERT_NO_THROW(p.Load(src));
		ASSERT_NO_THROW(p.Save(out.path));
	}

	const std::string after = ReadText(out.path);
	ASSERT_FALSE(after.empty()) << "nothing was written to " << out.path;

	const auto oldKeys = KeyNames(before);
	const auto newKeys = KeyNames(after);
	ASSERT_FALSE(oldKeys.empty());
	for (const auto& key : oldKeys)
		EXPECT_TRUE(newKeys.count(key) != 0)
			<< "re-saving the shipped project dropped the key '" << key << "'";
}










