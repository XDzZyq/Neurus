# src/editor/CMakeLists.txt - Editor Layer

add_library(neurus_editor STATIC
	Editor.hpp
	Editor.cpp
	RenderManager.hpp
	RenderManager.cpp
	Input.hpp
	commands/EditorCommandAdapter.hpp
	commands/EditorCommandAdapter.cpp
	viewport/DebugDrawBuilder.hpp
	viewport/DebugDrawBuilder.cpp
	viewport/EditorViewport.hpp
	viewport/EditorViewport.cpp
	viewport/TransformGizmo.hpp
	viewport/TransformGizmo.cpp
	viewport/GizmoDrawBuilder.hpp
	viewport/GizmoDrawBuilder.cpp
	controllers/SceneController.hpp
	controllers/SceneController.cpp
	controllers/ObjectController.hpp
	controllers/ObjectController.cpp
	controllers/Controllers.hpp
	controllers/ControllerContext.hpp
	controllers/MutationNotify.hpp
	controllers/CameraController.hpp
	controllers/CameraController.cpp
	controllers/RenderConfigController.hpp
	controllers/RenderConfigController.cpp
	controllers/ScriptController.hpp
	controllers/ScriptController.cpp
	controllers/ShaderController.hpp
	controllers/ShaderController.cpp
	controllers/TransformGizmoController.hpp
	controllers/TransformGizmoController.cpp
	operations/IOperationSink.hpp
	operations/Operation.hpp
	operations/OperationContext.hpp
	operations/OperationManager.hpp
	operations/OperationManager.cpp
	operations/SceneOperations.hpp
	operations/ConfigOperations.hpp
	operations/ShaderOperations.hpp
	operations/ScriptOperations.hpp
	operations/registrations/OperationRegistration.hpp
	operations/registrations/OperationRegistration.cpp
	events/GizmoEvents.hpp
	events/ScriptEvents.hpp
)

# Qt lives in the UI layer. UIEvents (the Qt signal bus) and Input.cpp (the
# Qt-enum translations) are compiled into neurus_ui; the editor itself is
# Qt-free so the headless renderer can link it.
target_link_libraries(neurus_editor PUBLIC
	neurus_command
	neurus_scene
	neurus_asset
	stb
)

target_include_directories(neurus_editor PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(neurus_editor PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${Vulkan_INCLUDE_DIRS}
)
