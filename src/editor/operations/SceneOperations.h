/**
 * @file SceneOperations.h
 * @brief Concrete undoable operations for scene property/transform edits.
 *
 * Each edit is a reversible before→after transition built on TransitionOp
 * (see Operation.h): it stores the target object's UID plus absolute
 * before/after values, and replays by dispatching the same scene event the UI
 * would emit. A concrete class supplies only two things — a MakeEvent() that
 * turns a stored value into its event, and a kLabel — while the CRTP base
 * provides Emit/Inverse/Label. Controllers record an edit by constructing the
 * matching operation directly (std::make_unique<SetScaleOp>(...)).
 */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cereal/types/vector.hpp>

#include "glm/glm.hpp"

#include "editor/events/SceneEvents.h"
#include "editor/operations/Operation.h"
#include "scene/GlmSerialization.h"

namespace neurus {

/** @brief Viewport + render visibility pair (value carried by SetVisibilityOp). */
struct VisibilityState
{
	bool viewportVisible = true;
	bool renderVisible = true;

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("viewportVisible", viewportVisible),
		   cereal::make_nvp("renderVisible", renderVisible));
	}
};

/** @brief Camera pose endpoint: position + look-at target (value for CameraTransformOp). */
struct CameraPose
{
	glm::vec3 position{ 0.0f };
	glm::vec3 target{ 0.0f };

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("position", position),
		   cereal::make_nvp("target", target));
	}
};

/** @brief Absolute light-power edit. */
class SetLightPowerOp : public TransitionOp<SetLightPowerOp, LightPowerChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Light Power";

	LightPowerChanged MakeEvent(int o, const float& v) const
	{
		return LightPowerChanged{ o, v };
	}
};

/** @brief Absolute light-color edit. */
class SetLightColorOp : public TransitionOp<SetLightColorOp, LightColorChanged, glm::vec3>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Light Color";

	LightColorChanged MakeEvent(int o, const glm::vec3& v) const
	{
		return LightColorChanged{ o, v.r, v.g, v.b };
	}
};

/** @brief Absolute light-shadow toggle. */
class SetLightShadowOp : public TransitionOp<SetLightShadowOp, LightShadowChanged, bool>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Toggle Light Shadow";

	LightShadowChanged MakeEvent(int o, const bool& v) const
	{
		return LightShadowChanged{ o, v };
	}
};

/** @brief Absolute position edit. */
class SetPositionOp : public TransitionOp<SetPositionOp, PositionChanged, glm::vec3>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Move";

	PositionChanged MakeEvent(int o, const glm::vec3& v) const
	{
		return PositionChanged{ o, v.x, v.y, v.z };
	}
};

/** @brief Absolute rotation edit. */
class SetRotationOp : public TransitionOp<SetRotationOp, RotationChanged, glm::vec3>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Rotate";

	RotationChanged MakeEvent(int o, const glm::vec3& v) const
	{
		return RotationChanged{ o, v.x, v.y, v.z };
	}
};

/** @brief Absolute scale edit. */
class SetScaleOp : public TransitionOp<SetScaleOp, ScaleChanged, glm::vec3>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Scale";

	ScaleChanged MakeEvent(int o, const glm::vec3& v) const
	{
		return ScaleChanged{ o, v.x, v.y, v.z };
	}
};

/** @brief Absolute visibility edit (viewport + render flags). */
class SetVisibilityOp : public TransitionOp<SetVisibilityOp, VisibilityChanged, VisibilityState>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Visibility";

	VisibilityChanged MakeEvent(int o, const VisibilityState& v) const
	{
		return VisibilityChanged{ o, v.viewportVisible, v.renderVisible };
	}
};

/** @brief Absolute light-radius edit. */
class SetLightRadiusOp : public TransitionOp<SetLightRadiusOp, LightRadiusChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Light Radius";

	LightRadiusChanged MakeEvent(int o, const float& v) const
	{
		return LightRadiusChanged{ o, v };
	}
};

/** @brief Absolute spot-light cutoff edit. */
class SetLightCutoffOp : public TransitionOp<SetLightCutoffOp, LightCutoffChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Light Cutoff";

	LightCutoffChanged MakeEvent(int o, const float& v) const
	{
		return LightCutoffChanged{ o, v };
	}
};

/** @brief Absolute spot-light outer-cutoff edit. */
class SetLightOuterCutoffOp : public TransitionOp<SetLightOuterCutoffOp, LightOuterCutoffChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Light Outer Cutoff";

	LightOuterCutoffChanged MakeEvent(int o, const float& v) const
	{
		return LightOuterCutoffChanged{ o, v };
	}
};

/** @brief Absolute mesh shadow-casting toggle. */
class SetMeshShadowOp : public TransitionOp<SetMeshShadowOp, MeshShadowChanged, bool>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Toggle Mesh Shadow";

	MeshShadowChanged MakeEvent(int o, const bool& v) const
	{
		return MeshShadowChanged{ o, v };
	}
};

/** @brief Absolute mesh material toggle. */
class SetMeshMaterialOp : public TransitionOp<SetMeshMaterialOp, MeshMaterialChanged, bool>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Toggle Mesh Material";

	MeshMaterialChanged MakeEvent(int o, const bool& v) const
	{
		return MeshMaterialChanged{ o, v };
	}
};

/** @brief Absolute environment IBL-intensity edit. */
class SetEnvIntensityOp : public TransitionOp<SetEnvIntensityOp, EnvironmentIntensityChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Environment Intensity";

	EnvironmentIntensityChanged MakeEvent(int o, const float& v) const
	{
		return EnvironmentIntensityChanged{ o, v };
	}
};

/** @brief Absolute environment rotation edit. */
class SetEnvRotationOp : public TransitionOp<SetEnvRotationOp, EnvironmentRotationChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Environment Rotation";

	EnvironmentRotationChanged MakeEvent(int o, const float& v) const
	{
		return EnvironmentRotationChanged{ o, v };
	}
};

// ---------------------------------------------------------------------------
// Debug object properties (issue #22)
// ---------------------------------------------------------------------------
//
// One op per knob, like every other property above. The three debug types share
// these ops because they share the events: the controller resolves the UID
// against whichever debug pool holds it, so an op never needs to know which.

/** @brief Absolute debug tint edit (RGBA). */
class SetDebugColorOp : public TransitionOp<SetDebugColorOp, DebugColorChanged, glm::vec4>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Debug Color";

	DebugColorChanged MakeEvent(int o, const glm::vec4& v) const
	{
		return DebugColorChanged{ o, v.r, v.g, v.b, v.a };
	}
};

/** @brief Absolute debug opacity edit. */
class SetDebugOpacityOp : public TransitionOp<SetDebugOpacityOp, DebugOpacityChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Debug Opacity";

	DebugOpacityChanged MakeEvent(int o, const float& v) const
	{
		return DebugOpacityChanged{ o, v };
	}
};

/** @brief Absolute debug x-ray toggle. */
class SetDebugXRayOp : public TransitionOp<SetDebugXRayOp, DebugXRayChanged, bool>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Toggle Debug X-Ray";

	DebugXRayChanged MakeEvent(int o, const bool& v) const
	{
		return DebugXRayChanged{ o, v };
	}
};

/** @brief Absolute debug-line width edit. */
class SetDebugLineWidthOp : public TransitionOp<SetDebugLineWidthOp, DebugLineWidthChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Debug Line Width";

	DebugLineWidthChanged MakeEvent(int o, const float& v) const
	{
		return DebugLineWidthChanged{ o, v };
	}
};

/** @brief Absolute debug-line stipple toggle. */
class SetDebugStippleOp : public TransitionOp<SetDebugStippleOp, DebugLineStippleChanged, bool>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Toggle Debug Stipple";

	DebugLineStippleChanged MakeEvent(int o, const bool& v) const
	{
		return DebugLineStippleChanged{ o, v };
	}
};

/** @brief Absolute point-sprite shape edit. */
class SetDebugPointTypeOp : public TransitionOp<SetDebugPointTypeOp, DebugPointTypeChanged, int>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Debug Point Type";

	DebugPointTypeChanged MakeEvent(int o, const int& v) const
	{
		return DebugPointTypeChanged{ o, v };
	}
};

/** @brief Absolute point-size edit. */
class SetDebugPointScaleOp : public TransitionOp<SetDebugPointScaleOp, DebugPointScaleChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Debug Point Size";

	DebugPointScaleChanged MakeEvent(int o, const float& v) const
	{
		return DebugPointScaleChanged{ o, v };
	}
};

/** @brief Absolute point projection-mode edit (screen-space vs world-space size). */
class SetDebugProjectionModeOp
	: public TransitionOp<SetDebugProjectionModeOp, DebugProjectionModeChanged, int>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Set Debug Projection Mode";

	DebugProjectionModeChanged MakeEvent(int o, const int& v) const
	{
		return DebugProjectionModeChanged{ o, v };
	}
};

/**
 * @brief Absolute replacement of a debug object's position list.
 *
 * Stores both endpoints as whole lists, so one op undoes a coordinate edit, a
 * row insertion and a row removal identically. The event carries the flattened
 * form; the op keeps the glm form because that is what the scene object holds.
 */
class SetDebugPositionsOp
	: public TransitionOp<SetDebugPositionsOp, DebugPositionsChanged, std::vector<glm::vec3>>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Edit Debug Positions";

	DebugPositionsChanged MakeEvent(int o, const std::vector<glm::vec3>& v) const
	{
		std::vector<float> xyz;
		xyz.reserve(v.size() * 3);
		for (const glm::vec3& p : v)
		{
			xyz.push_back(p.x);
			xyz.push_back(p.y);
			xyz.push_back(p.z);
		}
		return DebugPositionsChanged{ o, std::move(xyz) };
	}
};

/**
 * @brief Absolute camera pose edit (position + target), coarse-grained.
 *
 * Records the coupled camera transform for an orbit / pan / dolly drag and for
 * panel target edits. Non-mergeable (empty MergeKey): each recorded op is its
 * own undo entry. This is correct because a viewport drag is already bounded by
 * a controller gesture (CameraDragBegin/End) that commits exactly ONE op on
 * release — so consecutive separate drags must stay separate undo entries.
 *
 * Scroll zoom, which has no press/release boundary, uses the mergeable sibling
 * CameraZoomOp instead. Camera *position* edited via the property panel reuses
 * the generic SetPositionOp, so there is no separate camera-position op to
 * overlap with the object transform path.
 */
class CameraTransformOp : public TransitionOp<CameraTransformOp, CameraPoseChanged, CameraPose>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Camera Transform";

	CameraPoseChanged MakeEvent(int o, const CameraPose& v) const
	{
		return CameraPoseChanged{ o,
			v.position.x, v.position.y, v.position.z,
			v.target.x, v.target.y, v.target.z };
	}
};

/**
 * @brief Absolute camera pose edit produced by scroll zoom — mergeable.
 *
 * Identical replay semantics to CameraTransformOp (same absolute CameraPose,
 * same CameraPoseChanged event), but exposes a per-camera MergeKey so a scroll
 * burst — which fires one op per notch with no gesture boundary — coalesces
 * into a single undo entry. Kept a separate type (rather than a flag on
 * CameraTransformOp) so the mergeable-vs-standalone intent is encoded in the
 * type, not decided at each call site.
 *
 * The merge key includes the zoom direction (derived from the stored pose:
 * camera-to-target distance shrinking = in, growing = out), so changing scroll
 * direction breaks the merge — a zoom-in run and a zoom-out run each get their
 * own undo entry.
 */
class CameraZoomOp : public TransitionOp<CameraZoomOp, CameraPoseChanged, CameraPose>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Camera Zoom";

	std::string MergeKey() const override
	{
		const float beforeR = glm::length(m_before.position - m_before.target);
		const float afterR  = glm::length(m_after.position - m_after.target);
		const char* dir = afterR < beforeR ? "in" : "out";
		return "camera_zoom:" + std::to_string(m_uid) + ":" + dir;
	}

	CameraPoseChanged MakeEvent(int o, const CameraPose& v) const
	{
		return CameraPoseChanged{ o,
			v.position.x, v.position.y, v.position.z,
			v.target.x, v.target.y, v.target.z };
	}
};

/** @brief Absolute camera field-of-view ("Camera Ratio") edit. */
class CameraFovOp : public TransitionOp<CameraFovOp, CameraFovChanged, float>
{
public:
	using TransitionOp::TransitionOp;
	static constexpr const char* kLabel = "Camera Ratio";

	std::string MergeKey() const override { return "camera_fov:" + std::to_string(m_uid); }

	CameraFovChanged MakeEvent(int o, const float& v) const
	{
		return CameraFovChanged{ o, v };
	}
};

/** @brief Absolute selection-set endpoint: ordered selected UIDs + active UID. */
struct SelectionState
{
	std::vector<int> selectedUids; ///< Ordered selected object UIDs.
	int activeUid = 0;             ///< Active object UID (0 = none).

	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("selectedUids", selectedUids),
		   cereal::make_nvp("activeUid", activeUid));
	}
};

/**
 * @brief Absolute selection-set edit (select / multi-select / deselect / clear).
 *
 * Selection is scene-level SET state (Scene::selections), not a per-object
 * flag, so it does not fit TransitionOp: the op stores the full before/after
 * UID lists and replays by dispatching a single SelectionChanged event.
 *
 * PreservesRedo() is true so navigating the selection does NOT discard a
 * pending redo chain. This is safe because every operation is an absolute
 * state-set: a preserved redo op restores its stored end state regardless of
 * selection changes recorded in between.
 */
class SetSelectionOp : public Operation
{
public:
	SetSelectionOp() = default;

	SetSelectionOp(SelectionState before, SelectionState after)
		: m_before(std::move(before))
		, m_after(std::move(after))
	{}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.emitNow(SelectionChanged{ m_after.selectedUids, m_after.activeUid });
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SetSelectionOp>(m_after, m_before);
	}

	std::string Label() const override { return "Select"; }

	bool PreservesRedo() const override { return true; }

	/** @brief Serializes the before/after selection endpoints. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("before", m_before),
		   cereal::make_nvp("after", m_after));
	}

private:
	SelectionState m_before;
	SelectionState m_after;
};

/**
 * @brief Absolute camera-activation edit (activate / switch / deactivate).
 *
 * The exact shape of SetSelectionOp and for the same reason: activation is
 * scene-level state (one UID on the Scene), not a per-object flag, so it does
 * not fit TransitionOp's object-UID dispatch. The op stores the before/after
 * UIDs and replays by dispatching one ActiveCameraChanged, so undo and redo run
 * the same controller handler a live toggle does.
 *
 * `0` is a first-class value on both ends: the viewport falls back to the
 * editor camera, which is a normal state and not an error.
 *
 * PreservesRedo() is deliberately NOT overridden. Unlike navigating the
 * selection, switching the view camera is a real document change, so it
 * truncates a pending redo chain exactly like every other scene mutation.
 */
class SetActiveCameraOp : public Operation
{
public:
	SetActiveCameraOp() = default;

	SetActiveCameraOp(int before, int after)
		: m_before(before)
		, m_after(after)
	{}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.emitNow(ActiveCameraChanged{ m_after });
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SetActiveCameraOp>(m_after, m_before);
	}

	std::string Label() const override { return "Activate Camera"; }

	/** @brief Serializes the before/after activation endpoints. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("before", m_before),
		   cereal::make_nvp("after", m_after));
	}

private:
	int m_before = 0;
	int m_after = 0;
};

/**
 * @brief Scene-membership toggle: add (or re-add) / delete a batch of objects.
 *
 * Stores the target object UIDs plus an `add` flag. Apply() re-dispatches the
 * ORIGINATING membership events (SceneObjectAddRequested per UID, or ONE
 * batched SceneObjectDeleteRequested), so replay runs the same controller
 * handler as a live edit; the handler's Submit is muted by OperationManager's
 * Phase::Replaying guard, so playback does not re-record. Inverse() flips the
 * flag — the inverse of Add is Delete and vice versa (the AddShaderFieldOp
 * convention). Batching the delete direction keeps the composite light:
 * delete-of-N records ONE op, not N.
 *
 * The pooled resources are NEVER removed — delete only drops the scene
 * references, so the inverse re-registers without any reload from disk and
 * survives project save/load (pool + history both persist).
 *
 * @note Replay re-enters the forward handler (harmless: Submit is muted). If
 *       a handler carries gesture side effects (selection, gesture state)
 *       that must NOT re-run on replay, give it a dedicated restore event
 *       (ShaderCodeRestored convention) instead of the forward event — but
 *       keep ops minimal by default.
 */
class SceneObjectAddOp : public Operation
{
public:
	SceneObjectAddOp() = default;

	/**
	 * @brief Constructs a membership toggle over a batch of objects.
	 * @param uids Target object UIDs (pooled resources).
	 * @param add true = add/re-add all; false = remove all scene references.
	 */
	SceneObjectAddOp(std::vector<int> uids, bool add)
		: m_uids(std::move(uids))
		, m_add(add)
	{}

	void Apply(OperationContext& ctx) override
	{
		if (m_add)
		{
			for (int uid : m_uids)
				ctx.events.emitNow(SceneObjectAddRequested{ uid });
		}
		else
		{
			ctx.events.emitNow(SceneObjectDeleteRequested{ m_uids });
		}
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SceneObjectAddOp>(m_uids, !m_add);
	}

	std::string Label() const override { return m_add ? "Add Object" : "Delete Object"; }

	/** @brief Serializes the target UID list + membership direction. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("uids", m_uids),
		   cereal::make_nvp("add", m_add));
	}

private:
	std::vector<int> m_uids;   ///< Target object UIDs (serialized).
	bool m_add = false;        ///< true = add/re-add; false = remove (inverse direction).
};

} // namespace neurus
