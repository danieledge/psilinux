// bk_epoc.cpp - psilxrun.exe, PsiLinux's runner on EPOC R5: the kernel
// (linux/runtime) on its own thread, with EPOC under it.
//
//  - The main thread is the kernel thread. It runs an active scheduler:
//    one active object per Linux thread takes that thread's calls (the
//    thread completes it, RThread::RequestComplete, and waits on a request
//    of its own for the answer), another learns of its death (Logon); a
//    timer, the terminal's doorbell and a housekeeping tick do the rest.
//  - Every Linux thread is an RThread of this process. Its program runs on a
//    stack in a chunk of its own; faults reach an exception handler, which
//    reports them and waits to be killed (linux/docs/DESIGN.md, M0).
//  - Files are F32, through one RFs session; memory is RChunks; the terminal
//    is PsiLinux.app's window, through the global chunk of lxshared.h.
//  - Sockets are ESOCK (bk_esock.cpp).
#include <stdarg.h>               // (gcc's: EPOC's VA_LIST is another thing)
#include <e32base.h>
#include <e32hal.h>
#include <e32math.h>
#include <f32file.h>
#include "psibell.h"
extern "C" {
#include "lx_core.h"
#include "lx_dbt.h"
#include "lxshared.h"
}

extern "C" int lx_vsnprintf_pub(char* buf, int size, const char* fmt, va_list ap);
extern "C" int lx_root_info(lx_u32* e, lx_u32* b);
extern "C" int lx_read_file(const char* aPath, char* aBuf, int aSize);

static RFs gFs;
static RThread gKernel;          // the kernel thread, a handle every thread of the process may use
static RChunk gShChunk;
static LxShared* gSh;
static RChunk gArena;
static TBool gArenaOk;
static TInt gThreadNo;
static TPsiBellRinger gAppRinger;
static TInt64 gEpoch;            // 1970, as a TTime
static RFile gLog;
static TBool gLogOpen;
static TUint gLastBeat, gBeatTicks;
static TUint32 gFaultPc, gFaultInsn, gFaultWhy;   // an unfixed alignment fault (the kernel thread logs it)
static TUint32 gFaultIdx, gFaultSp, gFaultW[20];

// (ER5 has no User::AllocZ)
static TAny* AllocZ(TInt aSize)
	{
	TAny* p = User::Alloc(aSize);
	if (p)
		Mem::FillZ(p, aSize);
	return p;
	}

// ---- errors ----------------------------------------------------------------

static int Err(TInt aErr)
	{
	switch (aErr)
		{
	case KErrNone: return 0;
	case KErrNotFound: case KErrPathNotFound: return -LX_ENOENT;
	case KErrAlreadyExists: return -LX_EEXIST;
	case KErrAccessDenied: return -LX_EACCES;
	case KErrInUse: case KErrLocked: return -LX_EBUSY;
	case KErrDiskFull: return -LX_ENOSPC;
	case KErrNoMemory: return -LX_ENOMEM;
	case KErrBadName: case KErrArgument: return -LX_EINVAL;
	case KErrNotSupported: return -LX_EOPNOTSUPP;
	case KErrNotReady: case KErrDisMounted: return -LX_ENODEV;
	case KErrDirFull: return -LX_ENOSPC;
	case KErrEof: return 0;
	case KErrTooBig: return -LX_EFBIG;
	case KErrTimedOut: return -LX_ETIMEDOUT;
		}
	return -LX_EIO;
	}

static TPtrC8 Narrow(const char* aS)
	{
	return TPtrC8((const TUint8*)aS);
	}

// a native path as a TFileName (ER5 is a narrow build: TText is 8-bit)
static void Name(TDes& aOut, const char* aPath)
	{
	TPtrC8 p = Narrow(aPath);
	aOut.Copy(p.Left(Min(p.Length(), aOut.MaxLength() - 2)));
	}

// ---- threads ------------------------------------------------------------------

class CCallAO;
class CDeathAO;
struct THThread
	{
	RThread iThread;
	CCallAO* iCall;
	CDeathAO* iDeath;
	TRequestStatus iReply;          // the Linux thread waits on this for its answer
	TRequestStatus* iCallStatus;    // the call AO's request, which the Linux thread completes
	volatile TInt iFault;           // the signal its exception handler reported
	lx_thread* iT;
	};

class CCallAO : public CActive
	{
public:
	CCallAO(lx_thread* aT) : CActive(EPriorityStandard), iT(aT) { CActiveScheduler::Add(this); }
	~CCallAO() { Cancel(); }
	void Arm()
		{
		if (IsActive())
			return;
		iStatus = KRequestPending;
		SetActive();
		}
private:
	void RunL()
		{
		THThread* h = (THThread*)iT->bk;
		if (h && h->iFault)
			{
			h->iThread.Kill(0);              // (the death AO reports the signal)
			return;
			}
		lx_k_syscall(iT);
		}
	void DoCancel()
		{
		TRequestStatus* s = &iStatus;        // (nobody else will complete it now)
		User::RequestComplete(s, KErrCancel);
		}
	lx_thread* iT;
	};

class CDeathAO : public CActive
	{
public:
	CDeathAO(lx_thread* aT) : CActive(EPriorityStandard + 1), iT(aT) { CActiveScheduler::Add(this); }
	void Start(RThread& aThread)
		{
		aThread.Logon(iStatus);
		SetActive();
		}
private:
	void RunL()
		{
		THThread* h = (THThread*)iT->bk;
		TInt sig = h ? h->iFault : 0;
		if (h && !sig && h->iThread.ExitType() == EExitPanic)
			sig = LX_SIGSEGV;                // (a panic the handler could not report)
		if (sig == LX_SIGBUS)
			{
			bk_log("an alignment fault not fixed: pc %x, instruction %08x (%d, critical %d, %u fixed); sp %x, frame at +%d:", gFaultPc, gFaultInsn, gFaultWhy, lx_cpu.critical, lx_cpu.fixups, gFaultSp, gFaultIdx);
			for (TInt q = 0; q < 20; q += 5)
				bk_log("  %08x %08x %08x %08x %08x", gFaultW[q], gFaultW[q + 1], gFaultW[q + 2], gFaultW[q + 3], gFaultW[q + 4]);
			}
		lx_k_thread_died(iT, sig);
		delete this;
		}
	void DoCancel() {}
	lx_thread* iT;
	};

// faults: the exception handler of every Linux thread
static TUint32 CurrentSp()
	{
	TUint32 sp;
	__asm__ __volatile__("mov %0, sp" : "=r"(sp));
	return sp;
	}

// Alpine mode: an unaligned load or store in translated code (ARMv6 allows
// them, the ARM710T faults: M0, DESIGN.md). EUSER's exception trampoline
// left the faulting registers on the stack, above User::HandleException's
// frame: [r3..r12, lr, r0, r1, r2, pc]; HandleException's saved lr points
// at the trampoline's "pop {r3-r12,lr}; pop {r0,r1,r2,pc}". The access is
// done a byte at a time, pc moves on, and the handler returns: the thread
// carries on (lx_exc_entry keeps its flags). 1 if it was fixed.
static TInt FixAlignment(TUint32* aAbove)
	{
	// (from just above lx_exc_entry's own two words: below them is this
	// handler's stack, which holds stale words of earlier faults)
	TUint32* s = aAbove + 2;
	gFaultWhy = 1;
	for (TInt i = 0; i < 24; i++)
		{
		TUint32 w = s[i];
		if (w < 0x50000000 || w >= 0x51000000 || (w & 3))
			continue;
		if (((TUint32*)w)[0] != 0xE8BD5FF8 || ((TUint32*)w)[1] != 0xE8BD8007)
			continue;
		TUint32* f = s + i + 1;              // r3
		TUint32 pc = f[14];
		gFaultPc = pc;
		gFaultIdx = i;
		gFaultSp = (TUint32)s;
		for (TInt q = 0; q < 20; q++) gFaultW[q] = s[i + q];
		gFaultWhy = 2;
		if (pc < lx_cpu.cache_base || pc >= lx_cpu.cache_end)
			return 0;                        // (not translated code)
		gFaultInsn = *(TUint32*)pc;
		gFaultWhy = 3;
		lx_u32 regs[15];
		regs[0] = f[11]; regs[1] = f[12]; regs[2] = f[13];
		for (TInt k = 3; k <= 12; k++)
			regs[k] = f[k - 3];
		regs[13] = 0;
		regs[14] = f[10];
		if (dbt_fixup(regs, (lx_u32)(f + 15), *(TUint32*)pc) < 0)
			return 0;
		dbt_patch_unaligned(pc);             // (no more exceptions from here)
		f[11] = regs[0]; f[12] = regs[1]; f[13] = regs[2];
		for (TInt j = 3; j <= 12; j++)
			f[j - 3] = regs[j];
		f[10] = regs[14];
		f[14] = pc + 4;
		return 1;
		}
	return 0;
	}

extern "C" void LxExcHandlerC(TExcType aType, TUint32* aEntrySp);
extern "C" void lx_exc_entry(TExcType aType);
// (the flags as the faulting code had them are restored on the way out:
// EPOC does not keep them across an exception handler, M0)
asm(
"	.text\n"
"	.align 2\n"
"	.global lx_exc_entry\n"
"lx_exc_entry:\n"
"	.word 0xE10F1000\n"            // mrs r1, cpsr
"	stmfd sp!, {r1, lr}\n"
"	mov r1, sp\n"
"	bl LxExcHandlerC\n"
"	ldmfd sp!, {r1, lr}\n"
"	.word 0xE128F001\n"            // msr cpsr_f, r1
"	mov pc, lr\n"
);

// the translated thread holding the CPU, if this is it
static lx_thread* CurrentDbt()
	{
	lx_thread* t = lx_cpu.cur;
	if (!t || !t->bk)
		return 0;
	if (((THThread*)t->bk)->iThread.Id() == RThread().Id())
		return t;
	return 0;
	}

extern "C" void LxExcHandlerC(TExcType aType, TUint32* aEntrySp)
	{
	lx_thread* t = CurrentDbt();
	gFaultWhy = 0;
	if (t && aType == EExcAlignment && !lx_cpu.critical && FixAlignment(aEntrySp))
		return;
	if (t && aType == EExcAlignment)
		t->fault_addr = gFaultPc;
	if (!t)
		t = lx_thread_by_sp(CurrentSp());
	if (!t || !t->bk)
		{
		RSemaphore never;
		never.CreateLocal(0);
		never.Wait();
		}
	THThread* h = (THThread*)t->bk;
	TInt sig = LX_SIGSEGV;
	switch (aType)
		{
	case EExcInvalidOpCode: case EExcPrivInstruction: sig = LX_SIGILL; break;
	case EExcIntegerDivideByZero: case EExcIntegerOverflow: sig = LX_SIGFPE; break;
	case EExcAlignment: sig = LX_SIGBUS; break;
	case EExcBreakPoint: case EExcSingleStep: sig = LX_SIGTRAP; break;
	case EExcKill: case EExcAbort: sig = LX_SIGKILL; break;
	default: break;
		}
	h->iFault = sig;
	if (t->state == LX_T_USER)
		{
		TRequestStatus* s = h->iCallStatus;  // (armed: it is not in a call)
		gKernel.RequestComplete(s, KErrNone);
		}
	// the kernel kills this thread; the housekeeping tick finds it if the
	// call request could not be used
	for (;;)
		User::WaitForAnyRequest();
	}

static TInt LxThreadMain(TAny* aPtr)
	{
	lx_thread* t = (lx_thread*)aPtr;
	RThread().SetExceptionHandler(lx_exc_entry, KExceptionAbort | KExceptionKill | KExceptionFault | KExceptionInteger | KExceptionDebug);
	lx_thread_start(t);                      // (never returns)
	return KErrNone;
	}

extern "C" int bk_thread_new(lx_thread* t)
	{
	THThread* h = (THThread*)AllocZ(sizeof(THThread));
	if (!h)
		return -LX_ENOMEM;
	h->iT = t;
	TBuf<16> name;
	name.Format(_L("lx%d"), ++gThreadNo);
	TInt r = h->iThread.Create(name, LxThreadMain, 0x4000, &User::Heap(), t, EOwnerProcess);
	if (r != KErrNone)
		{
		User::Free(h);
		return Err(r);
		}
	h->iThread.SetPriority(EPriorityLess);
	h->iCall = new CCallAO(t);
	h->iDeath = new CDeathAO(t);
	if (!h->iCall || !h->iDeath)
		{
		delete h->iCall;
		delete h->iDeath;
		h->iThread.Kill(0);
		h->iThread.Close();
		User::Free(h);
		return -LX_ENOMEM;
		}
	h->iCallStatus = &h->iCall->iStatus;
	h->iCall->Arm();
	h->iDeath->Start(h->iThread);
	t->bk = h;
	return 0;
	}

extern "C" void bk_thread_start(lx_thread* t)
	{
	((THThread*)t->bk)->iThread.Resume();
	}

extern "C" void bk_thread_reply(lx_thread* t)
	{
	THThread* h = (THThread*)t->bk;
	h->iCall->Arm();                         // (ready for its next call before it can make one)
	TRequestStatus* s = &h->iReply;
	h->iThread.RequestComplete(s, KErrNone);
	}

extern "C" void bk_thread_kill(lx_thread* t)
	{
	THThread* h = (THThread*)t->bk;
	if (h && h->iThread.ExitType() == EExitPending)
		h->iThread.Kill(0);
	}

extern "C" void bk_thread_suspend(lx_thread* t) { if (t->bk) ((THThread*)t->bk)->iThread.Suspend(); }
extern "C" void bk_thread_resume(lx_thread* t) { if (t->bk) ((THThread*)t->bk)->iThread.Resume(); }

extern "C" void bk_thread_free(lx_thread* t)
	{
	THThread* h = (THThread*)t->bk;
	if (!h)
		return;
	delete h->iCall;                         // (the death AO deletes itself)
	h->iThread.Close();
	User::Free(h);
	t->bk = 0;
	}

// on the Linux thread
extern "C" void bk_thread_call(lx_thread* t)
	{
	THThread* h = (THThread*)t->bk;
	h->iReply = KRequestPending;
	TRequestStatus* s = h->iCallStatus;
	gKernel.RequestComplete(s, KErrNone);
	User::WaitForRequest(h->iReply);
	}

extern "C" void bk_thread_exit_self(lx_thread*) { User::Exit(KErrNone); }
extern "C" void bk_cas_yield(void) { User::After(1); }

// ---- memory ------------------------------------------------------------------

extern "C" int bk_region_new(lx_region* r, lx_u32 size, int kind)
	{
	RChunk* c = new RChunk;
	if (!c)
		return -LX_ENOMEM;
	TInt e;
	size = (size + 4095) & ~4095u;
	if (kind == LX_REG_STACK)
		{
		// a stack with 16 KB uncommitted below it: running off its end
		// faults (SIGSEGV) instead of overwriting something else
		const TInt KGuard = 0x4000;
		e = c->CreateDoubleEndedLocal(KGuard, KGuard + size, KGuard + size);
		if (e == KErrNone)
			r->base = (lx_u32)(c->Base() + c->Bottom());
		}
	else
		{
		e = c->CreateLocal(size, size);
		if (e == KErrNone)
			r->base = (lx_u32)c->Base();
		}
	if (e != KErrNone)
		{
		delete c;
		return Err(e);
		}
	r->size = size;
	r->bk = c;
	return 0;
	}

extern "C" void bk_region_free(lx_region* r)
	{
	RChunk* c = (RChunk*)r->bk;
	if (c)
		{
		c->Close();
		delete c;
		}
	r->bk = 0;
	r->size = 0;
	}

const TInt KArenaMax = 24 * 1024 * 1024;      // address space: RAM is the real limit

extern "C" lx_u32 bk_arena_base(void)
	{
	if (!gArenaOk && gArena.CreateLocal(0, KArenaMax) == KErrNone)
		gArenaOk = ETrue;
	return gArenaOk ? (lx_u32)gArena.Base() : 0;
	}
extern "C" lx_u32 bk_arena_max(void) { return gArenaOk ? KArenaMax : 0; }

// Alpine mode's code cache: max bytes of address space, size committed
extern "C" int bk_code_region(lx_region* r, lx_u32 max, lx_u32 size)
	{
	RChunk* c = new RChunk;
	if (!c)
		return -LX_ENOMEM;
	TInt e = c->CreateLocal(size, max);
	if (e != KErrNone)
		{
		delete c;
		return Err(e);
		}
	r->base = (lx_u32)c->Base();
	r->size = max;
	r->bk = c;
	return 0;
	}
extern "C" int bk_code_grow(lx_region* r, lx_u32 size) { return Err(((RChunk*)r->bk)->Adjust(size)); }
extern "C" int bk_arena_grow(lx_u32 size) { return gArenaOk ? Err(gArena.Adjust((size + 4095) & ~4095u)) : -LX_ENOMEM; }
extern "C" void* bk_alloc(lx_u32 n) { return AllocZ(n ? n : 1); }
extern "C" void bk_free(void* p) { User::Free(p); }
extern "C" void bk_memcpy(void* d, const void* s, lx_u32 n) { Mem::Copy(d, s, n); }
extern "C" void bk_memmove(void* d, const void* s, lx_u32 n) { Mem::Copy(d, s, n); }
extern "C" void bk_memset(void* d, int c, lx_u32 n) { Mem::Fill(d, n, TChar(c & 0xff)); }

// Preferences: "Keep memory for other programs" (the app sets it, and may
// change it while Linux runs): lx_reserve_ok checks each growth against it
extern "C" lx_u32 bk_mem_reserve(void) { return gSh ? gSh->reserve : 0; }

extern "C" void bk_mem_info(lx_u32* total, lx_u32* freeb)
	{
	TMemoryInfoV1Buf mi;
	UserHal::MemoryInfo(mi);
	*total = mi().iTotalRamInBytes;
	*freeb = mi().iFreeRamInBytes;
	}

// ---- time ----------------------------------------------------------------------

static lx_s64 From64(const TInt64& a) { return (lx_s64)(((lx_u64)a.High() << 32) | a.Low()); }
static TInt64 To64(lx_s64 a) { return TInt64((TUint)((lx_u64)a >> 32), (TUint)a); }

static TInt UtcOffset()
	{
	TLocale loc;
	return loc.UniversalTimeOffset().Int();
	}

extern "C" lx_s64 bk_now_real(void)
	{
	TTime t;
	t.UniversalTime();
	return From64(t.Int64()) - From64(gEpoch);
	}

extern "C" lx_s64 bk_now_mono(void)
	{
	return (lx_s64)User::TickCount() * 15625;
	}

static TInt64 gSeed;
extern "C" lx_u32 bk_random(void)
	{
	TUint a = Math::Rand(gSeed), b = Math::Rand(gSeed);
	return (a << 16) ^ b ^ User::TickCount();
	}

// The kernel's wake-up. It is set again only when it must go off sooner
// than it is set for now: a timer that goes off early costs one look at the
// parked calls, and cancelling and setting an RTimer after every system call
// (hundreds of times a second) is needless work. (It was first suspected of
// the emulator's timer-queue fault; that turned out to follow the emulator's
// card workaround instead: docs/DECISIONS.md 21.)
class CLxTimer : public CTimer
	{
public:
	CLxTimer() : CTimer(EPriorityStandard), iAt(0) {}
	void ConstructL() { CTimer::ConstructL(); CActiveScheduler::Add(this); }
	lx_s64 iAt;                  // when it is set to go off (0: not set)
private:
	void RunL() { iAt = 0; lx_k_timer(); }
	};
static CLxTimer* gTimer;

extern "C" void bk_wake_at(lx_s64 mono)
	{
	if (!gTimer || !mono)
		return;                              // (nothing to wait for: a timer set goes off harmlessly)
	if (gTimer->IsActive() && gTimer->iAt && gTimer->iAt <= mono)
		return;                              // it goes off soon enough
	gTimer->Cancel();
	lx_s64 d = mono - bk_now_mono();
	if (d < 0) d = 0;
	if (d > 1800000000) d = 1800000000;      // (half an hour; it is set again when it fires)
	gTimer->iAt = bk_now_mono() + d;
	gTimer->After(TTimeIntervalMicroSeconds32((TInt)d));
	}

// ---- logging -------------------------------------------------------------------

extern "C" void bk_log(const char* fmt, ...)
	{
	char buf[300];
	va_list ap;
	va_start(ap, fmt);
	int n = lx_vsnprintf_pub(buf, sizeof buf - 2, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 3) n = sizeof buf - 3;
	buf[n++] = '\r';
	buf[n++] = '\n';
	if (gLogOpen)
		{
		TInt size = 0;
		gLog.Size(size);
		if (size > (lx_k.trace ? 4096 * 1024 : 64 * 1024))
			gLog.SetSize(0), size = 0;       // (keep it small)
		gLog.Write(size, TPtrC8((const TUint8*)buf, n));
		gLog.Flush();
		}
	}

// ---- the terminal ---------------------------------------------------------------

static TInt OutFree()
	{
	if (!gSh)
		return 0;
	return LX_OUT_SIZE - (TInt)(gSh->out_head - gSh->out_tail);
	}

extern "C" int bk_tty_room(lx_tty* tty)
	{
	if (tty->index != 0)
		return 4096;                         // (more windows: one day)
	return OutFree();
	}

extern "C" int bk_tty_write(lx_tty* tty, const char* buf, int n)
	{
	if (tty->index != 0 || !gSh)
		return n;
	TInt room = OutFree();
	if (n > room)
		n = room;
	for (TInt i = 0; i < n; i++)
		gSh->out[(gSh->out_head + i) % LX_OUT_SIZE] = buf[i];
	gSh->out_head += n;
	gAppRinger.Ring(&gSh->app_bell);
	return n;
	}

static TBool gStopping;
extern "C" void bk_tty_status(lx_tty* tty, int running, int status)
	{
	if (!gSh || tty->index != 0 || running)
		return;
	gSh->exit_status = status;
	gSh->state = LXS_EXITED;
	gAppRinger.Ring(&gSh->app_bell);
	if (!gStopping)
		{
		gStopping = ETrue;
		CActiveScheduler::Stop();
		}
	}

// Keys, a resize, a quit or room in 'out' from the app: its doorbell.
class CLxTerm : public CBase
	{
public:
	void ConstructL()
		{
		iWaiter = new(ELeave) CPsiBellWaiter(TCallBack(Rung, this));
		iTick = CPeriodic::NewL(CActive::EPriorityLow);
		iTick->Start(1000000, 1000000, TCallBack(Tick, this));
		iResize = gSh->resize_seq - 1;
		Look();
		}
	~CLxTerm() { delete iWaiter; delete iTick; }
	void Look()
		{
		for (TInt round = 0; round < 8; round++)
			{
			if (gSh->quit)
				{
				Quit();
				return;
				}
			if (gSh->resize_seq != iResize)
				{
				iResize = gSh->resize_seq;
				lx_k_tty_resize(&lx_k.ttys[0], gSh->rows, gSh->cols);
				}
			TUint head = gSh->kbd_head;
			if (head != gSh->kbd_tail)
				{
				char buf[256];
				TInt n = 0;
				while (gSh->kbd_tail != head && n < (TInt)sizeof buf)
					{
					buf[n++] = gSh->kbd[gSh->kbd_tail % LX_KBD_SIZE];
					gSh->kbd_tail++;
					}
				lx_k_tty_input(&lx_k.ttys[0], buf, n);
				}
			if (OutFree() != iFree)
				{
				iFree = OutFree();
				lx_k_tty_writable(&lx_k.ttys[0]);
				}
			if (gSh->bell_magic != PSI_BELL_MAGIC || !iWaiter->Arm(&gSh->eng_bell))
				return;                      // armed (or no bells: the tick looks)
			}
		}
	static TInt Rung(TAny* aSelf) { ((CLxTerm*)aSelf)->Look(); return 0; }
	static TInt Tick(TAny* aSelf) { ((CLxTerm*)aSelf)->Housekeeping(); return 1; }
	void Housekeeping();
	void Quit()
		{
		if (gStopping)
			return;
		gStopping = ETrue;
		lx_k_shutdown();
		CActiveScheduler::Stop();
		}
private:
	CPsiBellWaiter* iWaiter;
	CPeriodic* iTick;
	TUint iResize;
	TInt iFree;
	};
static const TBool KExecLogDefault = EFalse;   // (ETrue in test builds: build/dbt-scratch/mk019.sh)
static TInt gStatusNext = 30;
static CLxTerm* gTerm;

// Once a second: faults that could not be reported at once, the app's
// heartbeat, and the numbers for the status line.
void CLxTerm::Housekeeping()
	{
	TInt i;
	for (i = 0; i < LX_MAXTHREAD; i++)
		{
		lx_thread* t = &lx_k.threads[i];
		if (t->state == LX_T_FREE || !t->bk)
			continue;
		THThread* h = (THThread*)t->bk;
		if (h->iFault && h->iThread.ExitType() == EExitPending)
			h->iThread.Kill(0);
		}
	if (gSh->app_beat != gLastBeat)
		{
		gLastBeat = gSh->app_beat;
		gBeatTicks = 0;
		}
	else if (++gBeatTicks > 45)
		{
		Quit();                              // the app has gone (crashed, or killed)
		return;
		}
	TInt n = 0;
	TUint used = 0;
	TInt secs = (TInt)((bk_now_mono() - lx_k.start_mono) / 1000000);
	if (lx_k.execlog && secs >= gStatusNext)
		{
		// every 30 s in Alpine mode: memory, and how the translator is doing
		lx_u32 tot, fr;
		bk_mem_info(&tot, &fr);
		gStatusNext = secs + 30;
		bk_log("%d s: %u KB free; translated %u blocks (%u KB of code), %u flushes; %u syscalls, %u helpers; unaligned: %u faults, %u sites, %u via sites",
			secs, fr / 1024, lx_cpu.translations, lx_cpu.host_words / 256, lx_cpu.flushes, lx_cpu.syscalls, lx_cpu.helpers,
			lx_cpu.fixups, lx_cpu.patched, lx_cpu.fixed_calls);
		}
	for (i = 0; i < LX_MAXPROC; i++)
		{
		lx_proc* p = &lx_k.procs[i];
		if (p->state != LX_P_ALIVE || p->pid == 1)
			continue;
		n++;
		if (p->mm && p->mm->refs && p->vfork_parent == 0)
			used += p->mm->image.size + p->mm->mapped;
		}
	for (i = 0; i < LX_MAXTHREAD; i++)
		if (lx_k.threads[i].state != LX_T_FREE)
			used += lx_k.threads[i].stack.size;
	lx_u32 total, freeb;
	bk_mem_info(&total, &freeb);
	// the foreground job's name
	char fg[32];
	fg[0] = 0;
	lx_tty* tty = &lx_k.ttys[0];
	for (i = 0; i < LX_MAXPROC && tty->pgrp; i++)
		{
		lx_proc* p = &lx_k.procs[i];
		if (p->state == LX_P_ALIVE && p->pgid == tty->pgrp)
			lx_strlcpy(fg, p->comm, sizeof fg);
		}
	TBool changed = n != gSh->nprocs || used / 16384 != gSh->mem_used / 16384 || freeb / 16384 != gSh->mem_free / 16384
		|| lx_strcmp(fg, gSh->fg_name) != 0;
	gSh->nprocs = n;
	gSh->mem_used = used;
	gSh->mem_free = freeb;
	lx_strlcpy(gSh->fg_name, fg, sizeof gSh->fg_name);
	if (changed)
		{
		gSh->info_seq++;
		gAppRinger.Ring(&gSh->app_bell);
		}
	Look();
	}

// ---- files -----------------------------------------------------------------------

extern "C" void* bk_file_open(const char* path, int flags, int* err)
	{
	TFileName name;
	Name(name, path);
	RFile* f = new RFile;
	if (!f)
		{
		*err = -LX_ENOMEM;
		return 0;
		}
	TInt acc = flags & LX_O_ACCMODE;
	TUint mode = EFileShareAny | (acc == LX_O_RDONLY ? EFileRead : EFileWrite);
	TInt r;
	if ((flags & LX_O_CREAT) && (flags & LX_O_EXCL))
		r = f->Create(gFs, name, mode);
	else
		{
		r = f->Open(gFs, name, mode);
		if (r == KErrNotFound && (flags & LX_O_CREAT))
			r = f->Create(gFs, name, mode);
		}
	if (r != KErrNone)
		{
		delete f;
		*err = Err(r);
		return 0;
		}
	return f;
	}

extern "C" int bk_file_read(void* h, lx_s64 off, char* buf, lx_u32 n)
	{
	TPtr8 p((TUint8*)buf, 0, n);
	TInt r = ((RFile*)h)->Read((TInt)off, p, n);
	return r != KErrNone ? Err(r) : p.Length();
	}

extern "C" int bk_file_write(void* h, lx_s64 off, const char* buf, lx_u32 n)
	{
	RFile* f = (RFile*)h;
	TInt size = 0;
	if (f->Size(size) == KErrNone && off > size)
		f->SetSize((TInt)off);               // (a hole: F32 will not write past the end)
	TInt r = f->Write((TInt)off, TPtrC8((const TUint8*)buf, n), n);
	return r != KErrNone ? Err(r) : (int)n;
	}

extern "C" int bk_file_size(void* h, lx_s64* size)
	{
	TInt s = 0;
	TInt r = ((RFile*)h)->Size(s);
	*size = s;
	return Err(r);
	}

extern "C" int bk_file_truncate(void* h, lx_s64 size) { return Err(((RFile*)h)->SetSize((TInt)size)); }
extern "C" int bk_file_sync(void* h) { return Err(((RFile*)h)->Flush()); }
extern "C" void bk_file_close(void* h) { ((RFile*)h)->Close(); delete (RFile*)h; }

static TBool IsDriveRoot(const TDesC& aName)
	{
	return aName.Length() <= 3 && aName.Length() >= 2 && aName[1] == ':';
	}

static void Trim(TDes& aName)
	{
	if (aName.Length() > 3 && aName[aName.Length() - 1] == '\\')
		aName.SetLength(aName.Length() - 1);
	}

extern "C" int bk_stat_path(const char* path, bk_stat* st)
	{
	TFileName name;
	Name(name, path);
	Trim(name);
	if (IsDriveRoot(name))
		{
		TInt d = EDriveC;
		RFs::CharToDrive(name[0], d);
		TDriveInfo di;
		if (gFs.Drive(di, d) != KErrNone || di.iType == EMediaNotPresent)
			return -LX_ENOENT;
		st->is_dir = 1;
		st->size = 0;
		st->mtime = 0;
		st->readonly = 0;
		return 0;
		}
	TEntry e;
	TInt r = gFs.Entry(name, e);
	if (r != KErrNone)
		return Err(r);
	st->is_dir = e.IsDir() ? 1 : 0;
	st->size = e.iSize;
	st->mtime = From64(e.iModified.Int64()) - From64(gEpoch) - (lx_s64)UtcOffset() * 1000000;
	st->readonly = e.IsReadOnly() ? 1 : 0;
	return 0;
	}

static int DirOp(const char* path, TInt aOp)
	{
	TFileName name;
	Name(name, path);
	if (aOp < 2 && (name.Length() == 0 || name[name.Length() - 1] != '\\'))
		name.Append('\\');
	switch (aOp)
		{
	case 0: return Err(gFs.MkDir(name));
	case 1: return Err(gFs.RmDir(name));
	default: return Err(gFs.Delete(name));
		}
	}
extern "C" int bk_mkdir(const char* path) { return DirOp(path, 0); }
extern "C" int bk_rmdir(const char* path) { return DirOp(path, 1); }
extern "C" int bk_unlink(const char* path) { return DirOp(path, 2); }

extern "C" int bk_rename(const char* from, const char* to)
	{
	TFileName a, b;
	Name(a, from);
	Name(b, to);
	Trim(a);
	Trim(b);
	return Err(gFs.Rename(a, b));
	}

extern "C" int bk_set_mtime(const char* path, lx_s64 mtime)
	{
	TFileName name;
	Name(name, path);
	Trim(name);
	TTime t(To64(mtime + From64(gEpoch) + (lx_s64)UtcOffset() * 1000000));
	return Err(gFs.SetModified(name, t));
	}

struct THDir
	{
	CDir* iDir;
	TInt iNext;
	char iName[260];
	};

extern "C" void* bk_dir_open(const char* path, int* err)
	{
	TFileName name;
	Name(name, path);
	if (name.Length() == 0 || name[name.Length() - 1] != '\\')
		name.Append('\\');
	name.Append('*');
	THDir* d = (THDir*)AllocZ(sizeof(THDir));
	if (!d)
		{
		*err = -LX_ENOMEM;
		return 0;
		}
	TInt r = gFs.GetDir(name, KEntryAttMaskSupported, ESortByName, d->iDir);
	if (r != KErrNone)
		{
		User::Free(d);
		*err = Err(r);
		return 0;
		}
	return d;
	}

extern "C" const char* bk_dir_next(void* dp, bk_stat* st)
	{
	THDir* d = (THDir*)dp;
	if (!d->iDir || d->iNext >= d->iDir->Count())
		return 0;
	const TEntry& e = (*d->iDir)[d->iNext++];
	TPtr8 out((TUint8*)d->iName, 0, sizeof d->iName - 1);
	out.Copy(e.iName);
	d->iName[out.Length()] = 0;
	st->is_dir = e.IsDir() ? 1 : 0;
	st->size = e.iSize;
	st->mtime = From64(e.iModified.Int64()) - From64(gEpoch) - (lx_s64)UtcOffset() * 1000000;
	st->readonly = e.IsReadOnly() ? 1 : 0;
	return d->iName;
	}

extern "C" void bk_dir_close(void* dp)
	{
	THDir* d = (THDir*)dp;
	delete d->iDir;
	User::Free(d);
	}

extern "C" int bk_disk_info(const char* path, lx_u64* size, lx_u64* freeb)
	{
	TFileName name;
	Name(name, path);
	TInt drive = EDriveC;
	if (name.Length() >= 2 && name[1] == ':')
		RFs::CharToDrive(name[0], drive);
	TVolumeInfo v;
	TInt r = gFs.Volume(v, drive);
	if (r != KErrNone)
		return Err(r);
	*size = (lx_u64)From64(v.iSize);
	*freeb = (lx_u64)From64(v.iFree);
	return 0;
	}

extern "C" int bk_drive_list(char* drives)
	{
	TDriveList list;
	TInt n = 0;
	if (gFs.DriveList(list) != KErrNone)
		{
		drives[0] = 0;
		return 0;
		}
	for (TInt d = 0; d < KMaxDrives && d < list.Length(); d++)
		{
		if (!list[d])
			continue;
		TDriveInfo di;
		if (gFs.Drive(di, d) != KErrNone || di.iType == EMediaNotPresent)
			continue;
		TChar c;
		if (RFs::DriveToChar(d, c) == KErrNone)
			drives[n++] = (char)(TUint)c;
		}
	drives[n] = 0;
	return n;
	}

extern "C" char bk_native_sep(void) { return '\\'; }

// ---- the runner ---------------------------------------------------------------------

static void OpenLog(const char* aRoot)
	{
	TFileName name;
	Name(name, aRoot);
	if (name.Length() && name[name.Length() - 1] != '\\')
		name.Append('\\');
	gFs.MkDirAll(name);
	name.Append(_L("PsiLinux.log"));
	if (gLog.Open(gFs, name, EFileWrite | EFileShareAny) != KErrNone && gLog.Replace(gFs, name, EFileWrite | EFileShareAny) != KErrNone)
		return;
	gLogOpen = ETrue;
	}

static void Fail(const char* aWhy)
	{
	if (!gSh)
		return;
	lx_strlcpy(gSh->message, aWhy, sizeof gSh->message);
	gSh->state = LXS_FAILED;
	gAppRinger.Ring(&gSh->app_bell);
	}

static void RunL()
	{
	CActiveScheduler* as = new(ELeave) CActiveScheduler;
	CleanupStack::PushL(as);
	CActiveScheduler::Install(as);
	User::LeaveIfError(gKernel.Duplicate(RThread(), EOwnerProcess));
	User::LeaveIfError(gFs.Connect());
	User::LeaveIfError(gShChunk.OpenGlobal(_L(LX_SHARED_NAME), EFalse));
	gSh = (LxShared*)gShChunk.Base();
	if (gSh->magic != LX_SHARED_MAGIC || gSh->version != LX_SHARED_VERSION)
		User::Leave(KErrNotSupported);
	{
		TTime epoch(TDateTime(1970, EJanuary, 0, 0, 0, 0, 0));
		gEpoch = epoch.Int64();
		TTime now;
		now.HomeTime();
		gSeed = now.Int64();
	}
	gSh->root[sizeof gSh->root - 1] = 0;
	gSh->program[sizeof gSh->program - 1] = 0;
	OpenLog(gSh->root);
	bk_log("PsiLinux runner %s: root %s, program %s", LX_VERSION, gSh->root, gSh->program);
	gTimer = new(ELeave) CLxTimer;
	gTimer->ConstructL();
	lx_k_init(gSh->root);
	{
		// <root>\TRACE: log every call (for debugging)
		TFileName t;
		Name(t, gSh->root);
		t.Append(_L("TRACE"));
		TEntry e;
		if (gFs.Entry(t, e) == KErrNone)
			lx_k.trace = 1;
		Name(t, gSh->root);
		t.Append(_L("NOCACHE"));
		if (gFs.Entry(t, e) == KErrNone)
			lx_k.no_cache = 1;
	}
	{
		lx_u32 ents = 0, bytes = 0;
		lx_root_info(&ents, &bytes);
		bk_log("kernel ready: root image %u entries (%u bytes of index)", ents, bytes);
	}
	{
		// Alpine mode: its version, for the app's About
		int n = lx_read_file("/etc/alpine-release", gSh->release, sizeof gSh->release - 1);
		while (n > 0 && (gSh->release[n - 1] == '\n' || gSh->release[n - 1] == '\r'))
			n--;
		gSh->release[n > 0 ? n : 0] = 0;
		if (gSh->release[0])
			{
			// Alpine mode: a status line every 30 s; with <root>\BOOTLOG (or in
			// a test build) also a line for each program started, to show where
			// a boot got to (many small writes to the card, so not by default)
			TFileName b;
			Name(b, gSh->root);
			b.Append(_L("BOOTLOG"));
			TEntry be;
			lx_k.execlog = (KExecLogDefault || gFs.Entry(b, be) == KErrNone) ? 2 : 1;
			bk_log("Alpine Linux %s", gSh->release);
			}
	}
	lx_k.ttys[0].ws.ws_row = (lx_u16)(gSh->rows > 0 ? gSh->rows : 24);
	lx_k.ttys[0].ws.ws_col = (lx_u16)(gSh->cols > 0 ? gSh->cols : 80);
	// the first program's argv: program, then the NUL-separated args
	char* argv[17];
	TInt argc = 0;
	argv[argc++] = gSh->program;
	const char* a = gSh->args;
	while (*a && argc < 16 && a < gSh->args + sizeof gSh->args)
		{
		argv[argc++] = (char*)a;
		a += lx_strlen(a) + 1;
		}
	argv[argc] = 0;
	static char* envp[] = {
		(char*)"HOME=/root", (char*)"PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin",
		(char*)"TERM=xterm-256color", (char*)"USER=root", (char*)"LOGNAME=root", (char*)"SHELL=/bin/sh",
		(char*)"LANG=C.UTF-8", (char*)"PSION=5mx", (char*)"PAGER=less", (char*)"EDITOR=vi", 0 };
	gTerm = new(ELeave) CLxTerm;
	gTerm->ConstructL();
	bk_log("starting %s", gSh->program);
	int pid;
	if (lx_root_missing())
		{
		// (a root image in parts, one of its .sis files not installed)
		char why[150];
		lx_snprintf(why, sizeof why, "The root image in %s is not complete: %s is missing. Install all its .sis files (part 1, part 2...) on that disk", gSh->root, lx_root_missing());
		bk_log("%s", why);
		Fail(why);
		pid = -1;
		}
	else
		pid = lx_k_spawn_first(gSh->program, argv, envp, 0);
	if (pid < 0 && !lx_root_missing() && lx_strcmp(gSh->program, "/usr/sbin/psilinux-shell") == 0)
		{
		// (an Alpine root from before 0.21: no psilinux-shell, so sh)
		static char sh[] = "/bin/sh", dashl[] = "-l";
		static char* shargv[] = { sh, dashl, 0 };
		bk_log("no %s: starting /bin/sh", gSh->program);
		pid = lx_k_spawn_first(sh, shargv, envp, 0);
		}
	if (pid < 0 && lx_root_missing())
		;                                    // (said above)
	else if (pid < 0)
		{
		char why[120];
		lx_snprintf(why, sizeof why, "Could not start %s (error %d): is the Linux root image in %sROOT.IMG?", gSh->program, -pid, gSh->root);
		bk_log("%s", why);
		Fail(why);
		}
	else
		{
		bk_log("started %s: pid %d", gSh->program, pid);
		gSh->state = LXS_RUNNING;
		gAppRinger.Ring(&gSh->app_bell);
		CActiveScheduler::Start();
		}
	gStopping = ETrue;
	lx_k_shutdown();
	bk_log("PsiLinux runner: stopped");
	delete gTerm;
	gTerm = 0;
	if (gSh->state != LXS_FAILED && gSh->state != LXS_EXITED)
		gSh->state = LXS_EXITED;
	gAppRinger.Ring(&gSh->app_bell);
	gAppRinger.Close();
	CleanupStack::PopAndDestroy();     // as
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* tc = CTrapCleanup::New();
	TRAPD(err, RunL());
	if (err != KErrNone && gSh)
		{
		char why[80];
		lx_snprintf(why, sizeof why, "The Linux runner stopped (error %d)", err);
		Fail(why);
		}
	if (gLogOpen)
		gLog.Close();
	gFs.Close();
	delete tc;
	return err;
	}
