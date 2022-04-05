#pragma once
#include "stdafx.h"
#include "DebuggerTypes.h"
#include "IMemoryHandler.h"
#include <unordered_set>
class Debugger;

struct AddressCounters
{
	uint32_t Address;
	uint32_t ReadCount;
	uint64_t ReadStamp;
	bool UninitRead;
	uint32_t WriteCount;
	uint64_t WriteStamp;
	uint32_t ExecCount;
	uint64_t ExecStamp;
};

class MemoryAccessCounter
{
private:
	Debugger* _debugger;
	
	vector<AddressCounters> _counters[4];
	vector<AddressCounters> _ppuCounters[4];

public:
	MemoryAccessCounter(Debugger* debugger);

	void ResetCounts();

	void GetAccessCounts(uint32_t offset, uint32_t length, DebugMemoryType memoryType, AddressCounters counts[]);
};
