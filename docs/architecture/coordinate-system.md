# Coordinate System

The whole codebase uses a Z-up, +Y forward right-handed convention (Blender
style), and every subsystem - scene, camera, shaders and tests - follows it.

## Axes

- Up direction: `+Z` (positive Z is world-space up)
- Forward direction: `+Y` (positive Y is forward/north)
- Right direction: `+X`

## Euler Rotation Storage and Order

- Euler rotation storage: `m_rotation = (pitch=X, roll=Y, yaw=Z)`
- Rotation multiplication order: `T * Rz(yaw) * Rx(pitch) * Ry(roll) * S`
- `Transform::GetDirection()`: `d = Rz(yaw) * Rx(pitch) * Ry(roll) * (0,1,0)`
  (rotates the forward vector)

## View Space and Projection

- View space is **left-handed** (`GLM_FORCE_DEPTH_ZERO_TO_ONE`): `+Z` is forward
  from camera, NDC Z range is `[0, 1]`.
- Camera projection requires `proj[1][1] *= -1` to flip Y for Vulkan NDC
  (NDC Y=-1 at top, Y=+1 at bottom).

## Camera Controller Spherical Coordinates

- `CameraController` spherical coordinates: elevation=`asin(dir.z)`,
  azimuth=`atan2(dir.x, dir.y)` (angle from the +Y forward axis).

## Test Helpers

- `MakeTestCamera(w, h)` places the camera at `(0,0,2)` with up=`(0,0,1)`.

## OBJ Files

OBJ files exported from Blender use Blender's native convention (Z-up, +Y
forward) - do NOT edit .obj files.
