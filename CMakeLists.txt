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

add_subdirectory(src)


# create ${BINARY_NAME}.lv2
add_custom_target(copy_binaries ALL
	${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${BINARY_NAME}>" ${BINARY_NAME}.lv2/
	DEPENDS ${BINARY_NAME}
)

file(COPY resources/modgui.ttl DESTINATION ${BINARY_NAME}.lv2)
file(COPY resources/modgui DESTINATION ${BINARY_NAME}.lv2)

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
