cmake_minimum_required(VERSION 3.10)

project(NeuralAmpModelerLv2 VERSION 0.2.3)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(LIB_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib CACHE PATH "The library install dir (default: lib)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	include_directories(SYSTEM /usr/local/include)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	link_libraries( "$<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:-lstdc++fs>" )
elseif (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
	message(FATAL_ERROR "Unrecognized Platform!")
endif()

# Test-variant DSP block options: also declared in src/CMakeLists.txt, but
# needed here (before add_subdirectory) to compute the plugin name suffix.
option(ENABLE_CAB "Enable cab IR convolution block (after the NAM)" ON)
option(ENABLE_DC_BLOCK "Enable DC blocker at the end of the chain" ON)
option(ENABLE_EQ "Enable 3-band EQ at the end of the chain" ON)

# Dynamic test-plugin identity: always _T, plus _C/_D/_E for each enabled
# DSP block, so differently-configured builds can coexist on the MOD.
# e.g. cab + EQ => neural_amp_modeler_tce, all three => neural_amp_modeler_tcde
set(NAM_NAME_SUFFIX "_T")

if (ENABLE_CAB)
	string(APPEND NAM_NAME_SUFFIX "C")
endif()

if (ENABLE_DC_BLOCK)
	string(APPEND NAM_NAME_SUFFIX "D")
endif()

if (ENABLE_EQ)
	string(APPEND NAM_NAME_SUFFIX "E")
endif()

string(TOLOWER "${NAM_NAME_SUFFIX}" NAM_NAME_SUFFIX_LOWER)

set(NAM_LV2_ID http://github.com/fragfz/neural-amp-modeler-lv2${NAM_NAME_SUFFIX_LOWER})
set(BINARY_NAME neural_amp_modeler${NAM_NAME_SUFFIX_LOWER})

# human-readable variant suffix for the plugin display name (space, no underscore)
string(REPLACE "_" " " NAM_NAME_DISPLAY "${NAM_NAME_SUFFIX}")

add_subdirectory(src)

# --- TTL fragment substitution for the optional DSP blocks ------------------
# Port index layout (append-only; must match NAM::Plugin::Ports in nam_plugin.h):
#   0 control, 1 notify, 2 input, 3 output, 4 input_level, 5 output_level,
#   6 quality_scale, 7 cab_enable (cab), 8/9/10 eq_bass/mid/treble (eq).
set(CAB_PORTS "")
set(CAB_KNOBS "")
set(CAB_PARAM_DECL "")
set(CAB_WRITABLE "")
set(EQ_PORTS "")
set(EQ_KNOBS "")

if (ENABLE_CAB)
	file(READ "${CMAKE_SOURCE_DIR}/resources/cab_ports.ttl.in" CAB_PORTS)
	file(READ "${CMAKE_SOURCE_DIR}/resources/cab_knobs.ttl.in" CAB_KNOBS)

	set(CAB_PARAM_DECL "
<${NAM_LV2_ID}#cab>
	a lv2:Parameter;
	mod:fileTypes \"cabsim\";
	rdfs:label \"Cab IR\";
	rdfs:range atom:Path.
")

	set(CAB_WRITABLE "
	patch:writable <${NAM_LV2_ID}#cab>;")
endif()

if (ENABLE_EQ)
	set(EQ_BASE 7)

	if (ENABLE_CAB)
		set(EQ_BASE 8)
	endif()

	set(EQ_I0 ${EQ_BASE})
	math(EXPR EQ_I1 "${EQ_BASE} + 1")
	math(EXPR EQ_I2 "${EQ_BASE} + 2")

	file(READ "${CMAKE_SOURCE_DIR}/resources/eq_ports.ttl.in" EQ_PORTS_RAW)
	file(READ "${CMAKE_SOURCE_DIR}/resources/eq_knobs.ttl.in" EQ_KNOBS_RAW)

	string(CONFIGURE "${EQ_PORTS_RAW}" EQ_PORTS @ONLY)
	string(CONFIGURE "${EQ_KNOBS_RAW}" EQ_KNOBS @ONLY)
endif()

# out_calibrated comes after all optional DSP block ports (append-only)
set(OUT_CAL_INDEX 7)

if (ENABLE_CAB)
	math(EXPR OUT_CAL_INDEX "${OUT_CAL_INDEX} + 1")
endif()

if (ENABLE_EQ)
	math(EXPR OUT_CAL_INDEX "${OUT_CAL_INDEX} + 3")
endif()

set(OUT_KNOB_INDEX ${OUT_CAL_INDEX})

# create ${BINARY_NAME}.lv2
add_custom_target(copy_binaries ALL
	${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${BINARY_NAME}>" ${BINARY_NAME}.lv2/
	DEPENDS ${BINARY_NAME}
)

file(COPY resources/modgui DESTINATION ${BINARY_NAME}.lv2)

configure_file(resources/modgui.ttl ${BINARY_NAME}.lv2/modgui.ttl @ONLY)
configure_file(resources/manifest.ttl.in ${BINARY_NAME}.lv2/manifest.ttl)
configure_file(resources/neural_amp_modeler.ttl.in ${BINARY_NAME}.lv2/${BINARY_NAME}.ttl)

install (DIRECTORY ${CMAKE_BINARY_DIR}/${BINARY_NAME}.lv2
       DESTINATION ${LIB_INSTALL_DIR}/lv2
)

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "oliphant@nostatic.org>")
set(CPACK_PACKAGE_VENDOR "Mike Oliphant")
set(CPACK_PACKAGE_DESCRIPTION "Neural Amp Modeler LV2 plugin implementation")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Neural Amp Modeler LV2 plugin")
set(CPACK_DEBIAN_PACKAGE_SECTION sound)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set (CPACK_STRIP_FILES ON)

include(CPack)
