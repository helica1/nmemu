#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "synthLib/plugin.h"

#include "nmmLib/nmmdevice.h"
#include "nmmLib/nmmpatch.h"

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

		// patches
		bool loadPatchFile(const juce::File& _file, std::string& _error);
		bool loadPatchText(const std::string& _text, const std::string& _name, std::string& _error);
		std::string getPatchName() const;
		std::string getPatchStatus() const;

	private:
		void createDevice();
		void servicePatchUpload(int _numHostSamples);
		void onMidiOut(const synthLib::SMidiEvent& _ev);

		std::unique_ptr<Device> m_device;
		std::unique_ptr<synthLib::Plugin> m_plugin;

		std::string m_osFile;
		std::string m_bootRomFile;
		std::string m_deviceError;
		std::vector<std::string> m_searchedDirs;

		std::array<uint8_t, 4> m_knobs{0xff, 0xff, 0xff, 0xff};

		std::vector<synthLib::SMidiEvent> m_midiOut;

		// The patch upload follows the editor: send "I am", wait for the synth's reply (which only
		// arrives once its OS has finished booting), then send the packets with a little spacing.
		struct PatchUpload
		{
			enum class State { Idle, WaitIAm, Sending, Done, Failed };
			State state = State::Idle;
			std::vector<std::vector<uint8_t>> frames;
			size_t next = 0;
			double nextSendSample = 0;
			double retrySample = 0;
			int retries = 0;
			int acks = 0;
		};
		mutable std::mutex m_patchMutex;
		PatchUpload m_upload;
		Patch m_patch;
		std::string m_patchText;
		std::string m_patchName;
		std::string m_patchFile;
		bool m_hasPatch = false;
		double m_hostSamplesProcessed = 0;
		double m_hostSamplerate = 48000.0;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
