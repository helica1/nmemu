cmake_minimum_required(VERSION 3.16)

# JUCE plugin shell around nmm::Device. Deliberately plain JUCE (no gearmulator jucePluginLib
# / RmlUi skins) so it builds with the two 3rdparty submodules only: CLAP + VST3 + AU.

project(nmmPlugin VERSION 0.1.0)

set(NMM_PLUGIN_FORMATS VST3)
if(APPLE)
	list(APPEND NMM_PLUGIN_FORMATS AU)
endif()
if(NMEMU_BUILD_STANDALONE)
	list(APPEND NMM_PLUGIN_FORMATS Standalone)
endif()

juce_add_plugin(nmmPlugin
	COMPANY_NAME "nmemu"
	IS_SYNTH TRUE
	NEEDS_MIDI_INPUT TRUE
	NEEDS_MIDI_OUTPUT TRUE
	IS_MIDI_EFFECT FALSE
	EDITOR_WANTS_KEYBOARD_FOCUS FALSE
	COPY_PLUGIN_AFTER_BUILD FALSE
	PLUGIN_MANUFACTURER_CODE Nmem
	PLUGIN_CODE Nmm1
	FORMATS ${NMM_PLUGIN_FORMATS}
	PRODUCT_NAME "NordMicroModular"
	BUNDLE_ID "org.nmemu.NordMicroModular"
	VST3_AUTO_MANIFEST FALSE
	PRODUCTS_FOLDER "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>"
)

set(SOURCES
	PluginProcessor.cpp PluginProcessor.h
	PluginEditor.cpp PluginEditor.h
	RomFinder.cpp RomFinder.h
	VirtualMidi.cpp VirtualMidi.h
	EditorWindow.cpp EditorWindow.h
)

target_sources(nmmPlugin PRIVATE ${SOURCES})
source_group("source" FILES ${SOURCES})

target_compile_definitions(nmmPlugin PUBLIC
	JUCE_WEB_BROWSER=0
	JUCE_USE_CURL=0
	JUCE_VST3_CAN_REPLACE_VST2=0
	JUCE_DISPLAY_SPLASH_SCREEN=0
	JUCE_USE_OGGVORBIS=0
	JUCE_USE_MP3AUDIOFORMAT=0
	JUCE_USE_FLAC=0
)

if(NMEMU_EDITOR)
	target_compile_definitions(nmmPlugin PRIVATE NMM_EMBEDDED_EDITOR=1)
	target_link_libraries(nmmPlugin PRIVATE nmmEditor)
else()
	target_compile_definitions(nmmPlugin PRIVATE NMM_EMBEDDED_EDITOR=0)
endif()

target_link_libraries(nmmPlugin
	PRIVATE
		nmmLib
		synthLib
		juce::juce_audio_utils
		juce::juce_audio_devices
		juce::juce_audio_processors
	PUBLIC
		juce::juce_recommended_config_flags
		juce::juce_recommended_lto_flags
		juce::juce_recommended_warning_flags
)

if(TARGET clap_juce_extensions)
	clap_juce_extensions_plugin(TARGET nmmPlugin
		CLAP_ID "org.nmemu.nordmicromodular"
		CLAP_FEATURES instrument synthesizer
		CLAP_SUPPORT_URL "https://github.com/helica1/nmemu"
		CLAP_MANUAL_URL "https://github.com/helica1/nmemu"
	)
endif()
