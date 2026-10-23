/**
 * @file test_editor_overlay_redraw.cpp
 * @brief The Editor's half of the overlay contract: a rebuilt overlay payload
 *        posts RedrawOverlaysRequest, and only then.
 *
 * The renderer stops drawing scene frames once it has accumulated
 * r_sampling_count of them, so a selection outline or a gizmo guide that changes
 * after convergence reaches the screen only if something makes the renderer
 * redraw the overlays. That "something" is one request posted by Edit(), and the
 * two failure modes are opposite and both silent:
 *
 *   1. Not posting -> the overlay change is invisible until an unrelated scene
 *      edit restarts accumulation. Nothing in the frame is wrong, which is why
 *      it reads as "the outline is stuck".
 *   2. Posting every tick -> accumulation is restarted forever, so a converged
 *      image is never re-presented and progressive sampling stops converging.
 *
 * Only the second one is measurable as a count, so both are pinned: a selection
 * change posts exactly one request, and an idle tick posts none.
 *
 * No GPU and no Vulkan: the sink is a recording fake, so nothing is ever drawn.
 * Editor(nullptr) would swallow the requests entirely - which is exactly why the
 * sink has to be a real RenderRequestSink here.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <variant>
#include <vector>

#include "editor/Editor.h"
#include "editor/events/GizmoEvents.h"
#include "editor/events/SceneEvents.h"
#include "render/RenderRequestSink.h"
#include "render/RenderRequests.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

using namespace neurus;

namespace
{

/**
 * @brief A RenderRequestSink that keeps what it was handed.
 *
 * The Editor can only Post, so a recorder is the complete observer of what it
 * asked the renderer for - no renderer, thread or GPU involved.
 */
class RecordingSink : public RenderRequestSink
{
public:
	void Post(RenderRequest&& request) override { m_posted.push_back(std::move(request)); }

	/// @brief How many renders the whole batch so far asked for.
	template<typename T>
	size_t Count() const
	{
		return static_cast<size_t>(std::count_if(m_posted.begin(), m_posted.end(),
		                                         [](const RenderRequest& r) {
			                                         return std::holds_alternative<T>(r);
		                                         }));
	}

	/// @brief Requests posted so far, of every kind.
	size_t Size() const { return m_posted.size(); }

	/// @brief Forgets everything posted so far, so a test can measure one tick.
	void Clear() { m_posted.clear(); }

private:
	std::vector<RenderRequest> m_posted;
};

/**
 * @brief An Editor with an event bus and one selectable object, and a clean sink.
 *
 * Editor::Initialize() is required: the "a selection moves the gizmo" subscription
 * and the RenderResetEvent subscription both live there, so without it the test
 * would measure an Editor that ignores its own events.
 *
 * The object is a Camera from the pool because it is a Transform3D - the gizmo
 * anchor resolves one - and it needs no asset on disk, so the test does not
 * depend on res/ being copied next to the binary.
 */
struct OverlayFixture
{
	explicit OverlayFixture()
		: editor(&sink)
	{
		editor.Initialize();
		editor.NewScene();

		auto camera = editor.GetResourceManager().Load<Camera>();
		uid = camera->GetObjectID();
		editor.GetScene().UseCamera(camera);

		// Both builders start dirty, and NewScene() marks the overlays dirty too:
		// this tick is the DocumentOpen redraw, not something a test caused.
		editor.Edit();
		sink.Clear();
	}

	RecordingSink sink;
	Editor editor;
	int uid = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. The case that motivated the request: selection changes are not scene edits
// ---------------------------------------------------------------------------

/**
 * @brief A selection posts exactly one RedrawOverlaysRequest and no reset.
 *
 * Selecting is deliberately not a scene modification, so it emits no
 * RenderResetEvent - nothing would restart accumulation for it. The outline
 * therefore hangs on this one request. And it must not post a reset: a reset
 * throws away every sample accumulated so far, which a click has no reason to do.
 */
TEST(EditorOverlayRedraw, SelectionChange_PostsExactlyOneRedrawAndNoReset)
{
	OverlayFixture f;

	f.editor.OnUIEvent(ObjectSelected{f.uid, 0});
	f.editor.Edit();

	EXPECT_EQ(f.sink.Count<RedrawOverlaysRequest>(), 1u)
	    << "a selection change rebuilt the gizmo payload, so the renderer has to "
	       "redraw the overlays or the new outline never appears";
	EXPECT_EQ(f.sink.Count<ResetAccumulationRequest>(), 0u)
	    << "selecting is not a scene edit; restarting accumulation would discard "
	       "the converged image for a click";
	EXPECT_EQ(f.sink.Size(), 1u) << "one overlay rebuild, one request";
}

/**
 * @brief Deselecting is the same contract in the other direction.
 *
 * The outline has to disappear, so "nothing is selected now" is as much an
 * overlay change as a selection is. The empty payload is what makes this easy to
 * lose: the rebuilt list is empty, not absent.
 */
TEST(EditorOverlayRedraw, Deselection_PostsExactlyOneRedraw)
{
	OverlayFixture f;

	f.editor.OnUIEvent(ObjectSelected{f.uid, 0});
	f.editor.Edit();
	f.sink.Clear();

	f.editor.OnUIEvent(ObjectSelected{0, 0}); // background click clears the selection
	f.editor.Edit();

	EXPECT_EQ(f.sink.Count<RedrawOverlaysRequest>(), 1u);
	EXPECT_EQ(f.sink.Size(), 1u);
}

// ---------------------------------------------------------------------------
// 2. The opposite failure: a redraw on every tick would pin accumulation at 0
// ---------------------------------------------------------------------------

/**
 * @brief An idle tick posts nothing at all.
 *
 * This is the regression that would be invisible in a screenshot: a redraw every
 * tick restarts nothing but re-presents forever, so progressive sampling (and the
 * convergence the whole sampling count exists for) never settles.
 */
TEST(EditorOverlayRedraw, IdleTick_PostsNothing)
{
	OverlayFixture f;

	f.editor.Edit();
	f.editor.Edit();

	EXPECT_EQ(f.sink.Size(), 0u) << "nothing changed, so nothing may be asked of the renderer";
}

// ---------------------------------------------------------------------------
// 3. The overlap: inputs that already reset accumulation still redraw
// ---------------------------------------------------------------------------

/**
 * @brief Arming the gizmo posts a reset *and* a redraw.
 *
 * The guide appears through RenderResetEvent (which posts ResetAccumulationRequest
 * and dirties the overlays), so this path was already covered - the point here is
 * that the new post is additive, not a replacement: it does not suppress the reset
 * that makes the scene re-render, and it is not suppressed by it.
 */
TEST(EditorOverlayRedraw, GizmoArm_PostsResetAndRedraw)
{
	OverlayFixture f;

	f.editor.OnUIEvent(GizmoModeRequested{f.uid, GizmoMode::Move});
	f.editor.Edit();

	EXPECT_EQ(f.sink.Count<ResetAccumulationRequest>(), 1u);
	EXPECT_EQ(f.sink.Count<RedrawOverlaysRequest>(), 1u);
}
