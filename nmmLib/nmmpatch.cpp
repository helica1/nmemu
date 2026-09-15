#include "nmmpatch.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "nmmlog.h"

namespace nmm
{
	namespace
	{
		std::string trim(const std::string& _s)
		{
			size_t a = 0, b = _s.size();
			while(a < b && std::isspace(static_cast<unsigned char>(_s[a]))) ++a;
			while(b > a && std::isspace(static_cast<unsigned char>(_s[b-1]))) --b;
			return _s.substr(a, b - a);
		}

		std::string lower(std::string _s)
		{
			for (auto& c : _s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return _s;
		}

		std::vector<std::string> tokenize(const std::string& _line)
		{
			std::vector<std::string> t;
			std::istringstream ss(_line);
			std::string tok;
			while(ss >> tok) t.push_back(tok);
			return t;
		}

		int toInt(const std::string& _s)
		{
			try { return std::stoi(_s); } catch(...) { return 0; }
		}

		std::vector<std::string> splitLines(const std::string& _text)
		{
			std::vector<std::string> lines;
			std::string cur;
			for (const char c : _text)
			{
				if(c == '\n') { lines.push_back(cur); cur.clear(); }
				else if(c != '\r') cur += c;
			}
			if(!cur.empty()) lines.push_back(cur);
			return lines;
		}

		bool isLegacy210(const std::vector<std::string>& _lines)
		{
			for(size_t i=0; i<_lines.size() && i<12; ++i)
			{
				const auto l = lower(trim(_lines[i]));
				if(l.rfind("version=", 0) == 0 && l.find("nord modular patch 2.10") != std::string::npos)
					return true;
			}
			return false;
		}

		void initModule(PatchModule& _m)
		{
			_m.desc = ModuleDb::find(_m.type);
			_m.params.clear();
			_m.customs.clear();
			if(!_m.desc)
				return;
			for(int i=0; i<_m.desc->paramCount; ++i) _m.params.push_back(_m.desc->params[i].defaultValue);
			for(int i=0; i<_m.desc->customCount; ++i) _m.customs.push_back(_m.desc->customs[i].defaultValue);
			if(_m.name.empty())
				_m.name = _m.desc->name;
		}

		class BitWriter
		{
		public:
			void write(const uint32_t _value, const int _bits)
			{
				for(int i=_bits-1; i>=0; --i)
					m_bits.push_back(static_cast<uint8_t>((_value >> i) & 1));
			}
			void string16(const std::string& _s)
			{
				const auto len = std::min<size_t>(_s.size(), 16);
				for(size_t i=0; i<len; ++i) write(static_cast<uint8_t>(_s[i]), 8);
				if(len < 16) write(0, 8);
			}
			std::vector<uint8_t> bytes()
			{
				while(m_bits.size() & 7) m_bits.push_back(0);
				std::vector<uint8_t> out;
				out.reserve(m_bits.size() / 8);
				for(size_t i=0; i<m_bits.size(); i += 8)
				{
					uint8_t b = 0;
					for(int j=0; j<8; ++j) b = static_cast<uint8_t>((b << 1) | m_bits[i + j]);
					out.push_back(b);
				}
				return out;
			}
		private:
			std::vector<uint8_t> m_bits;
		};
	}

	PatchModule* PatchArea::find(const int _index)
	{
		for (auto& m : modules) if(m.index == _index) return &m;
		return nullptr;
	}

	const PatchModule* PatchArea::find(const int _index) const
	{
		for (const auto& m : modules) if(m.index == _index) return &m;
		return nullptr;
	}

	// ---------------------------------------------------------------------------------------------
	// .pch text files
	// ---------------------------------------------------------------------------------------------

	std::string PchFile::nameFromFilename(const std::string& _filename)
	{
		auto s = _filename;
		const auto slash = s.find_last_of("/\\");
		if(slash != std::string::npos) s = s.substr(slash + 1);
		const auto dot = s.find_last_of('.');
		if(dot != std::string::npos && dot > 0) s = s.substr(0, dot);
		// bank backups are named "NN - Name"
		if(s.size() > 5 && std::isdigit(static_cast<unsigned char>(s[0])) && std::isdigit(static_cast<unsigned char>(s[1])) && s.substr(2, 3) == " - ")
			s = s.substr(5);
		return s.substr(0, 16);
	}

	bool PchFile::load(const std::string& _filename, Patch& _patch, std::string& _error)
	{
		std::ifstream f(_filename, std::ios::binary);
		if(!f)
		{
			_error = "cannot open " + _filename;
			return false;
		}
		std::stringstream ss;
		ss << f.rdbuf();
		return parse(ss.str(), _patch, _error, nameFromFilename(_filename));
	}

	bool PchFile::parse(const std::string& _text, Patch& _patch, std::string& _error, const std::string& _name)
	{
		_patch = Patch();
		const auto lines = splitLines(_text);
		if(lines.empty())
		{
			_error = "empty file";
			return false;
		}

		const bool ok = isLegacy210(lines) ? parse210(lines, _patch, _error) : parse30(lines, _patch, _error);
		if(!ok)
			return false;

		if(_patch.name == "Init Patch" && !_name.empty())
			_patch.name = _name;
		return true;
	}

	bool PchFile::parse30(const std::vector<std::string>& _lines, Patch& _patch, std::string& _error)
	{
		bool sawHeader = false;
		int unknownModules = 0;

		// custom dumps are applied after all module dumps are in
		std::vector<std::pair<int, std::vector<int>>> customDumps[2];

		size_t i = 0;
		while(i < _lines.size())
		{
			const auto line = trim(_lines[i]);
			if(line.empty() || line[0] != '[' || line.rfind("[/", 0) == 0)
			{
				++i;
				continue;
			}
			const auto close = line.find(']');
			if(close == std::string::npos) { ++i; continue; }
			const auto section = lower(line.substr(1, close - 1));
			const auto closeTag = lower("[/" + line.substr(1, close));

			std::vector<std::string> body;
			++i;
			while(i < _lines.size())
			{
				const auto l = trim(_lines[i]);
				++i;
				if(lower(l) == closeTag)
					break;
				body.push_back(l);
			}

			auto areaOf = [&](const std::vector<std::string>& _b) -> PatchArea*
			{
				if(_b.empty()) return nullptr;
				const auto t = tokenize(_b[0]);
				if(t.empty()) return nullptr;
				return &_patch.area(toInt(t[0]) == 1 ? 1 : 0);
			};

			if(section == "header")
			{
				std::vector<std::string> tok;
				for (const auto& l : body)
				{
					if(lower(l).rfind("version", 0) == 0) continue;
					const auto t = tokenize(l);
					tok.insert(tok.end(), t.begin(), t.end());
				}
				if(tok.size() >= 23)
				{
					auto& h = _patch.header;
					h.keyRangeMin = toInt(tok[0]); h.keyRangeMax = toInt(tok[1]);
					h.velRangeMin = toInt(tok[2]); h.velRangeMax = toInt(tok[3]);
					h.bendRange = toInt(tok[4]);
					h.portamentoTime = toInt(tok[5]);
					h.portamento = toInt(tok[6]) != 0;
					h.voices = toInt(tok[7]);
					h.separatorPosition = toInt(tok[8]);
					h.octaveShift = toInt(tok[9]);
					h.voiceRetriggerPoly = toInt(tok[10]);
					h.voiceRetriggerCommon = toInt(tok[11]);
					h.unknown1 = toInt(tok[12]); h.unknown2 = toInt(tok[13]); h.unknown3 = toInt(tok[14]); h.unknown4 = toInt(tok[15]);
					for(int c=0; c<7; ++c) h.cableVis[c] = toInt(tok[16 + c]) != 0;
					sawHeader = true;
				}
			}
			else if(section == "moduledump")
			{
				auto* area = areaOf(body);
				if(!area) continue;
				for(size_t k=1; k<body.size(); ++k)
				{
					const auto t = tokenize(body[k]);
					if(t.size() < 4) continue;
					PatchModule m;
					m.index = toInt(t[0]);
					m.type = toInt(t[1]);
					m.x = toInt(t[2]);
					m.y = toInt(t[3]);
					initModule(m);
					if(!m.desc)
					{
						++unknownModules;
						NMMLOG("pch: unknown module type %d (index %d), skipped", m.type, m.index);
						continue;
					}
					area->modules.push_back(std::move(m));
				}
			}
			else if(section == "currentnotedump")
			{
				std::vector<std::string> tok;
				for (const auto& l : body) { const auto t = tokenize(l); tok.insert(tok.end(), t.begin(), t.end()); }
				for(size_t k=0; k+2<tok.size(); k+=3)
					_patch.notes.push_back({toInt(tok[k]), toInt(tok[k+1]), toInt(tok[k+2])});
			}
			else if(section == "cabledump")
			{
				auto* area = areaOf(body);
				if(!area) continue;
				for(size_t k=1; k<body.size(); ++k)
				{
					const auto t = tokenize(body[k]);
					if(t.size() < 7) continue;
					// color, first module/conn/type, second module/conn/type; type 1 = output. The
					// editor writes the destination first. For input-input chains both are inputs and
					// the second one is the "source".
					const int color = toInt(t[0]);
					const int m1 = toInt(t[1]), c1 = toInt(t[2]), t1 = toInt(t[3]);
					const int m2 = toInt(t[4]), c2 = toInt(t[5]), t2 = toInt(t[6]);
					PatchCable cable;
					cable.color = color;
					if(t1 != 0)
					{
						cable.srcModule = m1; cable.srcConn = c1; cable.srcIsOutput = true;
						cable.dstModule = m2; cable.dstConn = c2;
					}
					else
					{
						cable.srcModule = m2; cable.srcConn = c2; cable.srcIsOutput = t2 != 0;
						cable.dstModule = m1; cable.dstConn = c1;
					}
					const auto* src = area->find(cable.srcModule);
					const auto* dst = area->find(cable.dstModule);
					if(!src || !dst)
					{
						NMMLOG("pch: cable refers to missing module %d or %d, skipped", cable.srcModule, cable.dstModule);
						continue;
					}
					// the wire colour is the source connector's signal type
					if(const auto* cd = src->desc->conn(cable.srcConn, cable.srcIsOutput))
						cable.color = cd->signal;
					area->cables.push_back(cable);
				}
			}
			else if(section == "parameterdump")
			{
				auto* area = areaOf(body);
				if(!area) continue;
				for(size_t k=1; k<body.size(); ++k)
				{
					const auto t = tokenize(body[k]);
					if(t.size() < 3) continue;
					auto* m = area->find(toInt(t[0]));
					if(!m) continue;
					// t[1] = type, t[2] = count, then the values in dump order
					for(size_t p=0; p<m->params.size() && 3+p<t.size(); ++p)
					{
						const auto& pd = m->desc->params[p];
						m->params[p] = std::max(pd.minValue, std::min(pd.maxValue, toInt(t[3+p])));
					}
				}
			}
			else if(section == "morphmapdump")
			{
				std::vector<std::string> tok;
				for (const auto& l : body) { const auto t = tokenize(l); tok.insert(tok.end(), t.begin(), t.end()); }
				if(tok.size() >= 4)
				{
					for(int k=0; k<4; ++k) _patch.morphValues[k] = toInt(tok[k]);
					for(size_t k=4; k+4<tok.size(); k+=5)
						_patch.morphAssignments.push_back({toInt(tok[k]), toInt(tok[k+1]), toInt(tok[k+2]), toInt(tok[k+3]), toInt(tok[k+4])});
				}
			}
			else if(section == "keyboardassignment")
			{
				std::vector<std::string> tok;
				for (const auto& l : body) { const auto t = tokenize(l); tok.insert(tok.end(), t.begin(), t.end()); }
				for(size_t k=0; k<4 && k<tok.size(); ++k) _patch.morphKeyboard[k] = toInt(tok[k]);
			}
			else if(section == "knobmapdump")
			{
				for (const auto& l : body)
				{
					const auto t = tokenize(l);
					if(t.size() < 4) continue;
					const int knob = toInt(t[3]);
					if(knob < 0 || knob >= 23) continue;
					_patch.knobs[knob] = {true, toInt(t[0]), toInt(t[1]), toInt(t[2])};
				}
			}
			else if(section == "ctrlmapdump")
			{
				for (const auto& l : body)
				{
					const auto t = tokenize(l);
					if(t.size() < 4) continue;
					_patch.ctrls.push_back({toInt(t[3]), toInt(t[0]), toInt(t[1]), toInt(t[2])});
				}
			}
			else if(section == "customdump")
			{
				if(body.empty()) continue;
				const auto first = tokenize(body[0]);
				const int sec = first.empty() ? 1 : (toInt(first[0]) == 1 ? 1 : 0);
				for(size_t k=1; k<body.size(); ++k)
				{
					const auto t = tokenize(body[k]);
					if(t.size() < 2) continue;
					std::vector<int> values;
					const int n = toInt(t[1]);
					for(int v=0; v<n && 2+v<static_cast<int>(t.size()); ++v) values.push_back(toInt(t[2+v]));
					customDumps[sec].emplace_back(toInt(t[0]), std::move(values));
				}
			}
			else if(section == "namedump")
			{
				auto* area = areaOf(body);
				if(!area) continue;
				for(size_t k=1; k<body.size(); ++k)
				{
					const auto& l = body[k];
					const auto sp = l.find(' ');
					if(sp == std::string::npos) continue;
					auto* m = area->find(toInt(l.substr(0, sp)));
					const auto name = trim(l.substr(sp + 1));
					if(m && !name.empty()) m->name = name.substr(0, 16);
				}
			}
			// [Notes], [Info], [Comments], [NME], ... are editor-only
		}

		for(int sec=0; sec<2; ++sec)
		{
			for (const auto& [index, values] : customDumps[sec])
			{
				auto* m = _patch.area(sec).find(index);
				if(!m) continue;
				for(size_t v=0; v<values.size() && v<m->customs.size(); ++v)
					m->customs[v] = values[v];
			}
		}

		if(!sawHeader)
		{
			_error = "no [Header] section, not a Nord Modular 3.0 patch";
			return false;
		}

		// assignments that name modules the patch does not have are leftovers of deleted modules
		auto moduleExists = [&](const int _section, const int _module)
		{
			return _section == 2 || _patch.area(_section).find(_module) != nullptr;
		};
		_patch.morphAssignments.erase(std::remove_if(_patch.morphAssignments.begin(), _patch.morphAssignments.end(),
			[&](const MorphAssignment& a) { return !moduleExists(a.section, a.module); }), _patch.morphAssignments.end());
		_patch.ctrls.erase(std::remove_if(_patch.ctrls.begin(), _patch.ctrls.end(),
			[&](const CtrlAssignment& a) { return !moduleExists(a.section, a.module); }), _patch.ctrls.end());
		for (auto& k : _patch.knobs)
			if(k.assigned && !moduleExists(k.section, k.module)) k.assigned = false;

		if(unknownModules)
			NMMLOG("pch: %d module(s) of unknown type skipped", unknownModules);
		return true;
	}

	bool PchFile::parse210(const std::vector<std::string>& _lines, Patch& _patch, std::string& _error)
	{
		struct LegacyCable { int srcModule, srcConn; bool srcIsOutput; int dstModule, dstInput; };
		std::vector<LegacyCable> pending;
		auto& area = _patch.area(1);

		auto value = [](const std::vector<std::string>& _body, const std::string& _key) -> std::string
		{
			const auto prefix = lower(_key) + "=";
			for (const auto& l : _body)
			{
				const auto t = trim(l);
				if(lower(t).rfind(prefix, 0) == 0)
					return trim(t.substr(prefix.size()));
			}
			return {};
		};

		size_t i = 0;
		while(i < _lines.size())
		{
			const auto line = trim(_lines[i]);
			if(line.empty() || line.front() != '[' || line.back() != ']') { ++i; continue; }
			const auto section = line.substr(1, line.size() - 2);
			std::vector<std::string> body;
			++i;
			while(i < _lines.size())
			{
				const auto l = trim(_lines[i]);
				if(!l.empty() && l.front() == '[' && l.back() == ']') break;
				body.push_back(l);
				++i;
			}

			if(lower(section) == "header")
			{
				auto& h = _patch.header;
				const auto name = value(body, "Name");
				if(!name.empty()) _patch.name = name.substr(0, 16);
				auto assign = [&](const char* _k, int& _target) { const auto v = value(body, _k); if(!v.empty()) _target = toInt(v); };
				assign("KbRangeMin", h.keyRangeMin); assign("KbRangeMax", h.keyRangeMax);
				assign("VelRangeMin", h.velRangeMin); assign("VelRangeMax", h.velRangeMax);
				assign("BendRange", h.bendRange); assign("PMTime", h.portamentoTime);
				assign("Voices", h.voices); assign("OctShift", h.octaveShift);
				assign("Retrig", h.voiceRetriggerPoly);
				h.voiceRetriggerCommon = h.voiceRetriggerPoly;
				const auto pm = value(body, "PMMode");
				if(!pm.empty()) h.portamento = toInt(pm) != 0;
			}
			else if(lower(section).rfind("module ", 0) == 0)
			{
				PatchModule m;
				m.index = toInt(section.substr(7));
				m.type = toInt(value(body, "Type"));
				m.x = toInt(value(body, "Col"));
				m.y = toInt(value(body, "Row"));
				m.name = value(body, "Name").substr(0, 16);
				initModule(m);
				if(!m.desc)
				{
					NMMLOG("pch: unknown module type %d (index %d), skipped", m.type, m.index);
					continue;
				}
				for(size_t p=0; p<m.params.size(); ++p)
				{
					const auto& pd = m.desc->params[p];
					const auto v = value(body, "P" + std::to_string(pd.index));
					if(v.empty()) continue;
					int pv = toInt(v);
					if(pd.flags & 1) pv = std::max(0, pv - 1);	// output destinations are 1-based in 2.10
					m.params[p] = std::max(pd.minValue, std::min(pd.maxValue, pv));
				}
				for (const auto& l : body)
				{
					// ImN=<source module>, IhN=[output flag:bit6][connector:bits0-5] for target input N
					if(lower(l).rfind("im", 0) != 0) continue;
					const auto eq = l.find('=');
					if(eq == std::string::npos) continue;
					const int input = toInt(l.substr(2, eq - 2));
					const int srcModule = toInt(l.substr(eq + 1));
					const auto ih = value(body, "Ih" + std::to_string(input));
					if(srcModule <= 0 || ih.empty()) continue;
					const int ihv = toInt(ih);
					pending.push_back({srcModule, ihv & 0x3f, (ihv & 0x40) != 0, m.index, input});
				}
				area.modules.push_back(std::move(m));
			}
		}

		if(area.modules.empty())
		{
			_error = "no modules found in 2.10 patch";
			return false;
		}

		for (const auto& c : pending)
		{
			const auto* src = area.find(c.srcModule);
			const auto* dst = area.find(c.dstModule);
			if(!src || !dst) continue;
			const auto* sc = src->desc->conn(c.srcConn, c.srcIsOutput);
			const auto* dc = dst->desc->conn(c.dstInput, false);
			if(!sc || !dc) continue;
			PatchCable cable;
			cable.color = sc->signal;
			cable.srcModule = c.srcModule; cable.srcConn = c.srcConn; cable.srcIsOutput = c.srcIsOutput;
			cable.dstModule = c.dstModule; cable.dstConn = c.dstInput;
			area.cables.push_back(cable);
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// sysex
	// ---------------------------------------------------------------------------------------------

	std::vector<uint8_t> PatchSysex::frame(const int _cc, const int _slot, const std::vector<uint8_t>& _payload, const bool _checksum)
	{
		std::vector<uint8_t> msg{0xf0, 0x33, static_cast<uint8_t>(((_cc & 0x1f) << 2) | (_slot & 3)), 0x06};
		msg.insert(msg.end(), _payload.begin(), _payload.end());
		if(_checksum)
		{
			uint32_t sum = 0;
			for (const auto b : msg) sum += b;
			msg.push_back(static_cast<uint8_t>(sum & 0x7f));
		}
		msg.push_back(0xf7);
		return msg;
	}

	std::vector<uint8_t> PatchSysex::pack7(const uint8_t* _raw, const size_t _count)
	{
		std::vector<uint8_t> out;
		out.reserve((_count * 8 + 6) / 7);
		uint32_t buffer = 0;
		int held = 0;
		for(size_t i=0; i<_count; ++i)
		{
			buffer = (buffer << 8) | _raw[i];
			held += 8;
			while(held >= 7)
			{
				held -= 7;
				out.push_back(static_cast<uint8_t>((buffer >> held) & 0x7f));
			}
		}
		if(held > 0)
			out.push_back(static_cast<uint8_t>((buffer << (7 - held)) & 0x7f));
		return out;
	}

	std::vector<uint8_t> PatchSysex::iAm()
	{
		return frame(0x00, 0, {0x00, 0x03, 0x03}, false);
	}

	std::vector<std::vector<uint8_t>> PatchSysex::serialize(const Patch& _patch)
	{
		std::vector<std::vector<uint8_t>> sections;

		// PatchName (55): three padding bytes, then the name
		{
			BitWriter w;
			w.write(55, 8); w.write(0, 8); w.write(0, 8); w.write(0, 8);
			w.string16(_patch.name);
			sections.push_back(w.bytes());
		}
		// Header (33)
		{
			const auto& h = _patch.header;
			BitWriter w;
			w.write(33, 8);
			w.write(h.keyRangeMin, 7); w.write(h.keyRangeMax, 7);
			w.write(h.velRangeMin, 7); w.write(h.velRangeMax, 7);
			w.write(h.bendRange, 5);
			w.write(h.portamentoTime, 7);
			w.write(h.portamento ? 1 : 0, 1);
			w.write(1, 1);								// pedal mode, the editor always sends 1
			w.write(std::max(1, std::min(32, h.voices)) - 1, 5);
			w.write(0, 2);
			w.write(h.separatorPosition & 0xfff, 12);
			w.write(h.octaveShift & 7, 3);
			for(int c=0; c<7; ++c) w.write(h.cableVis[c] ? 1 : 0, 1);
			w.write(h.voiceRetriggerCommon & 1, 1);
			w.write(h.voiceRetriggerPoly & 1, 1);
			w.write(0xf, 4);
			w.write(0, 3);
			sections.push_back(w.bytes());
		}

		auto moduleDump = [&](const int _section)
		{
			const auto& a = _patch.area(_section);
			BitWriter w;
			w.write(74, 8); w.write(_section, 1);
			w.write(static_cast<uint32_t>(a.modules.size()), 7);
			for (const auto& m : a.modules)
			{
				w.write(m.type, 7); w.write(m.index, 7); w.write(m.x, 7); w.write(m.y, 7);
			}
			return w.bytes();
		};
		auto noteDump = [&]()
		{
			BitWriter w;
			w.write(105, 8);
			auto note = [&](const NoteSlot& n) { w.write(n.note & 0x7f, 7); w.write(n.attack & 0x7f, 7); w.write(n.release & 0x7f, 7); };
			if(_patch.notes.size() >= 2)
			{
				note(_patch.notes[0]);
				w.write(static_cast<uint32_t>(_patch.notes.size() - 2), 5);
				for(size_t i=1; i<_patch.notes.size(); ++i) note(_patch.notes[i]);
			}
			else
			{
				note({64, 0, 0}); w.write(0, 5); note({64, 0, 0});
			}
			return w.bytes();
		};
		auto cableDump = [&](const int _section)
		{
			const auto& a = _patch.area(_section);
			BitWriter w;
			w.write(82, 8); w.write(_section, 1);
			w.write(static_cast<uint32_t>(a.cables.size()), 15);
			for (const auto& c : a.cables)
			{
				w.write(c.color & 7, 3);
				w.write(c.srcModule, 7); w.write(c.srcConn, 6); w.write(c.srcIsOutput ? 1 : 0, 1);
				w.write(c.dstModule, 7); w.write(c.dstConn, 6);
			}
			return w.bytes();
		};
		auto paramDump = [&](const int _section)
		{
			const auto& a = _patch.area(_section);
			BitWriter w;
			w.write(77, 8); w.write(_section, 1);
			uint32_t n = 0;
			for (const auto& m : a.modules) if(!m.params.empty()) ++n;
			w.write(n, 7);
			for (const auto& m : a.modules)
			{
				if(m.params.empty()) continue;
				w.write(m.index, 7); w.write(m.type, 7);
				for(size_t p=0; p<m.params.size(); ++p)
					w.write(static_cast<uint32_t>(m.params[p]), m.desc->params[p].bits);
			}
			return w.bytes();
		};
		auto morphMap = [&]()
		{
			BitWriter w;
			w.write(101, 8);
			for(int i=0; i<4; ++i) w.write(_patch.morphValues[i] & 0x7f, 7);
			for(int i=0; i<4; ++i) w.write(_patch.morphKeyboard[i] & 3, 2);
			const auto n = std::min<size_t>(_patch.morphAssignments.size(), 31);
			w.write(static_cast<uint32_t>(n), 5);
			for(size_t i=0; i<n; ++i)
			{
				const auto& ma = _patch.morphAssignments[i];
				w.write(ma.section & 1, 1); w.write(ma.module, 7); w.write(ma.param, 7); w.write(ma.morph & 3, 2); w.write(ma.range & 0xff, 8);
			}
			return w.bytes();
		};
		auto knobMap = [&]()
		{
			BitWriter w;
			w.write(98, 8);
			for (const auto& k : _patch.knobs)
			{
				if(!k.assigned) { w.write(0, 1); continue; }
				w.write(1, 1); w.write(k.section & 3, 2); w.write(k.module, 7); w.write(k.param, 7);
			}
			return w.bytes();
		};
		auto controlMap = [&]()
		{
			BitWriter w;
			w.write(96, 8);
			w.write(static_cast<uint32_t>(_patch.ctrls.size()), 7);
			for (const auto& c : _patch.ctrls)
			{
				w.write(c.control, 7); w.write(c.section & 3, 2); w.write(c.module, 7); w.write(c.param, 7);
			}
			return w.bytes();
		};
		auto customDump = [&](const int _section)
		{
			const auto& a = _patch.area(_section);
			BitWriter w;
			w.write(91, 8); w.write(_section, 1);
			uint32_t n = 0;
			for (const auto& m : a.modules) if(!m.customs.empty()) ++n;
			w.write(n, 7);
			for (const auto& m : a.modules)
			{
				if(m.customs.empty()) continue;
				w.write(m.index, 7);
				w.write(static_cast<uint32_t>(m.customs.size()), 8);
				for (const auto v : m.customs) w.write(v & 0xff, 8);
			}
			return w.bytes();
		};
		auto nameDump = [&](const int _section)
		{
			const auto& a = _patch.area(_section);
			BitWriter w;
			w.write(90, 8); w.write(_section, 1);
			w.write(static_cast<uint32_t>(a.modules.size()), 7);
			for (const auto& m : a.modules)
			{
				w.write(m.index, 8);
				w.string16(m.name);
			}
			return w.bytes();
		};

		sections.push_back(moduleDump(1));
		sections.push_back(moduleDump(0));
		sections.push_back(noteDump());
		sections.push_back(cableDump(1));
		sections.push_back(cableDump(0));
		sections.push_back(paramDump(1));
		sections.push_back(paramDump(0));
		sections.push_back(morphMap());
		sections.push_back(knobMap());
		sections.push_back(controlMap());
		sections.push_back(customDump(1));
		sections.push_back(customDump(0));
		sections.push_back(nameDump(1));
		sections.push_back(nameDump(0));
		return sections;
	}

	std::vector<std::vector<uint8_t>> PatchSysex::packetize(const std::vector<std::vector<uint8_t>>& _sections, const int _slot)
	{
		constexpr size_t packetBytes = 166;

		struct Packet { std::vector<uint8_t> data; int sectionsEnded = 0; };
		std::vector<Packet> packets;
		Packet cur;

		for (const auto& s : _sections)
		{
			for (const auto b : s)
			{
				if(cur.data.size() == packetBytes)
				{
					packets.push_back(std::move(cur));
					cur = Packet();
				}
				cur.data.push_back(b);
			}
			++cur.sectionsEnded;
		}
		if(!cur.data.empty())
			packets.push_back(std::move(cur));

		std::vector<std::vector<uint8_t>> frames;
		for(size_t i=0; i<packets.size(); ++i)
		{
			const bool first = i == 0;
			const bool last = i + 1 == packets.size();
			const int cc = 0x1c | (first ? 1 : 0) | (last ? 2 : 0);
			std::vector<uint8_t> payload{static_cast<uint8_t>(0x40 | (packets[i].sectionsEnded & 0x3f))};
			const auto packed = pack7(packets[i].data.data(), packets[i].data.size());
			payload.insert(payload.end(), packed.begin(), packed.end());
			frames.push_back(frame(cc, _slot, payload));
		}
		return frames;
	}

	std::vector<std::vector<uint8_t>> PatchSysex::upload(const Patch& _patch, const int _slot)
	{
		return packetize(serialize(_patch), _slot);
	}
}
