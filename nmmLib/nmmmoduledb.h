#pragma once

#include <cstddef>
#include <cstdint>

namespace nmm
{
	// Static description of the Nord Modular G1 module set, generated from nmedit's modules.xml
	struct ParamDesc
	{
		int index;			// parameter index within the module
		int minValue;
		int maxValue;
		int defaultValue;
		int bits;			// width in the ParameterDump bit stream
		int flags;			// 1 = output destination selector (stored 1-based in 2.10 files)
	};

	struct ConnDesc
	{
		int index;			// connector index, inputs and outputs are numbered separately
		int isOutput;
		int signal;			// cable colour: 0 audio, 1 control, 2 logic, 3 slave, 4/5 user, 6 none
	};

	struct ModuleDesc
	{
		int index;					// module type id
		const char* name;
		const ParamDesc* params;	// "parameter" class, in dump order
		int paramCount;
		const ParamDesc* customs;	// "custom" class (sequencer events, display units, ...)
		int customCount;
		const ConnDesc* conns;
		int connCount;

		const ParamDesc* param(const int _index) const
		{
			for(int i=0; i<paramCount; ++i) if(params[i].index == _index) return &params[i];
			return nullptr;
		}
		const ConnDesc* conn(const int _index, const bool _isOutput) const
		{
			for(int i=0; i<connCount; ++i) if(conns[i].index == _index && (conns[i].isOutput != 0) == _isOutput) return &conns[i];
			return nullptr;
		}
	};

	class ModuleDb
	{
	public:
		static const ModuleDesc* find(int _index);
		static size_t count();
		static const ModuleDesc* at(size_t _i);
	};
}
