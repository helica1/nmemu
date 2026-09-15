#pragma once

#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>

#include "nmmtypes.h"
#include "nmmmc.h"
#include "nmmdsp.h"
#include "nmmromloader.h"

#include "synthLib/audioTypes.h"
#include "synthLib/midiTypes.h"

#include "dsp56kBase/ringbuffer.h"
#include "dsp56kBase/semaphore.h"

namespace nmm
{
	struct HardwareConfig
	{
		std::vector<uint8_t> bootRom;		// 512K boot flash image, optional. Without it the OS is booted directly (HLE)
		OsImage os;							// required
		std::vector<uint32_t> dspSlots;		// host port slots that have a DSP56303 behind them
		std::string flashFile;				// optional persisted OS/patch flash image
	};

	/*
	Timing model, following gearmulator's Nord Lead 2x:

	- The DSP runs in its own thread. Every completed ESSI0 transmit frame is one sample period.
	  The frame callback blocks the DSP on a counting semaphore that the audio thread refills
	  with the number of frames the host asked for, so the DSP never runs ahead of the host.
	- The 68k runs in its own thread and is granted system clock cycles in proportion to the
	  DSP frames that have passed, so the two stay in step. Before the DSP produces frames it
	  runs freely, that is how it gets through the boot ROM and OS init.
	- MIDI events are queued with a sample offset and pushed into the emulated UARTs from the
	  frame callback.
	*/
	class Hardware
	{
	public:
		using AudioOutputs = std::array<std::vector<dsp56k::TWord>, 2>;

		explicit Hardware(const HardwareConfig& _config);
		~Hardware();

		bool isValid() const { return m_valid; }

		Microcontroller& getUC() { return *m_uc; }
		const Microcontroller& getUC() const { return *m_uc; }

		size_t getDspCount() const { return m_dsps.size(); }
		DSP& getDSP(const size_t _i) { return *m_dsps[_i]; }
		DSP* getMasterDSP() { return m_dsps.empty() ? nullptr : m_dsps.front().get(); }

		// audio thread: produce _frames output frames and consume _frames input frames
		void processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, uint32_t _frames, uint32_t _latency);

		// raw 24 bit DAC words of the last processAudio call, left = ESSI0 slot 0, right = ESSI1 slot 0
		const AudioOutputs& getAudioOutputs() const { return m_audioOutputs; }

		// MIDI: sysex goes to the PC port (the editor connection), everything else to MIDI IN.
		// The offset is in samples relative to the frames processed so far.
		bool sendMidi(const synthLib::SMidiEvent& _ev);
		void readMidiOut(std::vector<uint8_t>& _midiOut, std::vector<uint8_t>& _pcOut);

		void setKnob(const uint32_t _index, const uint8_t _value) { m_uc->setKnob(_index, _value); }

		bool usesBootRom() const { return m_usesBootRom; }
		bool osStarted() const { return m_osStarted; }
		bool bootFinished() const { return m_bootFinished; }

		void onDspBooted(DSP& _dsp);

		uint64_t getEssiFrameCount() const { return m_essiFrameIndex; }
		uint64_t getIrqdInjected() const { return m_irqdInjected; }
		uint64_t getIrqdMasked() const { return m_irqdMasked; }

		void setDspTraceFrames(const uint32_t _frames) { m_dspTraceFrames = _frames; }

		void haltDSPs();
		void resumeDSPs();
		bool requestingHaltDSPs() const { return m_dspHalted; }

		// bring-up helpers: sample the DSP PC on every frame
		void enableDspProfile(const bool _e) { m_dspProfile = _e; }
		const std::unordered_map<uint32_t, uint64_t>& dspPcHistogram() const { return m_dspPcHist; }

	private:
		void hleBoot();
		void ucThreadFunc();
		void processUC();
		void syncUCtoDSP();
		void onEssiFrame();
		void processMidiInput();
		void advanceSamples(uint32_t _samples, uint32_t _latency);
		void ensureBufferSize(uint32_t _frames);

		bool m_valid = false;
		bool m_usesBootRom = false;
		std::atomic<bool> m_osStarted{false};
		std::atomic<bool> m_bootFinished{false};

		std::unique_ptr<Microcontroller> m_uc;
		std::vector<std::unique_ptr<DSP>> m_dsps;

		// timing
		const double m_samplerateInv;
		std::atomic<uint64_t> m_essiFrameIndex{0};
		uint64_t m_lastEssiFrameIndex = 0;
		int64_t m_remainingUcCycles = 0;
		double m_remainingUcCyclesD = 0;
		std::mutex m_essiFrameAddedMutex;
		dsp56k::ConditionVariable m_essiFrameAddedCv;
		std::mutex m_requestedFramesAvailableMutex;
		std::condition_variable m_requestedFramesAvailableCv;	// std: needs a timed wait for the stall diagnostic
		size_t m_requestedFrames = 0;
		bool m_dspHalted = false;
		dsp56k::SpscSemaphoreWithCount m_haltDSPSem;
		uint32_t m_essiLatency = 0;
		int32_t m_dspNotifyCorrection = 0;

		std::unique_ptr<std::thread> m_ucThread;
		std::atomic<bool> m_destroy{false};

		// MIDI
		dsp56k::RingBuffer<synthLib::SMidiEvent, 16384, true> m_midiIn;
		uint32_t m_midiOffsetCounter = 0;

		// audio buffers
		AudioOutputs m_audioOutputs;
		std::vector<float> m_dummyInput;

		std::atomic<uint32_t> m_dspTraceFrames{0};
		uint64_t m_dspTraceAtFrame = 0;
		uint32_t m_dspTraceAtCount = 0;
		std::atomic<uint64_t> m_irqdInjected{0};
		std::atomic<uint64_t> m_irqdMasked{0};
		bool m_dspProfile = false;
		std::unordered_map<uint32_t, uint64_t> m_dspPcHist;
	};
}
