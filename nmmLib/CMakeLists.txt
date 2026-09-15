cmake_minimum_required(VERSION 3.16)

project(nmmLib)

add_library(nmmLib STATIC)

set(SOURCES
	nmmtypes.h
	nmmlog.h
	nmmdspslots.cpp nmmdspslots.h
	nmmflash.cpp nmmflash.h
	nmmpanel.cpp nmmpanel.h
	nmmpcport.cpp nmmpcport.h
	nmmmc.cpp nmmmc.h
	nmmdsp.cpp nmmdsp.h
	nmmhardware.cpp nmmhardware.h
	nmmdevice.cpp nmmdevice.h
	nmmromloader.cpp nmmromloader.h
)

target_sources(nmmLib PRIVATE ${SOURCES})
source_group("source" FILES ${SOURCES})

target_link_libraries(nmmLib PUBLIC hardwareLib synthLib dsp56kEmu)

target_include_directories(nmmLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
