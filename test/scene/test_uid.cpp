/**
 * @file test_uid.cpp
 * @brief Tests for ObjectID (scene identity + metadata base class).
 *
 * Covers:
 * - ObjectID default construction (name, type, visibility)
 * - UID identity inheritance
 * - SetVisible / polymorphic accessors
 * - Name and type assignment
 * - Mixed UID/ObjectID counter increments
 * - Deleted copy and move on every UID-derived identity type (compile time)
 */

#include <gtest/gtest.h>
#include <asset/data/ImageData.hpp>
#include <asset/data/MeshData.hpp>
#include <asset/data/ShaderData.hpp>
#include <core/UID.hpp>
#include <scene/Camera.hpp>
#include <scene/DebugLine.hpp>
#include <scene/DebugMesh.hpp>
#include <scene/DebugPoints.hpp>
#include <scene/Environment.hpp>
#include <scene/Light.hpp>
#include <scene/Mesh.hpp>
#include <scene/ObjectID.hpp>
#include <scene/Scene.hpp>
#include <scene/Sprite.hpp>

#include <type_traits>

// --- ObjectID Tests ------------------------------------------------------

/**
 * @test ObjectID default-constructs with expected defaults.
 */
TEST(ObjectIDTest, DefaultConstruction)
{
	neurus::ObjectID obj;

	// Default name should be empty
	EXPECT_TRUE(obj.o_name.empty());

	// Default type should be NONE_GO
	EXPECT_EQ(obj.o_type, neurus::ObjectID::GOType::NONE_GO);

	// Default visibility flags
	EXPECT_TRUE(obj.is_viewport);
	EXPECT_TRUE(obj.is_rendered);
}

/**
 * @test ObjectID inherits valid UID.
 */
TEST(ObjectIDTest, InheritsValidUID)
{
	neurus::ObjectID obj;
	EXPECT_GE(obj.GetObjectID(), 0);
}

/**
 * @test SetVisible modifies both flags.
 */
TEST(ObjectIDTest, SetVisible)
{
	neurus::ObjectID obj;
	obj.SetVisible(false, false);
	EXPECT_FALSE(obj.is_viewport);
	EXPECT_FALSE(obj.is_rendered);

	obj.SetVisible(true, false);
	EXPECT_TRUE(obj.is_viewport);
	EXPECT_FALSE(obj.is_rendered);
}

/**
 * @test Virtual accessors return nullptr by default.
 */
TEST(ObjectIDTest, VirtualAccessorsReturnNull)
{
	neurus::ObjectID obj;
	EXPECT_EQ(obj.GetShader(), nullptr);
	EXPECT_EQ(obj.GetTransform(), nullptr);
	EXPECT_EQ(obj.GetMaterial(), nullptr);
}

/**
 * @test ObjectID can have name and type assigned.
 */
TEST(ObjectIDTest, AssignedNameAndType)
{
	neurus::ObjectID obj;
	obj.o_name = "TestCamera";
	obj.o_type = neurus::ObjectID::GOType::GO_CAM;

	EXPECT_EQ(obj.o_name, "TestCamera");
	EXPECT_EQ(obj.o_type, neurus::ObjectID::GOType::GO_CAM);
}

/**
 * @test UID counter increments across UID and ObjectID instances.
 */
TEST(ObjectIDTest, CounterIncrementsMixed)
{
	int before = neurus::UID::GetTotalAllocated();

	{
		neurus::UID a;
		neurus::ObjectID b;
		neurus::UID c;
		(void)a;
		(void)b;
		(void)c;
	}

	int after = neurus::UID::GetTotalAllocated();
	EXPECT_GE(after, before + 3);
}

// --- Identity Special Members -------------------------------------------

namespace
{

/**
 * @brief Compile-time pin: an identity type carries no value semantics.
 *
 * Identity is owned by the ResourceManager pool, so duplication is Load<T>
 * plus an explicit field copy (Editor::DuplicateObject) - the one path that
 * allocates a new id. A `= default` on any of the four operations would let an
 * id be transferred or duplicated; every consumer that already holds that id
 * (scene references, undo records, GPU caches, the pool's own indexes) would
 * then name a different object.
 *
 * @tparam T Type expected to delete copy and move.
 */
template<typename T>
void ExpectIdentityOnly()
{
	static_assert(!std::is_copy_constructible_v<T>, "identity type must not be copy constructible");
	static_assert(!std::is_copy_assignable_v<T>, "identity type must not be copy assignable");
	static_assert(!std::is_move_constructible_v<T>, "identity type must not be move constructible");
	static_assert(!std::is_move_assignable_v<T>, "identity type must not be move assignable");
}

} // namespace

/**
 * @test Every UID-derived type deletes copy and move.
 *
 * Fails to compile, rather than at run time, if a future scene or data type
 * gains value semantics.
 */
TEST(ObjectIDTest, IdentityIsNeverCopiedOrMoved)
{
	ExpectIdentityOnly<neurus::UID>();
	ExpectIdentityOnly<neurus::ObjectID>();
	ExpectIdentityOnly<neurus::Camera>();
	ExpectIdentityOnly<neurus::Mesh>();
	ExpectIdentityOnly<neurus::Light>();
	ExpectIdentityOnly<neurus::Environment>();
	ExpectIdentityOnly<neurus::Sprite>();
	ExpectIdentityOnly<neurus::DebugLine>();
	ExpectIdentityOnly<neurus::DebugMesh>();
	ExpectIdentityOnly<neurus::DebugPoints>();
	ExpectIdentityOnly<neurus::Scene>();
	ExpectIdentityOnly<neurus::MeshData>();
	ExpectIdentityOnly<neurus::ImageData>();
	ExpectIdentityOnly<neurus::ShaderData>();
}

