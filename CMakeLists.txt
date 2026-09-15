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

option(NMEMU_EDITOR "Embed the Animatek NME patch editor into the plugin (needs the 3rdparty/Animatek-NME submodule)" ON)

set(NME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/Animatek-NME)
if(NMEMU_EDITOR AND NOT EXISTS ${NME_DIR}/JUCE/CMakeLists.txt)
	message(WARNING "Animatek NME submodule not checked out, building without the embedded editor")
	set(NMEMU_EDITOR OFF)
endif()

# The editor needs JUCE 8, its submodule brings one along. Without the editor, gearmulator's JUCE is used.
if(NMEMU_EDITOR)
	set(NMEMU_JUCE_DIR ${NME_DIR}/JUCE)
	set(NMEMU_CLAP_DIR ${NME_DIR}/libs/clap-juce-extensions)
else()
	set(NMEMU_JUCE_DIR ${GM_SRC}/3rdparty/JUCE)
	set(NMEMU_CLAP_DIR ${GM_SRC}/3rdparty/clap-juce-extensions)
endif()

if(NMEMU_BUILD_PLUGIN AND EXISTS ${NMEMU_JUCE_DIR}/CMakeLists.txt)
	add_subdirectory(${NMEMU_JUCE_DIR} JUCE EXCLUDE_FROM_ALL)
	if(EXISTS ${NMEMU_CLAP_DIR}/clap-libs/clap/CMakeLists.txt)
		set(CLAP_JUCE_EXTENSIONS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
		add_subdirectory(${NMEMU_CLAP_DIR} clap-juce-extensions EXCLUDE_FROM_ALL)
	else()
		message(WARNING "clap-juce-extensions submodules missing, CLAP format disabled")
	endif()
	if(NMEMU_EDITOR)
		add_subdirectory(nmmEditor)
	endif()
	add_subdirectory(nmmPlugin)
	add_subdirectory(nmmMidiTest)
endif()
