if (MSVC)
	add_compile_options(
		"$<$<CONFIG:DEBUG>:/W4>"
		"$<$<CONFIG:RELEASE>:/O2>"
	)
else()
	add_compile_options(
		-Wall 
		# -Wpedantic -Wextra -Wstrict-aliasing -Wunreachable-code -Weffc++ -Wno-unused-parameter
		"$<$<CONFIG:DEBUG>:-Og;-ggdb>"
		"$<$<CONFIG:RELWITHDEBINFO>:-O3>"
		"$<$<CONFIG:RELEASE>:-O3>"
	)
endif()


if (CMAKE_SYSTEM_PROCESSOR MATCHES "(amd64)|(AMD64)|(x86_64)")
	option(USE_NATIVE_ARCH "Enable architecture-specific optimizations" OFF)

	if (USE_NATIVE_ARCH)
		if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
			add_compile_options(/arch:AVX2 /fp:fast)
			message(STATUS "Enabling /arch:AVX2")		 
		else()
			add_compile_options(-march=x86-64-v3)
			message(STATUS "Enabling -march=x86-64-v3")
		endif()
	else()
		set(MULTIFRAME_8X8_CONVOLUTION OFF CACHE BOOL "0" FORCE) 
	endif (USE_NATIVE_ARCH)
endif ()

add_subdirectory(../deps/NeuralAudio NeuralAudio)

# DSP block options (declared in the top-level CMakeLists, before add_subdirectory,
# because the plugin name/URI suffix depends on them). Here they select the code
# that actually gets compiled and the sources that go into the binary.
if (ENABLE_CAB)
	add_definitions(-DENABLE_CAB)
	message(STATUS "Cab IR block enabled")
endif()

if (ENABLE_DC_BLOCK)
	add_definitions(-DENABLE_DC_BLOCK)
	message(STATUS "DC blocker enabled")
endif()

if (ENABLE_EQ)
	add_definitions(-DENABLE_EQ)
	message(STATUS "3-band EQ enabled")
endif()

set(SOURCES nam_lv2.cpp
	nam_plugin.cpp
	nam_plugin.h)

if (ENABLE_CAB)
	list(APPEND SOURCES cab_convolver.cpp cab_convolver.h)
endif()

if (ENABLE_EQ)
	list(APPEND SOURCES eq.h)
endif()


set(NA_SOURCES ../deps/NeuralAudio/NeuralAudio/NeuralModel.h)

add_library(${BINARY_NAME} SHARED ${SOURCES} ${NA_SOURCES})

target_include_directories(${BINARY_NAME} PUBLIC ../deps/NeuralAudio)
target_include_directories(${BINARY_NAME} PUBLIC ../deps/lv2/include)
target_include_directories(${BINARY_NAME} PUBLIC ../deps/denormal)

target_link_libraries(${BINARY_NAME} PRIVATE NeuralAudio)

# inject the full plugin URI (follows the dynamic name suffix) into the code
target_compile_definitions(${BINARY_NAME} PRIVATE "PLUGIN_URI=\"${NAM_LV2_ID}\"")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})
source_group(NAM ${CMAKE_CURRENT_SOURCE_DIR} FILES ${NA_SOURCES})

option(DISABLE_DENORMALS "Disable floating point denormals" ON)

if (DISABLE_DENORMALS)
	add_definitions(-DDISABLE_DENORMALS)
endif (DISABLE_DENORMALS)

option(SMART_BYPASS_ENABLED "Enable auto-bypass on silence" OFF)

if (SMART_BYPASS_ENABLED)
	add_definitions(-DSMART_BYPASS_ENABLED)
	message(STATUS "Smart Bypass enabled")
else()
	message(STATUS "Smart Bypass NOT enabled")
endif (SMART_BYPASS_ENABLED)

set_target_properties(${BINARY_NAME}
	PROPERTIES
	CXX_VISIBILITY_PRESET hidden
	INTERPROCEDURAL_OPTIMIZATION TRUE
	PREFIX ""
)

# Platform

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_compile_definitions(${BINARY_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
