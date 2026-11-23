/**
 * @file test_transform.cpp
 * @brief Unit tests for Transform base and Transform3D classes.
 *
 * TDD: RED (test written first) → GREEN (implementation verified).
 * All tests are pure CPU math - no GPU required.
 */

#include <gtest/gtest.h>

#include <sstream>

#include <cereal/archives/json.hpp>

#include "scene/Transform.hpp"

using namespace neurus;

// -----------------------------------------------------------------------
// Transform base class
// -----------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<Transform>);
static_assert(!std::is_copy_assignable_v<Transform>);
static_assert(!std::is_copy_constructible_v<Transform3D>);
static_assert(!std::is_copy_assignable_v<Transform3D>);

TEST(Transform3D, TranslateOnly)
{
	Transform3D t;
	Transform& base = t;
	EXPECT_EQ(base.GetTransformPtr(), &t);
	EXPECT_EQ(t.GetModelMatrix(), glm::mat4(1.0f));
	t.SetPosition(glm::vec3(5.0f, 10.0f, -3.0f));

	glm::mat4 model = t.GetModelMatrix();

	// Translation is stored in column 3 (the w column) in column-major
	// M[3][0] = x, M[3][1] = y, M[3][2] = z
	EXPECT_FLOAT_EQ(model[3][0], 5.0f);
	EXPECT_FLOAT_EQ(model[3][1], 10.0f);
	EXPECT_FLOAT_EQ(model[3][2], -3.0f);

	// Rotation/scale part (upper-left 3x3) should remain identity
	EXPECT_EQ(glm::mat3(model), glm::mat3(1.0f));
	t.SetPosition(glm::vec3(-1.0f, 2.0f, 3.0f));
	EXPECT_EQ(glm::vec3(t.GetModelMatrix()[3]), glm::vec3(-1.0f, 2.0f, 3.0f));
}

TEST(Transform3D, QuarterTurnsRotateTheBasisVectors)
{
	struct Rotation { glm::vec3 degrees; glm::mat3 expected; };
	const Rotation cases[] = {
		{{90, 0, 0}, {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}},
		{{0, 90, 0}, {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}}
	};
	for (const auto& rotation : cases)
	{
		SCOPED_TRACE(::testing::Message() << "pitch " << rotation.degrees.x << ", roll " << rotation.degrees.y);
		Transform3D transform;
		transform.SetRotation(rotation.degrees);
		const auto matrix = transform.GetModelMatrix();
		for (int column = 0; column < 3; ++column)
			for (int row = 0; row < 3; ++row)
				EXPECT_NEAR(matrix[column][row], rotation.expected[column][row], 1e-6f);
	}
}

// -----------------------------------------------------------------------
// Transform3D - Normal matrix
// -----------------------------------------------------------------------

/**
 * @brief Normal matrix matches inverse-transpose of upper-left 3x3 model matrix.
 */
TEST(Transform3D, NormalMatrix)
{
	Transform3D t;
	t.SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	t.SetRotation(glm::vec3(45.0f, 30.0f, 15.0f));
	t.SetScale(glm::vec3(2.0f, 1.0f, 3.0f));

	glm::mat3 normalMat = t.GetNormalMatrix();
	glm::mat3 expected = glm::transpose(glm::inverse(glm::mat3(t.GetModelMatrix())));

	for (int col = 0; col < 3; ++col)
	{
		for (int row = 0; row < 3; ++row)
		{
			EXPECT_FLOAT_EQ(normalMat[col][row], expected[col][row]);
		}
	}
}

// -----------------------------------------------------------------------
// Transform3D - Serialization
// -----------------------------------------------------------------------

/**
 * @brief A deserialized transform rebuilds its cached model matrix.
 *
 * o_modelMatrix is a computed value, not a serialized one, and the setters -
 * the only place it is otherwise recomputed - are bypassed by deserialization.
 * A loaded transform therefore used to keep the identity matrix it was
 * constructed with: a scene reloaded from a project file rendered every object
 * at the origin with unit scale while the Property panel showed the TRS that
 * had been loaded correctly.
 */
TEST(Transform3D, Load_RecomputesTheCachedModelMatrix)
{
	Transform3D original;
	original.SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
	original.SetRotation(glm::vec3(0.0f, 0.0f, 90.0f)); // yaw
	original.SetScale(glm::vec3(2.0f));

	std::ostringstream out;
	{
		cereal::JSONOutputArchive ar(out);
		ar(cereal::make_nvp("transform", original));
	}

	Transform3D loaded;
	ASSERT_EQ(loaded.GetModelMatrix(), glm::mat4(1.0f)); // identity before the load

	{
		std::istringstream in(out.str());
		cereal::JSONInputArchive ar(in);
		ar(cereal::make_nvp("transform", loaded));
	}

	EXPECT_EQ(loaded.GetPosition(), original.GetPosition());
	EXPECT_EQ(loaded.GetRotation(), original.GetRotation());
	EXPECT_EQ(loaded.GetScale(), original.GetScale());
	EXPECT_EQ(loaded.GetModelMatrix(), original.GetModelMatrix())
	    << "a loaded transform must not keep the identity matrix it was built with";
}

// -----------------------------------------------------------------------
// Transform3D - Combined TRS
// -----------------------------------------------------------------------

/**
 * @brief Full TRS composition with yaw=90° (rotation around Z-up axis).
 *
 * SetPosition(10,20,30), SetRotation(0,0,90) = pitch=0 roll=0 yaw=90°,
 * SetScale(2,2,2).
 *
 * Model matrix = T * Rz(90°) * S:
 * - Column 0 (scaled X): Rz(90°)*(2,0,0) = (0, 2, 0)
 * - Column 1 (scaled Y): Rz(90°)*(0,2,0) = (-2, 0, 0)
 * - Column 2 (scaled Z): Rz(90°)*(0,0,2) = (0, 0, 2)
 * - Translation: (10, 20, 30)
 */
TEST(Transform3D, FullTRS)
{
	Transform3D t;
	t.SetPosition(glm::vec3(10.0f, 20.0f, 30.0f));
	t.SetRotation(glm::vec3(0.0f, 0.0f, 90.0f)); // pitch=0, roll=0, yaw=90°
	t.SetScale(glm::vec3(2.0f, 0.5f, 3.0f));

	glm::mat4 model = t.GetModelMatrix();

	// Column 0 (X axis) - rotates to +Y: (0, 2, 0)
	EXPECT_NEAR(model[0][0], 0.0f, 1e-6f);
	EXPECT_FLOAT_EQ(model[0][1], 2.0f);
	EXPECT_NEAR(model[0][2], 0.0f, 1e-6f);

	// Column 1 (Y axis) - rotates to -X: (-0.5, 0, 0)
	EXPECT_FLOAT_EQ(model[1][0], -0.5f);
	EXPECT_NEAR(model[1][1], 0.0f, 1e-6f);
	EXPECT_NEAR(model[1][2], 0.0f, 1e-6f);

	// Column 2 (Z axis) - unchanged: (0, 0, 3)
	EXPECT_NEAR(model[2][0], 0.0f, 1e-6f);
	EXPECT_NEAR(model[2][1], 0.0f, 1e-6f);
	EXPECT_FLOAT_EQ(model[2][2], 3.0f);

	// Translation: [10, 20, 30]
	EXPECT_FLOAT_EQ(model[3][0], 10.0f);
	EXPECT_FLOAT_EQ(model[3][1], 20.0f);
	EXPECT_FLOAT_EQ(model[3][2], 30.0f);
}

// -----------------------------------------------------------------------
// Transform3D - GetDirection
// -----------------------------------------------------------------------

/**
 * @brief Helper: compute Euler angles (pitch, roll, yaw) from a forward
 *        direction vector, matching the Rz*Rx*Ry rotation order used by
 *        Transform3D with Z-up coordinates and forward = (0, 1, 0).
 *
 * Derivation (from GetDirection()):
 *   d = Rz(yaw) * Rx(pitch) * Ry(roll) * (0, 1, 0)
 *   Ry(roll) has no effect on the Y-axis forward vector, so:
 *   d = Rz(yaw) * Rx(pitch) * (0, 1, 0)
 *     = Rz(yaw) * (0, cos(pitch), sin(pitch))
 *   d.x = -cos(pitch) * sin(yaw)
 *   d.y =  cos(pitch) * cos(yaw)
 *   d.z =  sin(pitch)
 *
 * Solving:
 *   pitch = asin(d.z)
 *   yaw   = atan2(-d.x, d.y)
 *
 * @param dir     Normalized forward direction vector.
 * @param rollDeg Optional roll angle in degrees (default 0).
 * @return glm::vec3 of (pitch, roll, yaw) in degrees.
 *         x = pitch (X), y = roll (Y), z = yaw (Z).
 */
static glm::vec3 EulerFromDirection(const glm::vec3& dir, float rollDeg = 0.0f)
{
	const float pitchRad = std::asin(dir.z);
	const float yawRad = std::atan2(-dir.x, dir.y);
	return glm::vec3(glm::degrees(pitchRad), rollDeg, glm::degrees(yawRad));
}

/**
 * @brief GetDirection returns the normalized local-space forward vector (0,1,0)
 *        rotated by the current Euler angles.
 *
 * This test verifies GetDirection() by:
 * 1. Setting a target point and computing the ground-truth direction.
 * 2. Converting that direction to Euler angles via EulerFromDirection().
 * 3. Setting those angles on the transform via SetRotation().
 * 4. Comparing GetDirection() against the ground-truth direction.
 */

TEST(Transform3D, ForwardDirectionMatchesTarget)
{
	const glm::vec3 targets[] = {
		{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {-1.0f, 1.0f, 0.0f},
		{0.0f, std::cos(glm::radians(30.0f)), std::sin(glm::radians(30.0f))}
	};
	for (const auto target : targets)
	{
		SCOPED_TRACE(::testing::Message() << target.x << ", " << target.y << ", " << target.z);
		const auto expected = glm::normalize(target);
		Transform3D transform;
		transform.SetRotation(EulerFromDirection(expected));
		const auto direction = transform.GetDirection();
		EXPECT_NEAR(direction.x, expected.x, 1e-5f);
		EXPECT_NEAR(direction.y, expected.y, 1e-5f);
		EXPECT_NEAR(direction.z, expected.z, 1e-5f);
	}
}

/**
 * @brief General 3D: non-trivial pitch, roll, and yaw with Z-up coordinate system.
 *
 * Uses known Euler angles (pitch, roll, yaw) = (-30°, 15°, 45°) stored as
 * vec3(pitch, roll, yaw) matching Transform3D's m_rotation convention.
 *
 * Independently computes expected direction via Rz*Rx*Ry * (0,1,0).
 * Ry(roll) has no effect on the Y-axis forward vector, so:
 *   d = Rz(45°) * Rx(-30°) * (0, 1, 0)
 *     = Rz(45°) * (0, cos(-30°), sin(-30°))
 *     = Rz(45°) * (0, 0.8660254, -0.5)
 *     = (-sin(45°)*0.8660254, cos(45°)*0.8660254, -0.5)
 *     ≈ (-0.612372, 0.612372, -0.5)
 */
TEST(Transform3D, GetDirection_General3D)
{
	Transform3D t;
	t.SetPosition(glm::vec3(5.0f, 2.0f, -3.0f)); // arbitrary, not at origin

	// pitch=-30°, roll=15°, yaw=45°
	const glm::vec3 eulerDeg(-30.0f, 15.0f, 45.0f);
	t.SetRotation(eulerDeg);

	// Independently compute expected direction from Euler angles
	// Using the same Rz*Rx*Ry rotation order as Transform3D::GetDirection()
	const glm::vec3 rad = glm::radians(eulerDeg);
	glm::mat4 rot{1.0f};
	rot = glm::rotate(rot, rad.z, glm::vec3(0.0f, 0.0f, 1.0f)); // Yaw   (Z)
	rot = glm::rotate(rot, rad.x, glm::vec3(1.0f, 0.0f, 0.0f)); // Pitch (X)
	rot = glm::rotate(rot, rad.y, glm::vec3(0.0f, 1.0f, 0.0f)); // Roll  (Y)

	const glm::vec3 expected = glm::normalize(
	    glm::vec3(rot * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));

	const glm::vec3 dir = t.GetDirection();
	EXPECT_NEAR(dir.x, expected.x, 1e-5f);
	EXPECT_NEAR(dir.y, expected.y, 1e-5f);
	EXPECT_NEAR(dir.z, expected.z, 1e-5f);
}
