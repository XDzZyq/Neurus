# src/script/CMakeLists.txt - Script layer (embedded CPython + pybind11 bindings)
#
# Layer rule: the script layer reaches the editor only through the editor's typed
# events (it includes the events headers and emits through the queue); it must
# never include the Editor or Renderer/UI headers. Python/pybind11 headers are
# confined to implementation sources: the layer headers (ScriptEngine.hpp,
# ScriptContext.hpp, ScriptPolicy.hpp, ScriptDispatch.hpp, ScriptView.hpp, proxies/)
# stay Python-free, ScriptSandbox.hpp and ScriptImports.hpp are included only by
# ScriptEngineImpl.hpp, and bindings/ holds only the pybind11 registration units
# plus the forward-declaring Bindings.hpp.
#
# Explicit source list (no glob), matching the other layer libraries.

add_library(neurus_script STATIC
	ScriptConsole.hpp
	ScriptContext.hpp
	ScriptDispatch.hpp
	ScriptEngine.hpp
	ScriptEngine.cpp
	ScriptEngineImpl.hpp
	ScriptImports.hpp
	ScriptImports.cpp
	ScriptPolicy.hpp
	ScriptPolicy.cpp
	ScriptSandbox.hpp
	ScriptSandbox.cpp
	ScriptView.hpp
	proxies/ProxyBase.hpp
	proxies/ObjectProxy.hpp
	proxies/Transform3DProxy.hpp
	proxies/SceneProxies.hpp
	proxies/ShaderProxies.hpp
	bindings/Bindings.hpp
	bindings/ObjectBindings.cpp
	bindings/DataBindings.cpp
	bindings/SceneBindings.cpp
	bindings/ConfigBindings.cpp
	bindings/ShaderBindings.hpp
	bindings/ShaderBindings.cpp
	bindings/Module.cpp
)

target_link_libraries(neurus_script PUBLIC
	neurus_editor
	neurus_core
	neurus_scene
	neurus_asset
	pybind11::embed
	Python::Python
)

target_include_directories(neurus_script PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

# The generated NEURUS_PYTHON_HOME header is an implementation detail of this
# library (only ScriptEngineImpl.hpp includes it).
target_include_directories(neurus_script PRIVATE
	${CMAKE_BINARY_DIR}/generated
)

target_compile_features(neurus_script PUBLIC cxx_std_20)
