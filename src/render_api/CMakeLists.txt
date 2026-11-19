# CPU-only values shared across the application, editor, UI and renderer.
add_library(neurus_render_api STATIC
	RenderFrameSnapshot.hpp
	RenderFrameSnapshot.cpp
	RenderContext.hpp
	RenderContext.cpp
	ProfilingData.hpp
	RenderRequests.hpp
	RenderRequests.cpp
	RenderRequestSink.hpp
	overlay/OverlayGeometry.hpp
	overlay/DebugDrawList.hpp
	overlay/GizmoDrawList.hpp
)

target_include_directories(neurus_render_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(neurus_render_api PUBLIC neurus_asset_data neurus_scene neurus_core)
