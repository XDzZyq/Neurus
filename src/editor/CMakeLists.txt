# src/editor/CMakeLists.txt - Editor Layer

add_library(neurus_editor STATIC
	Editor.h
	Editor.cpp
	Input.h
	Input.cpp
	viewport/DebugDrawBuilder.h
	viewport/DebugDrawBuilder.cpp
	viewport/EditorViewport.h
	viewport/EditorViewport.cpp
	viewport/TransformGizmo.h
	viewport/TransformGizmo.cpp
	viewport/GizmoDrawBuilder.h
	viewport/GizmoDrawBuilder.cpp
	controllers/SceneController.h
	controllers/SceneController.cpp
	controllers/Controllers.h
	controllers/ControllerContext.h
	controllers/CameraController.h
	controllers/CameraController.cpp
	controllers/RenderConfigController.h
	controllers/RenderConfigController.cpp
	controllers/ShaderController.h
	controllers/ShaderController.cpp
	controllers/TransformGizmoController.h
	controllers/TransformGizmoController.cpp
	operations/IOperationSink.h
	operations/Operation.h
	operations/OperationContext.h
	operations/OperationManager.h
	operations/OperationManager.cpp
	operations/SceneOperations.h
	operations/ConfigOperations.h
	operations/ShaderOperations.h
	operations/registrations/OperationRegistration.h
	operations/registrations/OperationRegistration.cpp
	events/UIEvents.h
	events/UIEvents.cpp
	events/GizmoEvents.h
)

target_link_libraries(neurus_editor PUBLIC
	Qt6::Core
	neurus_scene
	stb
)

target_include_directories(neurus_editor PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(neurus_editor PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${Vulkan_INCLUDE_DIRS}
)

# Enable Qt MOC for QObject subclasses (UIEvents)
set_target_properties(neurus_editor PROPERTIES AUTOMOC ON)
