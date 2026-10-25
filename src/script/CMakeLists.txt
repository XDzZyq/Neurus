# src/script/CMakeLists.txt - Script layer (embedded CPython + pybind11 bindings)
#
# Layer rule: script -> editor is allowed; script must never include Renderer or
# UI headers. Python/pybind11 headers are confined to implementation sources:
# the layer headers (ScriptEngine.h, ScriptContext.h, ScriptDispatch.h,
# ScriptView.h, proxies/) stay Python-free, and bindings/ holds only the
# pybind11 registration units plus the forward-declaring Bindings.h.
#
# Explicit source list (no glob), matching the other layer libraries.

add_library(neurus_script STATIC
	ScriptConsole.h
	ScriptContext.h
	ScriptDispatch.h
	ScriptEngine.h
	ScriptEngine.cpp
	ScriptEngineImpl.h
	ScriptView.h
	proxies/ProxyBase.h
	proxies/ObjectProxy.h
	proxies/Transform3DProxy.h
	bindings/Bindings.h
	bindings/ObjectBindings.cpp
	bindings/SceneBindings.cpp
	bindings/ConfigBindings.cpp
	bindings/Module.cpp
)

target_link_libraries(neurus_script PUBLIC
	neurus_editor
	neurus_core
	neurus_scene
	pybind11::embed
	Python::Python
)

target_include_directories(neurus_script PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

# The generated NEURUS_PYTHON_HOME header is an implementation detail of this
# library (only ScriptEngineImpl.h includes it).
target_include_directories(neurus_script PRIVATE
	${CMAKE_BINARY_DIR}/generated
)

target_compile_features(neurus_script PUBLIC cxx_std_20)
