# src/ui/CMakeLists.txt - UI Layer (Qt6 QML)

add_library(neurus_ui STATIC
	UIManager.h
	UIManager.cpp
	UIContext.h
	UIContext.cpp
	Icons.h
	Icons.cpp
	items/ScalarSlider.h
	items/ScalarSlider.cpp
	items/OutlinerRow.h
	items/OutlinerRow.cpp
	items/ProfilingRow.h
	items/ProfilingRow.cpp
	items/Vec3Spin.h
	items/Vec3Spin.cpp
	items/ShaderFieldRow.h
	items/ShaderFieldRow.cpp
	items/CodeEditor.h
	items/CodeEditor.cpp
	models/LogModel.h
	models/LogModel.cpp
	models/LogFilterProxy.h
	models/LogFilterProxy.cpp
	models/ShaderStructModel.h
	models/ShaderStructModel.cpp
	delegates/LogDelegate.h
	delegates/LogDelegate.cpp
	delegates/ShaderFieldDelegate.h
	delegates/ShaderFieldDelegate.cpp
	utils/ShaderHighlighter.h
	utils/ShaderHighlighter.cpp
	utils/I18n.h
	utils/I18n.cpp
	utils/POCatalog.h
	utils/POCatalog.cpp
	presets/CameraProperties.h
	presets/CameraProperties.cpp
	presets/MeshProperties.h
	presets/MeshProperties.cpp
	presets/LightProperties.h
	presets/LightProperties.cpp
	presets/EnvironmentProperties.h
	presets/EnvironmentProperties.cpp
	panels/UIPanel.h
	panels/UIPanel.cpp
	panels/Outliner.h
	panels/Outliner.cpp
	panels/PropertyPanel.h
	panels/PropertyPanel.cpp
	panels/ProfilingPanel.h
	panels/ProfilingPanel.cpp
	panels/RenderConfigPanel.h
	panels/RenderConfigPanel.cpp
	panels/Viewport.h
	panels/Viewport.cpp
	panels/ShaderEditorPanel.h
	panels/ShaderEditorPanel.cpp
	panels/LogPanel.h
	panels/LogPanel.cpp
	panels/PreferencesDialog.h
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
