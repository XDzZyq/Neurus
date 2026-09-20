# src/core/CMakeLists.txt - Core Type Definitions

# STATIC (not INTERFACE) since UID and ResourceManager have .cpp files
# (s_count definition, non-template members). Consumers get the include dirs
# and glm/cereal transitively via PUBLIC.
add_library(neurus_core STATIC
	UID.h
	UID.cpp
	ResourceManager.h
	ResourceManager.cpp
	Serialize.h
)

# src/ (${CMAKE_CURRENT_SOURCE_DIR}/..) exposes headers as "core/X.h".
target_include_directories(neurus_core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(neurus_core PUBLIC
	glm
	cereal
)
