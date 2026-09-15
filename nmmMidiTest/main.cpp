#include <cstdio>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "nmmLib/nmmpatch.h"

namespace
{
	struct Receiver : juce::MidiInputCallback
	{
		std::mutex mutex;
		std::vector<std::vector<uint8_t>> messages;

		void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& _m) override
		{
			std::vector<uint8_t> raw(_m.getRawData(), _m.getRawData() + _m.getRawDataSize());
			std::lock_guard lock(mutex);
			messages.push_back(std::move(raw));
		}

		bool waitFor(const std::function<bool(const std::vector<uint8_t>&)>& _pred, const int _timeoutMs)
		{
			const auto t0 = std::chrono::steady_clock::now();
			while(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(_timeoutMs))
			{
				{
					std::lock_guard lock(mutex);
					for (const auto& m : messages)
						if(_pred(m)) return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return false;
		}
	};

	void print(const char* _prefix, const std::vector<uint8_t>& _m)
	{
		std::printf("%s", _prefix);
		for(size_t i=0; i<_m.size() && i<24; ++i) std::printf(" %02x", _m[i]);
		if(_m.size() > 24) std::printf(" ... (%zu bytes)", _m.size());
		std::printf("\n");
	}
}

int main(int _argc, char** _argv)
{
	juce::ScopedJuceInitialiser_GUI juceInit;

	const std::string portName = _argc > 2 ? _argv[2] : "Nord Micro Modular (emulator)";

	std::printf("MIDI inputs:\n");
	for (const auto& d : juce::MidiInput::getAvailableDevices()) std::printf("  %s\n", d.name.toRawUTF8());
	std::printf("MIDI outputs:\n");
	for (const auto& d : juce::MidiOutput::getAvailableDevices()) std::printf("  %s\n", d.name.toRawUTF8());

	juce::MidiDeviceInfo inInfo, outInfo;
	for (const auto& d : juce::MidiInput::getAvailableDevices()) if(d.name == juce::String(portName)) inInfo = d;
	for (const auto& d : juce::MidiOutput::getAvailableDevices()) if(d.name == juce::String(portName)) outInfo = d;
	if(inInfo.identifier.isEmpty() || outInfo.identifier.isEmpty())
	{
		std::printf("port '%s' not found, is the emulator running?\n", portName.c_str());
		return 1;
	}

	Receiver rx;
	auto in = juce::MidiInput::openDevice(inInfo.identifier, &rx);
	auto out = juce::MidiOutput::openDevice(outInfo.identifier);
	if(!in || !out) { std::printf("failed to open the ports\n"); return 1; }
	in->start();

	auto send = [&](const std::vector<uint8_t>& _syx)
	{
		print("-> ", _syx);
		out->sendMessageNow(juce::MidiMessage::createSysExMessage(_syx.data() + 1, static_cast<int>(_syx.size()) - 2));
	};

	// I am
	const auto isIAmReply = [](const std::vector<uint8_t>& m) { return m.size() >= 5 && m[1] == 0x33 && ((m[2] >> 2) & 0x1f) == 0 && m[4] == 0x01; };
	bool ok = false;
	for(int attempt=0; attempt<8 && !ok; ++attempt)
	{
		send(nmm::PatchSysex::iAm());
		ok = rx.waitFor(isIAmReply, 2000);
	}
	std::printf("I am reply: %s\n", ok ? "received" : "NOT received");
	if(!ok) return 1;

	if(_argc > 1)
	{
		nmm::Patch patch;
		std::string err;
		if(!nmm::PchFile::load(_argv[1], patch, err)) { std::printf("failed to load patch: %s\n", err.c_str()); return 1; }
		patch.routeOutputsToMain();
		const auto frames = nmm::PatchSysex::upload(patch, 0);
		std::printf("uploading '%s': %zu packets\n", patch.name.c_str(), frames.size());
		for (const auto& f : frames)
		{
			send(f);
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		const auto isAck = [](const std::vector<uint8_t>& m) { return m.size() >= 4 && m[1] == 0x33 && ((m[2] >> 2) & 0x1f) == 0x16; };
		rx.waitFor([&](const std::vector<uint8_t>&) { return false; }, 4000);
		{ std::lock_guard lock(rx.mutex); for (const auto& m : rx.messages) print("<- ", m); }
		int acks = 0;
		{
			std::lock_guard lock(rx.mutex);
			for (const auto& m : rx.messages) if(isAck(m)) ++acks;
			std::printf("received %zu messages, %d ACKs\n", rx.messages.size(), acks);
		}
		return acks >= static_cast<int>(frames.size()) ? 0 : 1;
	}
	return 0;
}
