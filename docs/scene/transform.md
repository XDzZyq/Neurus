# Transform

`Transform` is the abstract spatial base and `Transform3D` its TRS implementation, computing a model matrix that is kept up to date eagerly.

## Transform base

`Transform` (`scene/Transform.h`) is the abstract polymorphic interface:

- `GetTransformPtr()` returns a non-owning pointer to the transform; `Transform3D` narrows the return type.
- Non-copyable, movable, with a protected constructor. All spatially placed scene objects inherit `Transform3D`.

## Transform3D

`Transform3D` manages position, rotation (Euler angles in degrees) and scale for 3D scene objects:

- `SetPosition` / `SetRotation` / `SetScale` are the writers; `GetPosition` / `GetRotation` / `GetScale` read them back.
- `GetModelMatrix()` returns the 4x4 TRS matrix.
- `GetNormalMatrix()` returns the inverse transpose of the upper-left 3x3 of the model matrix, which keeps normals correct under non-uniform scaling.
- Not thread-safe: access from the main thread only.

Rotation is stored as `(pitch = X, roll = Y, yaw = Z)` in degrees, converted to radians at compute time. The full convention is documented in [Coordinate System](../architecture/coordinate-system.md).

## Model matrix composition

```
Model = T(position) * Rz(yaw) * Rx(pitch) * Ry(roll) * S(scale)
```

Applied to a vertex `v`: `T * (Rz * (Rx * (Ry * (S * v))))`, so the order is scale, roll (Y), pitch (X), yaw (Z), then translate - rotation in ZXY order (yaw = Z, pitch = X, roll = Y).

## Eager recomputation

The model matrix is always up to date:

- Every setter recomputes it immediately.
- `serialize` persists only `m_position`, `m_rotation` and `m_scale`; `o_modelMatrix` is a computed value. Deserialization bypasses the setters, so the load branch calls `RecomputeModelMatrix()` itself. Without that, a loaded transform would keep the identity matrix it was constructed with, and the property panel would read the loaded TRS while the geometry and debug passes drew the object at the origin with unit scale.
- The setters and the load branch are the only two writers, and both go through `RecomputeModelMatrix()`, so the cached matrix cannot go stale behind their back.

## GetDirection

`GetDirection()` rotates the local forward vector `(0, 1, 0)` by the current Euler rotation (yaw = Z, pitch = X, roll = Y) and returns a normalized direction. With no rotation it returns `(0, 1, 0)`.

## Related conventions

- The codebase is Z-up, +Y forward, right-handed: up is `+Z`, forward is `+Y`, right is `+X`.
- View space is left-handed (`GLM_FORCE_DEPTH_ZERO_TO_ONE`): `+Z` is forward from the camera and NDC Z is `[0, 1]`.
- The camera projection flips Y for Vulkan NDC (`proj[1][1] *= -1`).
- `CameraController` spherical coordinates use `elevation = asin(dir.z)` and `azimuth = atan2(dir.x, dir.y)`.

All of these are covered in [Coordinate System](../architecture/coordinate-system.md).
