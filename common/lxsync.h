// lxsync.h - the user side of LxSync1.ldd, the runner's cache driver for the
// netBook. Shared by the driver (runner/ldd/lxsyncldd.cpp) and the runner.
//
// Why: the netBook's SA-1100 has separate instruction and data caches, and
// its data cache is write-back. Code the runner writes (a program it loads,
// the translator's blocks and patches) can still be in the data cache when
// the processor fetches it, or the instruction cache can hold what was at
// that address before: the program then runs whatever was in RAM (Linux
// died at its first instruction, 14 November 2026). The 5mx's ARM710T has
// one cache for both, so it never needed this. A program cannot clean or
// invalidate the caches in user mode, and EPOC R5 has no call for it.
//
// Only for the netBook: the driver links against the netBook kernel's own
// export numbers, which are not the 5mx's. The runner loads it only when
// UserHal::MachineInfo names a netBook.
#ifndef LXSYNC_H
#define LXSYNC_H
#include <e32std.h>

_LIT(KLxSyncName, "LxSync1");
_LIT(KLxSyncFile, "LXSYNC1.LDD");       // \System\Libs\ on any disk
const TInt KLxSyncMajor = 0;
const TInt KLxSyncMinor = 1;
const TInt KLxSyncBuild = 1;

enum TLxSyncControl
	{
	ELxSyncCheck = 0,   // -> 0 on an SA-1100 in supervisor mode, else KErrNotSupported
	ELxSyncRange = 1    // a1 = start, a2 = length: the data cache's lines for
	                    // that range cleaned to RAM, the write buffer drained,
	                    // the whole instruction cache invalidated -> 0
	};

class RLxSync : public RBusLogicalChannel
	{
public:
	inline TInt Open()
		{ return DoCreate(KLxSyncName, TVersion(KLxSyncMajor, KLxSyncMinor, KLxSyncBuild), NULL, KNullUnit, NULL, NULL); }
	inline TInt Check() { return DoControl(ELxSyncCheck); }
	inline TInt Sync(TUint32 aStart, TUint32 aLength) { return DoControl(ELxSyncRange, (TAny*)aStart, (TAny*)aLength); }
	};

#endif
