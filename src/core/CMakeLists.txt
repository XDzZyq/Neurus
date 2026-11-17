# src/core/CMakeLists.txt - Core Type Definitions

# Threading primitives for ThreadPool (std::jthread) — portable pthread link.
find_package(Threads REQUIRED)

# STATIC (not INTERFACE) since UID and ResourceManager have .cpp files
# (s_count definition, non-template members). Consumers get the include dirs
# and glm/cereal transitively via PUBLIC.
add_library(neurus_core STATIC
	UID.hpp
	UID.cpp
	ResourceManager.hpp
	ResourceManager.cpp
	Serialize.hpp
	RenderConfig.hpp
	GraphMorphism.hpp
	RenderGraphSpec.hpp
	RenderGraphSpec.cpp
	ThreadPool.hpp
	ThreadPool.cpp
	BatchQueue.hpp
)

# src/ (${CMAKE_CURRENT_SOURCE_DIR}/..) exposes headers as "core/X.hpp".
target_include_directories(neurus_core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_core PUBLIC
	glm
	cereal
	Threads::Threads
	# Log.hpp's TTY check lives in the platform layer, so every translation unit
	# that expands NEURUS_LOG/NEURUS_ERR needs its symbols.
	neurus_platform_paths
)
