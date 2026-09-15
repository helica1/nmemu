#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "synthLib/plugin.h"

#include "nmmLib/nmmdevice.h"

namespace nmm
{
	class AudioPluginAudioProcessor : public juce::AudioProcessor
	{
	public:
		AudioPluginAudioProcessor();
		~AudioPluginAudioProcessor() override;

		void prepareToPlay(double _sampleRate, int _samplesPerBlock) override;
		void releaseResources() override {}
		bool isBusesLayoutSupported(const BusesLayout& _layouts) const override;
		void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

		juce::AudioProcessorEditor* createEditor() override;
		bool hasEditor() const override { return true; }

		const juce::String getName() const override { return JucePlugin_Name; }
		bool acceptsMidi() const override { return true; }
		bool producesMidi() const override { return true; }
		bool isMidiEffect() const override { return false; }
		double getTailLengthSeconds() const override { return 0.0; }

		int getNumPrograms() override { return 1; }
		int getCurrentProgram() override { return 0; }
		void setCurrentProgram(int) override {}
		const juce::String getProgramName(int) override { return {}; }
		void changeProgramName(int, const juce::String&) override {}

		void getStateInformation(juce::MemoryBlock& _destData) override;
		void setStateInformation(const void* _data, int _sizeInBytes) override;

		// UI access
		bool isDeviceValid() const { return m_device && m_device->isValid(); }
		std::string getStatusText() const;
		void setKnob(uint32_t _index, uint8_t _value);
		uint8_t getKnob(uint32_t _index) const { return m_knobs[_index & 3]; }
		void sendSysexFile(const juce::File& _file);
		void sendSysex(const std::vector<uint8_t>& _bytes);

	private:
		void createDevice();

		std::unique_ptr<Device> m_device;
		std::unique_ptr<synthLib::Plugin> m_plugin;

		std::string m_osFile;
		std::string m_bootRomFile;
		std::string m_deviceError;
		std::vector<std::string> m_searchedDirs;

		std::array<uint8_t, 4> m_knobs{0xff, 0xff, 0xff, 0xff};

		std::vector<synthLib::SMidiEvent> m_midiOut;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
