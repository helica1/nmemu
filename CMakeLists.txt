cmake_minimum_required(VERSION 3.16)

# Nord Modular / Micro Modular emulator built on the gearmulator libraries
# (dsp56300 core, mc68k core, synthLib, hardwareLib).

project(nmemu VERSION 0.1.0)

set(GEARMULATOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../gearmulator CACHE PATH "Path to a gearmulator checkout")
set(GM_SRC ${GEARMULATOR_DIR}/source)

include(${GM_SRC}/cmake/base.cmake)

set(ASMJIT_STATIC TRUE)
set(ASMJIT_NO_INSTALL TRUE)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)

option(NMEMU_FORCE_INTERPRETER "Disable the DSP JIT and use the interpreter (what iOS would run)" OFF)
if(NMEMU_FORCE_INTERPRETER)
	set(DSP56K_FORCE_INTERPRETER ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${GM_SRC}/cpu/dsp56300/source dsp56300)
add_subdirectory(${GM_SRC}/framework/baseLib baseLib)
add_subdirectory(${GM_SRC}/3rdparty/libresample libresample)
add_subdirectory(${GM_SRC}/framework/synthLib synthLib)
add_subdirectory(${GM_SRC}/cpu/mc68k mc68k)
target_include_directories(68kEmu PUBLIC ${GM_SRC}/cpu)
add_subdirectory(${GM_SRC}/framework/hardwareLib hardwareLib)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nmmLib/CMakeLists.txt)
	add_subdirectory(nmmLib)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nmmConsole/CMakeLists.txt)
	add_subdirectory(nmmConsole)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nmmDspTest/CMakeLists.txt)
	add_subdirectory(nmmDspTest)
endif()

option(NMEMU_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/CLAP)" ON)
option(NMEMU_BUILD_STANDALONE "Also build the standalone app" OFF)

if(NMEMU_BUILD_PLUGIN AND EXISTS ${GM_SRC}/3rdparty/JUCE/CMakeLists.txt)
	set(JUCE_ENABLE_MODULE_SOURCE_GROUPS ON CACHE BOOL "" FORCE)
	add_subdirectory(${GM_SRC}/3rdparty/JUCE JUCE EXCLUDE_FROM_ALL)
	if(EXISTS ${GM_SRC}/3rdparty/clap-juce-extensions/clap-libs/clap/CMakeLists.txt)
		add_subdirectory(${GM_SRC}/3rdparty/clap-juce-extensions clap-juce-extensions EXCLUDE_FROM_ALL)
	else()
		message(WARNING "clap-juce-extensions submodules missing, CLAP format disabled")
	endif()
	add_subdirectory(nmmPlugin)
endif()
