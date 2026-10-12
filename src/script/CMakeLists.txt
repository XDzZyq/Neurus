# src/script/CMakeLists.txt - Script layer (embedded CPython + pybind11 bindings)
#
# Layer rule: script -> editor is allowed; script must never include Renderer or
# UI headers. Python/pybind11 headers are confined to implementation sources:
# the public ScriptEngine.h / ScriptContext.h stay Python-free, and the binding
# headers only forward-declare pybind11 types.
#
# Explicit source list (no glob), matching the other layer libraries.

add_library(neurus_script STATIC
	ScriptContext.h
	ScriptContext.cpp
	ScriptEngine.h
	ScriptEngine.cpp
	ScriptEngineImpl.h
	bindings/Bindings.h
	bindings/ProxyBase.h
	bindings/Runtime.h
	bindings/Runtime.cpp
	bindings/ObjectBindings.cpp
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
