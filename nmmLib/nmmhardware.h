#pragma once

#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <unordered_map>

#include "nmmtypes.h"
#include "nmmmc.h"
#include "nmmdsp.h"
#include "nmmromloader.h"

namespace nmm
{
	struct HardwareConfig
	{
		std::vector<uint8_t> bootRom;		// 512K boot flash image, optional. Without it the OS is booted directly (HLE)
		OsImage os;							// required
		std::vector<uint32_t> dspSlots;		// host port slots that have a DSP56303 behind them
		std::string flashFile;				// optional persisted OS/patch flash image
	};

	class Hardware
	{
	public:
		explicit Hardware(const HardwareConfig& _config);
		~Hardware();

		bool isValid() const { return m_valid; }

		Microcontroller& getUC() { return *m_uc; }
		const Microcontroller& getUC() const { return *m_uc; }

		size_t getDspCount() const { return m_dsps.size(); }
		DSP& getDSP(const size_t _i) { return *m_dsps[_i]; }

		// run one 68k instruction including peripherals
		void stepUC();

		// run the 68k for a number of instructions (free running, no audio pacing)
		void runUC(uint64_t _instructions);

		// Audio pump for free-running operation: keeps the DSP serial ports fed with silence and
		// drains whatever they produced. Call this regularly from the thread that runs the UC.
		// Captured output is appended to the capture buffers (4 channels: ESSI0 slot0/1, ESSI1 slot0/1).
		void serviceAudio();

		// audio capture, 24 bit DSP words sign extended to int32
		const std::vector<std::vector<int32_t>>& getCapture() const { return m_capture; }
		void setCaptureLimitFrames(const size_t _frames) { m_captureLimit = _frames; }
		void clearCapture() { for (auto& c : m_capture) c.clear(); }

		static constexpr uint32_t g_captureChannels = 4;

		// sample the first DSP's PC on every serviceAudio call
		void enableDspProfile(const bool _e) { m_dspProfile = _e; }
		const std::unordered_map<uint32_t, uint64_t>& dspPcHistogram() const { return m_dspPcHist; }

		bool usesBootRom() const { return m_usesBootRom; }
		bool osStarted() const { return m_osStarted; }

		void onDspBooted(DSP& _dsp);

		hwLib::SciMidi& getMidi() { return m_uc->getMidi(); }		// the MIDI IN/OUT pair
		PcPort& getPcPort() { return m_uc->getPcPort(); }			// the PC IN/OUT pair, used by the editor

		uint64_t getEssiFrameCount() const { return m_essiFrames; }
		uint64_t getIrqdInjected() const { return m_irqdInjected; }
		uint64_t getIrqdMasked() const { return m_irqdMasked; }

	private:
		void hleBoot();
		void onEssiFrame();

		bool m_valid = false;
		bool m_usesBootRom = false;
		bool m_osStarted = false;

		std::unique_ptr<Microcontroller> m_uc;
		std::vector<std::unique_ptr<DSP>> m_dsps;

		static void topUpInput(dsp56k::Essi& _essi, size_t _targetFrames);
		void drainOutput(dsp56k::Essi& _essi, uint32_t _channelBase);

		std::atomic<uint64_t> m_essiFrames{0};
		std::atomic<uint64_t> m_irqdInjected{0};
		std::atomic<uint64_t> m_irqdMasked{0};
		bool m_dspProfile = false;
		uint32_t m_lastP17 = 0xffffffff;
		uint32_t m_lastHcr = 0;
		std::unordered_map<uint32_t, uint64_t> m_dspPcHist;
		std::vector<std::vector<int32_t>> m_capture = std::vector<std::vector<int32_t>>(g_captureChannels);
		size_t m_captureLimit = 96000 * 60;
	};
}
