cmake_minimum_required(VERSION 3.16)

# The Animatek NME editor's shared sources as a static library, built from the lists in the
# editor's own CMakeLists.txt so that updating the submodule keeps this in step.

project(nmmEditor)

set(NME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/Animatek-NME)

file(STRINGS ${NME_DIR}/CMakeLists.txt NME_CMAKE_LINES)

# collect the entries of a set(...) / juce_add_binary_data(...) block line by line: the blocks
# contain comments with parentheses, so a regex over the whole file will not do
set(NME_SOURCES "")
set(NME_DATA "")
set(NME_VERSION "0.0.0")
set(NME_BLOCK "")
foreach(line ${NME_CMAKE_LINES})
	if(line MATCHES "^project\\(AnimatekNME VERSION ([0-9.]+)")
		set(NME_VERSION ${CMAKE_MATCH_1})
	endif()
	if(line MATCHES "^set\\(NME_SHARED_SOURCES")
		set(NME_BLOCK sources)
	elseif(line MATCHES "^juce_add_binary_data\\(NmeData SOURCES")
		set(NME_BLOCK data)
	elseif(NME_BLOCK AND line MATCHES "^\\)")
		set(NME_BLOCK "")
	elseif(NME_BLOCK STREQUAL "sources" AND line MATCHES "(source/[A-Za-z0-9_/.-]+\\.cpp)")
		list(APPEND NME_SOURCES ${CMAKE_MATCH_1})
	elseif(NME_BLOCK STREQUAL "data" AND line MATCHES "(data/[A-Za-z0-9_/.-]+)")
		list(APPEND NME_DATA ${CMAKE_MATCH_1})
	endif()
endforeach()

list(LENGTH NME_SOURCES NME_SOURCE_COUNT)
list(LENGTH NME_DATA NME_DATA_COUNT)
message(STATUS "Animatek NME ${NME_VERSION}: ${NME_SOURCE_COUNT} sources, ${NME_DATA_COUNT} data files")
if(NME_SOURCE_COUNT LESS 10 OR NME_DATA_COUNT LESS 1)
	message(FATAL_ERROR "could not read the source lists from ${NME_DIR}/CMakeLists.txt")
endif()

set(NME_SOURCE_PATHS "")
foreach(f ${NME_SOURCES})
	list(APPEND NME_SOURCE_PATHS ${NME_DIR}/${f})
endforeach()
set(NME_DATA_PATHS "")
foreach(f ${NME_DATA})
	list(APPEND NME_DATA_PATHS ${NME_DIR}/${f})
endforeach()

juce_add_binary_data(NmeData SOURCES ${NME_DATA_PATHS})
set_target_properties(NmeData PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(nmmEditor STATIC ${NME_SOURCE_PATHS})
set_target_properties(nmmEditor PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(nmmEditor PUBLIC ${NME_DIR}/source)

target_compile_definitions(nmmEditor PUBLIC
	NME_MCP_BRIDGE=0
	JUCE_WEB_BROWSER=0
	JUCE_USE_CURL=0
	JUCE_APPLICATION_NAME_STRING="Animatek NME"
	JUCE_APPLICATION_VERSION_STRING="${NME_VERSION}"
	JUCE_VST3_CAN_REPLACE_VST2=0
	JUCE_DISPLAY_SPLASH_SCREEN=0
	JUCE_MODAL_LOOPS_PERMITTED=1
)

target_link_libraries(nmmEditor PUBLIC
	NmeData
	juce::juce_core
	juce::juce_events
	juce::juce_data_structures
	juce::juce_graphics
	juce::juce_gui_basics
	juce::juce_gui_extra
	juce::juce_audio_basics
	juce::juce_audio_devices
	juce::juce_audio_processors
	juce::juce_audio_utils
	juce::juce_recommended_config_flags
)
