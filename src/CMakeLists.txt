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

option(ENABLE_CAB "Enable cab IR convolution block (after NAM 2)" ON)

if (ENABLE_CAB)
	add_definitions(-DENABLE_CAB)
	message(STATUS "Cab IR block enabled")

	target_sources(neural_amp_modeler_dual PRIVATE
		3rdparty/FFTConvolver/AudioFFT.cpp
		3rdparty/FFTConvolver/FFTConvolver.cpp
		3rdparty/FFTConvolver/TwoStageFFTConvolver.cpp
		3rdparty/FFTConvolver/Utilities.cpp
		cab_convolver.cpp)

	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../resources/cab_ports.ttl.in" CAB_PORTS)
	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../resources/cab_knobs.ttl.in" CAB_KNOBS)
else()
	message(STATUS "Cab IR block NOT enabled")

	set(CAB_PORTS "")
	set(CAB_KNOBS "")
endif (ENABLE_CAB)

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

# quality port index: first control port after the NAM blocks is 10 (indices
# 4..9 are the NAM controls), +1 for the cab enable, +3 for the EQ
set(QUALITY_INDEX 10)

if (ENABLE_CAB)
	math(EXPR QUALITY_INDEX "${QUALITY_INDEX} + 1")
endif()

if (ENABLE_EQ)
	add_definitions(-DENABLE_EQ)
	message(STATUS "EQ enabled")

	# the EQ port indices depend on whether the cab enable port is present
	set(EQ_BASE 10)

	if (ENABLE_CAB)
		math(EXPR EQ_BASE "${EQ_BASE} + 1")
	endif()

	math(EXPR EQ_I1 "${EQ_BASE} + 1")
	math(EXPR EQ_I2 "${EQ_BASE} + 2")

	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../resources/eq_ports.ttl.in" EQ_PORTS)
	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../resources/eq_knobs.ttl.in" EQ_KNOBS)

	string(REPLACE "@EQ_I0@" "${EQ_BASE}" EQ_PORTS "${EQ_PORTS}")
	string(REPLACE "@EQ_I1@" "${EQ_I1}" EQ_PORTS "${EQ_PORTS}")
	string(REPLACE "@EQ_I2@" "${EQ_I2}" EQ_PORTS "${EQ_PORTS}")
	string(REPLACE "@EQ_I0@" "${EQ_BASE}" EQ_KNOBS "${EQ_KNOBS}")
	string(REPLACE "@EQ_I1@" "${EQ_I1}" EQ_KNOBS "${EQ_KNOBS}")
	string(REPLACE "@EQ_I2@" "${EQ_I2}" EQ_KNOBS "${EQ_KNOBS}")

	math(EXPR QUALITY_INDEX "${QUALITY_INDEX} + 3")
else()
	message(STATUS "EQ NOT enabled")

	set(EQ_PORTS "")
	set(EQ_KNOBS "")
endif (ENABLE_EQ)

# Export the ttl template variables to the parent scope: the root
# CMakeLists.txt runs configure_file() on the ttl templates, and normal
# variables set here (in the add_subdirectory() child scope) are not
# visible there.
set(EQ_PORTS "${EQ_PORTS}" PARENT_SCOPE)
set(EQ_KNOBS "${EQ_KNOBS}" PARENT_SCOPE)
set(CAB_PORTS "${CAB_PORTS}" PARENT_SCOPE)
set(CAB_KNOBS "${CAB_KNOBS}" PARENT_SCOPE)
set(QUALITY_INDEX "${QUALITY_INDEX}" PARENT_SCOPE)

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
