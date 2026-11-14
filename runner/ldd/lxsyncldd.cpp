// lxsyncldd.cpp - LxSync1.ldd, the PsiLinux runner's cache driver for the
// netBook (SA-1100): makes code the runner has just written visible to the
// processor. See ../../common/lxsync.h for why.
//
// Kernel code: a bug here takes the whole machine down. It runs cache
// maintenance instructions only (CP15 c7): clean data cache lines by
// address, drain the write buffer, invalidate the instruction cache. Nothing
// is written to memory, and no register outside CP15 is touched.
//
// The SA-1100 (Intel SA-1100 Developer's Manual, 6.2.4): "clean D entry"
// (c7, c10, 1) takes a virtual address and writes that line back if it is
// dirty; "drain write buffer" (c7, c10, 4); "flush I" (c7, c5, 0)
// invalidates the whole instruction cache. Lines are 32 bytes. The range is
// the runner's own memory (the caller's address space), as it was given;
// cleaning a line that is not in the cache does nothing, and the operation
// does not fault on an unmapped address. A long range is capped: past
// 16 KB (twice the 8 KB data cache) the cache is cleaned whole by reading
// 16 KB of the ROM, which pushes out every dirty line.
//
// Built against the netBook kernel's export numbers (ekern_nb.def, from
// PsiBoot's driver): never load it on another machine.
#include "kern.h"
#include <e32svr.h>
#include "lxsync.h"

const TUint32 KSa1100Id = 0x4401a110;   // the CPU ID, revision masked off
const TUint32 KLine = 32;
const TUint32 KByLine = 16 * 1024;      // longer ranges: the whole cache
const TUint32 KRomBase = 0x50000000;    // cacheable, always mapped: read to evict

static TUint CpuMode()
	{
	TUint cpsr;
	asm volatile("mrs %0, cpsr" : "=r"(cpsr));
	return cpsr & 0x1f;
	}

static TUint32 CpuId()
	{
	TUint32 id;
	asm volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(id));
	return id;
	}

static TInt Check()
	{
	if (CpuMode() == 0x10)                  // (user mode: no CP15)
		return KErrNotSupported;
	if ((CpuId() & 0xfffffff0) != KSa1100Id)
		return KErrNotSupported;
	return KErrNone;
	}

static void Sync(TUint32 aStart, TUint32 aLength)
	{
	if (aLength > KByLine)
		{
		// the whole data cache: 16 KB of other cacheable memory read through
		// it replaces every line, writing back the dirty ones (the SA-1100's
		// replacement is round robin, 8 KB in all)
		volatile const TUint32* p = (volatile const TUint32*)KRomBase;
		TUint32 sum = 0;
		for (TUint32 i = 0; i < KByLine / 4; i += KLine / 4)
			sum += p[i];
		(void)sum;
		}
	else
		{
		TUint32 a = aStart & ~(KLine - 1);
		TUint32 end = aStart + aLength;
		for (; a < end; a += KLine)
			asm volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(a) : "memory");
		}
	TUint32 z = 0;
	asm volatile(
		"mcr p15, 0, %0, c7, c10, 4\n\t"    // drain the write buffer
		"mcr p15, 0, %0, c7, c5, 0\n\t"     // invalidate the instruction cache
		"nop\n\tnop\n\tnop\n\tnop\n\t"      // (the SA-1100 may have fetched up to 3 more)
		: : "r"(z) : "memory");
	}

class DLxSyncDevice : public DLogicalDevice
	{
public:
	DLxSyncDevice();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual DLogicalChannel* CreateL();
	};

class DLxSyncChannel : public DLogicalChannel
	{
public:
	DLxSyncChannel(DLogicalDevice* aDevice);
protected:
	virtual void DoCancel(TInt aReqNo);
	virtual void DoRequest(TInt aReqNo, TAny* a1, TAny* a2);
	virtual void DoCreateL(TInt aUnit, CBase* aPdd, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt DoControl(TInt aFunction, TAny* a1, TAny* a2);
	};

DLxSyncDevice::DLxSyncDevice()
	{
	iVersion = TVersion(KLxSyncMajor, KLxSyncMinor, KLxSyncBuild);
	iParseMask = 0;     // no unit, no physical device, no info
	iUnitsMask = 0;
	}

TInt DLxSyncDevice::Install()
	{
	return SetName(&KLxSyncName);
	}

void DLxSyncDevice::GetCaps(TDes8& aDes) const
	{
	TVersion v(KLxSyncMajor, KLxSyncMinor, KLxSyncBuild);
	aDes.FillZ(aDes.MaxLength());
	aDes.Copy((const TUint8*)&v, Min(aDes.MaxLength(), (TInt)sizeof(v)));
	}

DLogicalChannel* DLxSyncDevice::CreateL()
	{
	return new (ELeave) DLxSyncChannel(this);
	}

DLxSyncChannel::DLxSyncChannel(DLogicalDevice* aDevice)
	: DLogicalChannel(aDevice)
	{
	}

void DLxSyncChannel::DoCreateL(TInt /*aUnit*/, CBase* /*aPdd*/, const TDesC8* /*aInfo*/, const TVersion& aVer)
	{
	if (!User::QueryVersionSupported(TVersion(KLxSyncMajor, KLxSyncMinor, KLxSyncBuild), aVer))
		User::Leave(KErrNotSupported);
	if (Check() != KErrNone)
		User::Leave(KErrNotSupported);
	}

void DLxSyncChannel::DoCancel(TInt /*aReqNo*/)
	{
	}

void DLxSyncChannel::DoRequest(TInt /*aReqNo*/, TAny* /*a1*/, TAny* /*a2*/)
	{
	// no asynchronous requests (iBehaviour stays 0, so the kernel refuses them)
	}

TInt DLxSyncChannel::DoControl(TInt aFunction, TAny* a1, TAny* a2)
	{
	switch (aFunction)
		{
	case ELxSyncCheck:
		return Check();
	case ELxSyncRange:
		Sync((TUint32)a1, (TUint32)a2);
		return KErrNone;
	default:
		return KErrNotSupported;
		}
	}

// --- the DLL

EXPORT_C DLogicalDevice* CreateLogicalDevice()
	{
	return new DLxSyncDevice;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
