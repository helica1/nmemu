#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "nmmmoduledb.h"

namespace nmm
{
	/*
	Nord Modular G1 patch model, close to the wire format so it can be serialised for upload
	without any editor state. Section numbering follows the protocol: 0 = common area, 1 = poly area.
	*/
	struct PatchModule
	{
		int index = 0;				// container index, what cables and assignments refer to
		int type = 0;				// module type id (ModuleDesc::index)
		int x = 0, y = 0;
		std::string name;
		std::vector<int> params;	// "parameter" class values in dump order, one per ModuleDesc::params
		std::vector<int> customs;	// "custom" class values, one per ModuleDesc::customs
		const ModuleDesc* desc = nullptr;
	};

	struct PatchCable
	{
		int color = 0;
		int srcModule = 0, srcConn = 0;
		bool srcIsOutput = true;	// false = input chained to another input
		int dstModule = 0, dstConn = 0;
	};

	struct PatchHeader
	{
		int keyRangeMin = 0, keyRangeMax = 127;
		int velRangeMin = 0, velRangeMax = 127;
		int bendRange = 2;
		int portamentoTime = 0;
		bool portamento = false;
		int voices = 1;
		int separatorPosition = 4000;
		int octaveShift = 2;
		int voiceRetriggerPoly = 1, voiceRetriggerCommon = 1;
		int unknown1 = 0, unknown2 = 0, unknown3 = 0, unknown4 = 0;
		bool cableVis[7] = {true, true, true, true, true, true, true};
	};

	struct MorphAssignment { int section = 0, module = 0, param = 0, morph = 0, range = 0; };
	struct KnobAssignment { bool assigned = false; int section = 0, module = 0, param = 0; };
	struct CtrlAssignment { int control = 0, section = 0, module = 0, param = 0; };
	struct NoteSlot { int note = 64, attack = 0, release = 0; };

	struct PatchArea
	{
		std::vector<PatchModule> modules;
		std::vector<PatchCable> cables;

		PatchModule* find(int _index);
		const PatchModule* find(int _index) const;
	};

	struct Patch
	{
		std::string name = "Init Patch";
		PatchHeader header;
		PatchArea areas[2];			// [0] common, [1] poly
		std::array<int, 4> morphValues{0, 0, 0, 0};
		std::array<int, 4> morphKeyboard{0, 0, 0, 0};
		std::vector<MorphAssignment> morphAssignments;
		std::array<KnobAssignment, 23> knobs{};
		std::vector<CtrlAssignment> ctrls;
		std::vector<NoteSlot> notes;

		PatchArea& area(const int _section) { return areas[_section ? 1 : 0]; }
		const PatchArea& area(const int _section) const { return areas[_section ? 1 : 0]; }

		size_t moduleCount() const { return areas[0].modules.size() + areas[1].modules.size(); }
	};

	class PchFile
	{
	public:
		// Parses the Clavia editor's text formats: "Nord Modular patch 3.0" and the older 2.10.
		// _name is used when the file carries no patch name (3.0 files never do), typically the file stem.
		static bool parse(const std::string& _text, Patch& _patch, std::string& _error, const std::string& _name = {});
		static bool load(const std::string& _filename, Patch& _patch, std::string& _error);

		static std::string nameFromFilename(const std::string& _filename);

	private:
		static bool parse30(const std::vector<std::string>& _lines, Patch& _patch, std::string& _error);
		static bool parse210(const std::vector<std::string>& _lines, Patch& _patch, std::string& _error);
	};

	class PatchSysex
	{
	public:
		// the "I am" handshake the editor opens a session with; the synth answers with its own
		static std::vector<uint8_t> iAm();

		// the 16 upload sections as raw 8 bit bytes, in the order the synth expects
		static std::vector<std::vector<uint8_t>> serialize(const Patch& _patch);

		// complete patch upload as a list of sysex messages for the given slot (0 on a Micro Modular)
		static std::vector<std::vector<uint8_t>> upload(const Patch& _patch, int _slot = 0);

		// one continuous stream cut into packets of at most 166 raw bytes, each 7 bit packed and framed
		static std::vector<std::vector<uint8_t>> packetize(const std::vector<std::vector<uint8_t>>& _sections, int _slot);

		static std::vector<uint8_t> frame(int _cc, int _slot, const std::vector<uint8_t>& _payload, bool _checksum = true);
		static std::vector<uint8_t> pack7(const uint8_t* _raw, size_t _count);
	};
}
