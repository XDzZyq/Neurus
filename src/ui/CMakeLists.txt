# src/ui/CMakeLists.txt - UI Layer (Qt6 QML)

add_library(neurus_ui STATIC
	UIManager.hpp
	UIManager.cpp
	# Qt boundary of the editor layer: the signal bus and the Qt-enum input
	# translations. They live in the UI target because it is the only consumer.
	../editor/events/UIEvents.hpp
	../editor/events/UIEvents.cpp
	../editor/Input.cpp
	UIContext.hpp
	UIContext.cpp
	Icons.hpp
	Icons.cpp
	items/ScalarSlider.hpp
	items/ScalarSlider.cpp
	items/OutlinerRow.hpp
	items/OutlinerRow.cpp
	items/ProfilingRow.hpp
	items/ProfilingRow.cpp
	items/Vec3Spin.hpp
	items/Vec3Spin.cpp
	items/ShaderFieldRow.hpp
	items/ShaderFieldRow.cpp
	items/CodeEditor.hpp
	items/CodeEditor.cpp
	items/ColorButton.hpp
	items/ColorButton.cpp
	models/LogModel.hpp
	models/LogModel.cpp
	models/LogFilterProxy.hpp
	models/LogFilterProxy.cpp
	models/ShaderStructModel.hpp
	models/ShaderStructModel.cpp
	delegates/LogDelegate.hpp
	delegates/LogDelegate.cpp
	delegates/ShaderFieldDelegate.hpp
	delegates/ShaderFieldDelegate.cpp
	utils/CodeHighlighter.hpp
	utils/CodeHighlighter.cpp
	utils/I18n.hpp
	utils/I18n.cpp
	utils/POCatalog.hpp
	utils/POCatalog.cpp
	presets/CameraProperties.hpp
	presets/CameraProperties.cpp
	presets/MeshProperties.hpp
	presets/MeshProperties.cpp
	presets/LightProperties.hpp
	presets/LightProperties.cpp
	presets/EnvironmentProperties.hpp
	presets/EnvironmentProperties.cpp
	presets/DebugProperties.hpp
	presets/DebugProperties.cpp
	panels/UIPanel.hpp
	panels/UIPanel.cpp
	panels/Outliner.hpp
	panels/Outliner.cpp
	panels/PropertyPanel.hpp
	panels/PropertyPanel.cpp
	panels/ProfilingPanel.hpp
	panels/ProfilingPanel.cpp
	panels/RenderConfigPanel.hpp
	panels/RenderConfigPanel.cpp
	panels/Viewport.hpp
	panels/Viewport.cpp
	panels/ShaderEditorPanel.hpp
	panels/ShaderEditorPanel.cpp
	panels/LogPanel.hpp
	panels/LogPanel.cpp
	panels/ScriptConsolePanel.hpp
	panels/ScriptConsolePanel.cpp
	panels/ScriptPreferences.hpp
	panels/PreferencesDialog.hpp
	panels/PreferencesDialog.cpp
)

target_link_libraries(neurus_ui PUBLIC
	Qt6::Core
	Qt6::Quick
	Qt6::Qml
	Qt6::Widgets
	neurus_editor
	ads::qtadvanceddocking-qt6
)

target_link_libraries(neurus_ui PRIVATE
	Vulkan::Vulkan
)

# ADS_STATIC: tell Qt-Advanced-Docking-System headers to use static linkage
target_compile_definitions(neurus_ui PUBLIC ADS_STATIC)

target_include_directories(neurus_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)

# Qt resource file (QML)
qt_add_resources(neurus_ui "resources"
	PREFIX "/"
	FILES
		qml/main.qml
		qml/outliner.qss
		qml/shader_editor.qss
		qml/logpanel.qss
)

# Qt resource file (SVG icons)
qt_add_resources(neurus_ui "icons"
	PREFIX "/icons"
	BASE "${CMAKE_SOURCE_DIR}/res/ui/icons"
	FILES
		"${CMAKE_SOURCE_DIR}/res/ui/icons/scene/camera.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/scene/environment.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/scene/light.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/scene/mesh.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/editor/movie.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/editor/movie_off.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/editor/preview_invisible.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/editor/preview_visible.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/editor/render_invisible.svg"
		"${CMAKE_SOURCE_DIR}/res/ui/icons/editor/render_visible.svg"
)

# Qt resource file (application icon - used for the window and taskbar icon)
qt_add_resources(neurus_ui "app_icon"
	PREFIX "/app"
	BASE "${CMAKE_SOURCE_DIR}/res/ui/logo"
	FILES
		"${CMAKE_SOURCE_DIR}/res/ui/logo/neurus.svg"
)

# Qt resource file (i18n dictionaries — loaded on demand by I18n)
# Globbed so dropping res/i18n/<code>.po in is the only step needed to add a
# language; I18n::supportedLanguages() enumerates the embedded catalogs.
file(GLOB NEURUS_I18N_CATALOGS CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/res/i18n/*.po")
qt_add_resources(neurus_ui "i18n"
	PREFIX "/i18n"
	BASE "${CMAKE_SOURCE_DIR}/res/i18n"
	FILES
		${NEURUS_I18N_CATALOGS}
)

# Enable Qt MOC, UIC, RCC on this target
set_target_properties(neurus_ui PROPERTIES
	AUTOMOC ON
	AUTORCC ON
)
