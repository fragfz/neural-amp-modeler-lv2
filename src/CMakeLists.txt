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

set(SOURCES nam_lv2.cpp
	nam_plugin.cpp
	nam_plugin.h)

set(NA_SOURCES ../deps/NeuralAudio/NeuralAudio/NeuralModel.h)

add_library(neural_amp_modeler_dual SHARED ${SOURCES} ${NA_SOURCES})

target_include_directories(neural_amp_modeler_dual PUBLIC ../deps/NeuralAudio)
target_include_directories(neural_amp_modeler_dual PUBLIC ../deps/lv2/include)
target_include_directories(neural_amp_modeler_dual PUBLIC ../deps/denormal)

target_link_libraries(neural_amp_modeler_dual PRIVATE NeuralAudio)

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

option(ENABLE_DC_BLOCK "Enable DC blocker at the end of the chain" ON)

if (ENABLE_DC_BLOCK)
	add_definitions(-DENABLE_DC_BLOCK)
	message(STATUS "DC blocker enabled")
else()
	message(STATUS "DC blocker NOT enabled")
endif (ENABLE_DC_BLOCK)

option(ENABLE_EQ "Enable 3-band EQ at the end of the chain" ON)

if (ENABLE_EQ)
	add_definitions(-DENABLE_EQ)
	message(STATUS "EQ enabled")

	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../resources/eq_ports.ttl.in" EQ_PORTS)
	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../resources/eq_knobs.ttl.in" EQ_KNOBS)
	set(QUALITY_INDEX 13)
else()
	message(STATUS "EQ NOT enabled")

	set(EQ_PORTS "")
	set(EQ_KNOBS "")
	set(QUALITY_INDEX 10)
endif (ENABLE_EQ)

set_target_properties(neural_amp_modeler_dual
	PROPERTIES
	CXX_VISIBILITY_PRESET hidden
	INTERPROCEDURAL_OPTIMIZATION TRUE
	PREFIX ""
)

# Platform

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_compile_definitions(neural_amp_modeler_dual PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
