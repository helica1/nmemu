cmake_minimum_required(VERSION 3.16)
project(nmmMidiTest VERSION 0.1.0)

# Console tool that talks to a running emulator through its virtual MIDI ports the way an
# editor does: says "I am", waits for the synth's reply, uploads a .pch and counts the ACKs.
juce_add_console_app(nmmMidiTest PRODUCT_NAME "nmmMidiTest")
target_sources(nmmMidiTest PRIVATE main.cpp)
target_compile_definitions(nmmMidiTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(nmmMidiTest PRIVATE nmmLib juce::juce_audio_devices juce::juce_core juce::juce_events juce::juce_recommended_config_flags)
