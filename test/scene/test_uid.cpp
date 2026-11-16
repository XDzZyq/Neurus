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
 */

#include <gtest/gtest.h>
#include <core/UID.hpp>
#include <scene/ObjectID.hpp>

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
