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
#include "lxfb.h"
#include "lxsync.h"
#include "lxepoc.h"
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
static TInt gThreads;                    // (0.38) the Linux threads' EPOC threads, for /proc/psilinux/mem
static TInt gThreadNo;
static TPsiBellRinger gAppRinger;
static TInt64 gEpoch;            // 1970, as a TTime
static RFile gLog;
static TBool gLogOpen;
static TUint gLastBeat, gBeatTicks;
static TUint32 gFaultPc, gFaultInsn, gFaultWhy;   // an unfixed alignment fault (the kernel thread logs it)
static TUint32 gFaultIdx, gFaultSp, gFaultW[20];
static TThreadId gKernelId;      // (0.29) the kernel thread's id (bk_log, faults)
static TBool gKernelIdSet;

// (0.37) The end log: one line for each session that ends, and why, always
// (not only with Tools > Keep a log), beside the runner in \System\Apps\
// PsiLinux\ (PsiLinux-end.log, PsiLinux-end.old before it): a session
// that ended "by itself" says what ended it. gEndWhy is the runner's own
// reason, when it ended the session (the app asked, the app's heartbeat
// stopped, a reboot); the kernel's lx_k.end_note says how the first
// program ended.
static const char* gEndWhy;
static TBool gEndNoted;
static void EndNote(const char* aWhat)
	{
	if (gEndNoted)
		return;
	gEndNoted = ETrue;
	TParse pa;
	TFileName exe(RProcess().FileName());
	if (pa.Set(exe, NULL, NULL) != KErrNone)
		return;
	TFileName name(pa.DriveAndPath());
	TFileName old(name);
	name.Append(_L("PsiLinux-end.log"));
	old.Append(_L("PsiLinux-end.old"));
	RFile f;
	TInt size = 0;
	if (f.Open(gFs, name, EFileWrite | EFileShareAny) == KErrNone)
		{
		f.Size(size);
		if (size > 8192)
			{
			f.Close();
			gFs.Delete(old);
			gFs.Rename(name, old);
			size = -1;
			}
		}
	else
		size = -1;
	if (size < 0 && f.Replace(gFs, name, EFileWrite | EFileShareAny) != KErrNone)
		return;
	TInt pos = 0;
	f.Seek(ESeekEnd, pos);
	TTime now;
	now.HomeTime();
	TDateTime d = now.DateTime();
	char line[400];
	TInt n = lx_snprintf(line, sizeof line - 2, "%04d-%02d-%02d %02d:%02d:%02d %s: %s%s%s",
		d.Year(), d.Month() + 1, d.Day() + 1, d.Hour(), d.Minute(), d.Second(), LX_VERSION,
		gEndWhy ? gEndWhy : "", gEndWhy ? "; " : "", aWhat);
	if (n > (TInt)sizeof line - 3)
		n = sizeof line - 3;
	line[n++] = '\r';
	line[n++] = '\n';
	f.Write(TPtrC8((const TUint8*)line, n));
	f.Close();
	}
// (0.29) how deep the kernel thread's stack has gone: its sp is looked at
// where the kernel is deepest (the backend's file calls, its allocations,
// the log), against where it was at the start (KStackSize is petran's
// -stack in the Makefile)
const TUint32 KStackSize = 0x10000;
static TUint32 gStackTop, gStackMin;
static inline void StackSample()
	{
	TUint32 sp;
	__asm__ __volatile__("mov %0, sp" : "=r"(sp));
	if (sp < gStackMin && gKernelIdSet)
		gStackMin = sp;
	}
static TUint32 StackUsed()
	{
	return (gStackTop && gStackMin && gStackMin < gStackTop) ? gStackTop - gStackMin : 0;
	}
static void Fail(const char* aWhy);
static void OpenLog(const char* aRoot);

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

// (0.29) A fault in the kernel thread itself (the runner's "Main"). Before
// 0.29 it had no handler: EPOC ended the runner with KERN-EXEC 3 and nothing
// was logged. Now it says where (the registers from EUSER's frame, the words
// on its stack, the instruction and the address it used, and what the
// kernel was doing), tells the app, and waits for the app to end the runner
// (so there is no "Program closed" dialog). The runner's own pc maps to
// build/lxrun-obj/psilxrun.map through the address of E32Main logged with it.
GLDEF_C TInt E32Main();
static TUint32 AccessAddress(const TUint32* aR, TUint32 aInsn)
	{
	// the address a load or store used, from its base register (0: not one)
	TUint32 rn = aR[(aInsn >> 16) & 15];
	TUint32 up = aInsn & (1u << 23), pre = aInsn & (1u << 24);
	if ((aInsn & 0x0C000000u) == 0x04000000u)        // LDR/STR (immediate or register offset)
		{
		TUint32 off = (aInsn & (1u << 25)) ? aR[aInsn & 15] : (aInsn & 0xFFF);   // (a shifted register: unshifted)
		return pre ? (up ? rn + off : rn - off) : rn;
		}
	if ((aInsn & 0x0E000090u) == 0x00000090u && (aInsn & 0x60))   // LDRH/STRH/LDRSB/LDRSH
		{
		TUint32 off = (aInsn & (1u << 22)) ? (((aInsn >> 4) & 0xF0) | (aInsn & 15)) : aR[aInsn & 15];
		return pre ? (up ? rn + off : rn - off) : rn;
		}
	if ((aInsn & 0x0E000000u) == 0x08000000u)        // LDM/STM
		return rn;
	return 0;
	}

// Waits for the app to end the runner, using no processor. (Not a loop of
// User::WaitForAnyRequest: the kernel thread's requests that had completed
// let it return at once, again and again, and at the kernel thread's
// priority that loop starved PsiLinux's window, which then never ended it.)
static void WaitToBeEnded()
	{
	RThread().SetPriority(EPriorityMuchLess);
	RSemaphore never;
	if (never.CreateLocal(0) == KErrNone)
		for (;;)
			never.Wait();
	for (;;)
		User::After(10000000);
	}

static void KernelFault(TExcType aType, TUint32* aEntrySp)
	{
	static TInt inFault;
	if (inFault++)
		{
		// (a fault while reporting one: the stack dump, last, read too far)
		if (inFault == 2)
			Fail("The Linux runner failed (an exception in its kernel thread); PsiLinux.log has the detail");
		WaitToBeEnded();
		}
	TUint32 r[16];
	Mem::FillZ(r, sizeof r);
	TUint32* s = aEntrySp + 2;
	TBool found = EFalse;
	for (TInt i = 0; i < 24 && !found; i++)
		{
		TUint32 w = s[i];
		if (w < 0x50000000 || w >= 0x51000000 || (w & 3))
			continue;
		if (((TUint32*)w)[0] != 0xE8BD5FF8 || ((TUint32*)w)[1] != 0xE8BD8007)
			continue;
		TUint32* f = s + i + 1;              // r3..r12, lr, r0, r1, r2, pc
		r[0] = f[11]; r[1] = f[12]; r[2] = f[13];
		for (TInt k = 3; k <= 12; k++)
			r[k] = f[k - 3];
		r[13] = (TUint32)(f + 15);
		r[14] = f[10];
		r[15] = f[14];
		found = ETrue;
		}
	if (!gLogOpen && gSh)
		OpenLog(gSh->root);                  // (even without Tools > Keep a log)
	TUint32 code = (TUint32)&E32Main;
	TUint32 insn = 0, addr = 0;
	// (the instruction, if pc is in the runner's code or the ROM)
	if (found && !(r[15] & 3) && ((r[15] >= (code & 0xFFF00000u) && r[15] < code + 0x100000) || (r[15] >= 0x50000000 && r[15] < 0x51000000)))
		{
		insn = *(TUint32*)r[15];
		addr = AccessAddress(r, insn);
		}
	bk_log("the kernel thread faulted: exception %d at pc %x (instruction %08x, address %x), lr %x, sp %x; E32Main is at %x (psilxrun.map)",
		aType, r[15], insn, addr, r[14], r[13], code);
	if (found)
		{
		bk_log("  r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
		bk_log("  r8 %08x r9 %08x r10 %08x r11 %08x r12 %08x", r[8], r[9], r[10], r[11], r[12]);
		}
	else
		bk_log("  (no EUSER frame found near %x)", (TUint32)aEntrySp);
	static const char* const what[] = { "nothing", "a system call", "the timer", "a thread's death" };
	TInt wi = (lx_k_now_what >= 0 && lx_k_now_what <= 3) ? lx_k_now_what : 0;
	lx_thread* ct = lx_cpu.cur;
	bk_log("  doing: %s (tid %d); translated thread holding the CPU: %d (critical %d); %u blocks, %u flushes",
		what[wi], lx_k_now_tid, ct ? ct->tid : 0, lx_cpu.critical, lx_cpu.translations, lx_cpu.flushes);
	bk_log("  the kernel's stack: %u of %u bytes used at most", StackUsed(), KStackSize);
	for (TUint n = 0; n < LX_TRAIL && n < lx_k_trail_n; n++)
		{
		volatile lx_u32* tr = lx_k_trail[(lx_k_trail_n - 1 - n) % LX_TRAIL];
		bk_log("  call -%u: tid %u pid %u: %u(%x)", n, tr[0], tr[1], tr[2], tr[3]);
		}
	if (found && !(r[13] & 3))
		{
		// the stack above the fault: the return addresses of its callers are
		// among these (last: it may run past the stack's top)
		TUint32* sp = (TUint32*)r[13];
		for (TInt q = 0; q < 32; q += 8)
			bk_log("  sp+%d: %08x %08x %08x %08x %08x %08x %08x %08x", q * 4, sp[q], sp[q + 1], sp[q + 2], sp[q + 3], sp[q + 4], sp[q + 5], sp[q + 6], sp[q + 7]);
		}
	char why[150];
	lx_snprintf(why, sizeof why, "The Linux runner failed: exception %d at %x (lr %x) in %s; PsiLinux.log has the detail",
		aType, r[15], r[14], what[wi]);
	inFault = 3;                             // (from here a fault is not told twice)
	Fail(why);
	WaitToBeEnded();                         // (the app sees the state and ends the runner)
	}

extern "C" void LxExcHandlerC(TExcType aType, TUint32* aEntrySp)
	{
	if (gKernelIdSet && RThread().Id() == gKernelId)
		KernelFault(aType, aEntrySp);        // (never returns)
	lx_thread* t = CurrentDbt();
	gFaultWhy = 0;
	if (t && aType == EExcAlignment && !lx_cpu.critical && FixAlignment(aEntrySp))
		return;
	if (t && aType == EExcAlignment)
		t->fault_addr = gFaultPc;
	else
		{
		// (0.28) any other fault: where it was (EUSER's frame, as for an
		// alignment fault), for the log line "signal N at pc"
		TUint32* s = aEntrySp + 2;
		lx_thread* ft = t ? t : lx_thread_by_sp(CurrentSp());
		for (TInt i = 0; ft && i < 24; i++)
			{
			TUint32 w = s[i];
			if (w < 0x50000000 || w >= 0x51000000 || (w & 3))
				continue;
			if (((TUint32*)w)[0] != 0xE8BD5FF8 || ((TUint32*)w)[1] != 0xE8BD8007)
				continue;
			ft->fault_addr = s[i + 1 + 14];
			break;
			}
		}
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
	gThreads++;
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
	gThreads--;
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

// ---- (0.39) sleep: the programs' memory to the disk and back ----------------
// Every chunk that holds Linux's memory (images, stacks, the mmap arena, the
// translator's cache) is in gSl. Asleep, each one's committed pages are in a
// file of its own (Sleep\<base>.mem) and the chunk is decommitted, but kept
// open: its base, which the programs' pointers hold, stays reserved. Pages
// unchanged since the file was last written (a hash of each 4 KB page, kept
// here) are not written again, for the card's sake: the programs' code is
// written once, at the first sleep.
struct TSlChunk
	{
	RChunk* iC;
	TBool iDouble;              // a stack: a double-ended chunk
	TBool iAsleep;
	TInt iBottom, iTop;         // committed, as it went to sleep (offsets in the chunk)
	TInt iFileBottom;           // the file holds the chunk from this offset (-1: no file)
	TInt iValid;                // pages at the file's start whose hashes are known
	TInt iHashCap;
	TUint32* iHash;             // two words a page
	};
const TInt KSlMax = 128;
static TSlChunk gSl[KSlMax];
static TFileName gSlDir;        // where the files are (empty: none written yet)
static TBool gSlSuspended;

static void SlName(TDes& aName, const TSlChunk& aS)
	{
	aName = gSlDir;
	aName.AppendNumFixedWidth((TUint)aS.iC->Base(), EHex, 8);
	aName.Append(_L(".mem"));
	}

static void SlAdd(RChunk* aC, TBool aDouble)
	{
	for (TInt i = 0; i < KSlMax; i++)
		if (!gSl[i].iC)
			{
			Mem::FillZ(&gSl[i], sizeof gSl[i]);
			gSl[i].iC = aC;
			gSl[i].iDouble = aDouble;
			gSl[i].iFileBottom = -1;
			return;
			}
	bk_log("sleep: more than %d chunks: Linux will not sleep", KSlMax);
	}

static TBool gSlFull;
static void SlRemove(RChunk* aC)
	{
	for (TInt i = 0; i < KSlMax; i++)
		if (gSl[i].iC == aC)
			{
			if (gSl[i].iFileBottom >= 0 && gSlDir.Length())
				{
				TFileName n;
				SlName(n, gSl[i]);
				gFs.Delete(n);
				}
			User::Free(gSl[i].iHash);
			Mem::FillZ(&gSl[i], sizeof gSl[i]);
			return;
			}
	}

static void PageHash(const TUint32* p, TUint32& a, TUint32& b)
	{
	TUint32 x = 0x811C9DC5u, y = 0x9E3779B9u;
	for (TInt i = 0; i < 1024; i++)
		{
		TUint32 w = p[i];
		x = (x ^ w) * 0x01000193u;
		y = ((y << 7) | (y >> 25)) + w + (TUint32)i;
		}
	a = x;
	b = y;
	}

// where the files go: the memory disk (D:), never the internal disk, whose
// files are in RAM themselves (a <root>\SLEEPONC file allows C:, for the
// emulator, which has no memory disk)
static TBool SlDir(TDes& aDir)
	{
	TDriveInfo di;
	TBool c = EFalse;
	if (gFs.Drive(di, EDriveD) == KErrNone && di.iType != EMediaNotPresent && di.iType != EMediaRam
		&& !(di.iMediaAtt & KMediaAttWriteProtected))
		aDir = _L("D:\\System\\Apps\\PsiLinux\\Sleep\\");
	else
		{
		TFileName t;
		Name(t, gSh->root);
		t.Append(_L("SLEEPONC"));
		TEntry e;
		if (gFs.Entry(t, e) != KErrNone)
			return EFalse;
		aDir = _L("C:\\System\\Apps\\PsiLinux\\Sleep\\");
		c = ETrue;
		}
	(void)c;
	TInt r = gFs.MkDirAll(aDir);
	return r == KErrNone || r == KErrAlreadyExists;
	}

static void SlThreads(TBool aSuspend)
	{
	if (aSuspend == gSlSuspended)
		return;
	for (TInt i = 0; i < LX_MAXTHREAD; i++)
		{
		lx_thread* t = &lx_k.threads[i];
		if (t->state != LX_T_FREE && t->bk)
			{
			if (aSuspend)
				((THThread*)t->bk)->iThread.Suspend();
			else
				((THThread*)t->bk)->iThread.Resume();
			}
		}
	gSlSuspended = aSuspend;
	}

// one chunk's committed pages to its file: the pages that changed
static TInt SlWrite(TSlChunk& aS, lx_u32& aWritten)
	{
	TInt bottom = aS.iDouble ? aS.iC->Bottom() : 0;
	TInt top = aS.iDouble ? aS.iC->Top() : aS.iC->Size();
	TInt pages = (top - bottom) / 4096;
	aS.iBottom = bottom;
	aS.iTop = top;
	TFileName n;
	SlName(n, aS);
	if (aS.iFileBottom != bottom)
		aS.iValid = 0;
	if (pages > aS.iHashCap)
		{
		TUint32* h = (TUint32*)User::ReAlloc(aS.iHash, pages * 8);
		if (!h)
			return KErrNoMemory;
		aS.iHash = h;
		aS.iHashCap = pages;
		}
	if (aS.iValid > pages)
		aS.iValid = pages;
	RFile f;
	TInt r = aS.iValid ? f.Open(gFs, n, EFileWrite) : f.Replace(gFs, n, EFileWrite);
	if (r != KErrNone && aS.iValid)
		{
		aS.iValid = 0;
		r = f.Replace(gFs, n, EFileWrite);
		}
	if (r != KErrNone)
		return r;
	aS.iFileBottom = -1;                     // (until it is all written)
	const TUint8* base = aS.iC->Base() + bottom;
	TInt run = -1;
	for (TInt i = 0; i <= pages && r == KErrNone; i++)
		{
		TBool changed = EFalse;
		if (i < pages)
			{
			TUint32 a, b;
			PageHash((const TUint32*)(base + i * 4096), a, b);
			changed = i >= aS.iValid || aS.iHash[2 * i] != a || aS.iHash[2 * i + 1] != b;
			aS.iHash[2 * i] = a;
			aS.iHash[2 * i + 1] = b;
			}
		if (changed && run < 0)
			run = i;
		if ((!changed || i - run >= 8) && run >= 0)
			{
			// (32 KB at most at a time: the card's driver)
			r = f.Write(run * 4096, TPtrC8(base + run * 4096, (i - run) * 4096));
			aWritten += (i - run) * 4096;
			run = changed ? i : -1;
			}
		}
	if (r == KErrNone)
		r = f.SetSize(top - bottom);
	f.Close();
	if (r != KErrNone)
		{
		aS.iValid = 0;
		return r;
		}
	aS.iValid = pages;
	aS.iFileBottom = bottom;
	return KErrNone;
	}

extern "C" int bk_sleep_save(lx_u32* written, lx_u32* saved)
	{
	TFileName dir;
	if (!SlDir(dir))
		return -LX_ENOSPC;                   // (no memory disk: nothing to gain)
	if (dir.CompareF(gSlDir) != 0)
		{
		// another disk than last time: every file is written afresh
		gSlDir = dir;
		for (TInt i = 0; i < KSlMax; i++)
			if (gSl[i].iC)
				{
				gSl[i].iValid = 0;
				gSl[i].iFileBottom = -1;
				}
		}
	*written = 0;
	*saved = 0;
	// the files first, with every chunk still in place: a write that fails
	// (the disk full) leaves Linux as it was
	SlThreads(ETrue);
	for (TInt i = 0; i < KSlMax; i++)
		if (gSl[i].iC)
			{
			TInt r = SlWrite(gSl[i], *written);
			if (r != KErrNone)
				{
				bk_log("sleep: %S: %d", &gSlDir, r);
				SlThreads(EFalse);
				return r == KErrDiskFull ? -LX_ENOSPC : -LX_EIO;
				}
			*saved += gSl[i].iTop - gSl[i].iBottom;
			}
	// then the RAM goes back
	for (TInt i = 0; i < KSlMax; i++)
		if (gSl[i].iC)
			{
			TSlChunk& c = gSl[i];
			TInt r = c.iDouble ? c.iC->AdjustDoubleEnded(c.iBottom, c.iBottom) : c.iC->Adjust(0);
			c.iAsleep = r == KErrNone;
			}
	return 0;
	}

extern "C" int bk_sleep_restore(void)
	{
	TInt i;
	// the RAM first, all of it, or none: another program may have it now
	for (i = 0; i < KSlMax; i++)
		if (gSl[i].iC && gSl[i].iAsleep)
			{
			TSlChunk& c = gSl[i];
			TInt r = c.iDouble ? c.iC->AdjustDoubleEnded(c.iBottom, c.iTop) : c.iC->Adjust(c.iTop);
			if (r != KErrNone)
				{
				for (TInt j = 0; j < i; j++)
					if (gSl[j].iC && gSl[j].iAsleep)
						{
						TSlChunk& d = gSl[j];
						if (d.iDouble)
							d.iC->AdjustDoubleEnded(d.iBottom, d.iBottom);
						else
							d.iC->Adjust(0);
						}
				return -LX_ENOMEM;
				}
			}
	// then the contents
	for (i = 0; i < KSlMax; i++)
		if (gSl[i].iC && gSl[i].iAsleep)
			{
			TSlChunk& c = gSl[i];
			TFileName n;
			SlName(n, c);
			RFile f;
			TInt r = f.Open(gFs, n, EFileRead);
			TInt len = c.iTop - c.iBottom;
			for (TInt at = 0; r == KErrNone && at < len; at += 32768)
				{
				TInt piece = len - at > 32768 ? 32768 : len - at;
				TPtr8 p(c.iC->Base() + c.iBottom + at, 0, piece);
				r = f.Read(at, p, piece);
				if (r == KErrNone && p.Length() != piece)
					r = KErrEof;
				}
			if (r != KErrNone)
				{
				bk_log("wake: %S: %d: Linux's memory is lost", &n, r);
				return -LX_EIO;
				}
			f.Close();
			c.iAsleep = EFalse;
			}
	SlThreads(EFalse);
	return 0;
	}

// the files of the last session, and of this one as the runner ends
static void SlClean()
	{
	static const TText* const KDirs[] = { _S("D:\\System\\Apps\\PsiLinux\\Sleep\\"), _S("C:\\System\\Apps\\PsiLinux\\Sleep\\") };
	for (TInt d = 0; d < 2; d++)
		{
		CFileMan* fm = 0;
		TRAPD(e, fm = CFileMan::NewL(gFs));
		if (fm)
			{
			TFileName w;
			w = TPtrC(KDirs[d]);
			w.Append(_L("*.mem"));
			fm->Delete(w);
			delete fm;
			}
		(void)e;
		}
	}

extern "C" lx_s64 bk_next_timer(void);

// (0.38) what the regions hold, by kind, for /proc/psilinux/mem
static lx_u32 gRegBytes[4];
static TInt gRegCount[4];
static void RegCount(TInt aKind, lx_u32 aSize, TInt aSign)
	{
	if (aKind < 0 || aKind > 3)
		aKind = 0;
	gRegBytes[aKind] += aSign * (TInt)aSize;
	gRegCount[aKind] += aSign;
	}

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
	r->kind = kind;
	RegCount(kind, size, 1);
	SlAdd(c, kind == LX_REG_STACK);
	return 0;
	}

extern "C" void bk_region_free(lx_region* r)
	{
	RChunk* c = (RChunk*)r->bk;
	if (c)
		{
		if (r->kind)
			RegCount(r->kind, r->size, -1);
		SlRemove(c);
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
		{
		gArenaOk = ETrue;
		SlAdd(&gArena, EFalse);
		}
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
	r->kind = 0;                             // (not counted with the regions: lx_dbt.c says what it has)
	SlAdd(c, EFalse);
	return 0;
	}
extern "C" int bk_code_grow(lx_region* r, lx_u32 size) { return Err(((RChunk*)r->bk)->Adjust(size)); }

// (0.28) The netBook's SA-1100 has separate instruction and data caches:
// code just written must be cleaned out of the data cache, and the
// instruction cache emptied, before it runs (lxsync.h). LxSync1.ldd does it;
// it is only loaded on a netBook (its kernel's export numbers are not the
// 5mx's), and the 5mx's one cache needs nothing.
static RLxSync gSync;
static TBool gSyncOpen;
static TBool IsNetBook()
	{
	TMachineInfoV1Buf mi;
	if (UserHal::MachineInfo(mi) != KErrNone)
		return EFalse;
	return mi().iMachineName.FindF(_L("netBook")) >= 0;
	}
static void SyncOpen()
	{
	if (!IsNetBook())
		return;
	lx_k.no_bx = 1;
	TInt err = User::LoadLogicalDevice(KLxSyncFile);
	if (err == KErrNone || err == KErrAlreadyExists)
		err = gSync.Open();
	if (err == KErrNone)
		{
		gSyncOpen = ETrue;
		err = gSync.Check();
		}
	if (err == KErrNone)
		bk_log("netBook: the cache driver LxSync1.ldd is ready");
	else
		bk_log("netBook: the cache driver LxSync1.ldd did not start (%d): Linux programs will not run", err);
	}
extern "C" void bk_code_sync(lx_u32 start, lx_u32 len)
	{
	if (gSyncOpen && len)
		gSync.Sync(start, len);
	}
extern "C" int bk_arena_grow(lx_u32 size) { return gArenaOk ? Err(gArena.Adjust((size + 4095) & ~4095u)) : -LX_ENOMEM; }
// (0.27) User::Alloc panics the caller (USER 47) for a size of KMaxTInt/2
// (1 GB) or more, and here the caller is the kernel thread: every Linux
// program would go with it. Such a size can only be a bad calculation, so it
// is refused like any other allocation the Psion cannot make (the kernel
// answers ENOMEM), and logged with the caller's address (PsiLinux.log, with
// Tools > Keep a log ticked) so that the calculation can be found.
extern "C" void* bk_alloc(lx_u32 n)
	{
	if (RThread().Id() == gKernelId)
		StackSample();
	if (n >= (lx_u32)(KMaxTInt / 2))
		{
		bk_log("bk_alloc: %u bytes refused (from %x)", n, (lx_u32)__builtin_return_address(0));
		return 0;
		}
	return AllocZ(n ? n : 1);
	}
extern "C" void bk_free(void* p) { User::Free(p); }
extern "C" void bk_memcpy(void* d, const void* s, lx_u32 n) { Mem::Copy(d, s, n); }
extern "C" void bk_memmove(void* d, const void* s, lx_u32 n) { Mem::Copy(d, s, n); }
extern "C" void bk_memset(void* d, int c, lx_u32 n) { Mem::Fill(d, n, TChar(c & 0xff)); }

// Preferences: "Keep memory for other programs" (the app sets it, and may
// change it while Linux runs): lx_reserve_ok checks each growth against it
extern "C" lx_u32 bk_mem_reserve(void) { return gSh ? gSh->reserve : 0; }
// (0.37) Tools > Show welcome banner (the app may change it while Linux runs)
extern "C" int bk_welcome(void) { return gSh ? !gSh->welcome_off : 1; }

extern "C" void bk_mem_where(lx_bkmem* m)
	{
	TInt used = 0;
	m->heap_size = User::Heap().Size();
	User::Heap().AllocSize(used);
	m->heap_used = used;
	m->image = gRegBytes[LX_REG_IMAGE];
	m->nimage = gRegCount[LX_REG_IMAGE];
	m->stack = gRegBytes[LX_REG_STACK];
	m->nstack = gRegCount[LX_REG_STACK];
	m->threads = gThreads;
	m->app_heap = gSh ? gSh->app_heap : 0;
	m->app_heap_used = gSh ? gSh->app_heap_used : 0;
	m->start_free = gSh ? gSh->start_free : 0;
	m->behind = gSh ? gSh->app_behind : 0;
	m->asked = gSh ? gSh->trim_seq : 0;
	}
// (0.38) EPOC's heap never shrinks by itself: after a burst (apk, a big
// pipe, a program's tables) its free end goes back to the Psion
extern "C" void bk_heap_compress(void) { User::Heap().Compress(); }

extern "C" void bk_mem_info(lx_u32* total, lx_u32* freeb)
	{
	TMemoryInfoV1Buf mi;
	UserHal::MemoryInfo(mi);
	*total = mi().iTotalRamInBytes;
	*freeb = mi().iFreeRamInBytes;
	}

// The machine's own name for /proc/cpuinfo (0.33: it said "Psion Series 5mx"
// everywhere); 1 when it has the netBook's and Series 7's StrongARM SA-1100
extern "C" int bk_machine(char* name, int size)
	{
	TMachineInfoV1Buf mi;
	name[0] = 0;
	if (UserHal::MachineInfo(mi) != KErrNone || size < 2)
		return 0;
	const TDesC& n = mi().iMachineName;
	TInt len = n.Length() < size - 1 ? n.Length() : size - 1;
	for (TInt i = 0; i < len; i++)
		name[i] = (char)n[i];
	name[len] = 0;
	return n.FindF(_L("netBook")) >= 0 || n.FindF(_L("Series 7")) >= 0;
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

// (0.29) the system tick's real length: 1/64 s on the 5mx, but taken from
// UserHal rather than assumed, as other machines (the netBook) may differ;
// every time in the kernel (sleeps, timeouts, the translator's slice) is
// counted in these
static TInt gTickUs = 15625;
static void TickInit()
	{
	TTimeIntervalMicroSeconds32 period;
	if (UserHal::TickPeriod(period) == KErrNone && period.Int() > 0 && period.Int() <= 1000000)
		gTickUs = period.Int();
	}
extern "C" lx_s64 bk_now_mono(void)
	{
	return (lx_s64)User::TickCount() * gTickUs;
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
	void RunL()
		{
		iAt = 0;
		if (lx_k_asleep && lx_k_wake("a timer") < 0)
			{
			iAt = bk_now_mono() + 1000000;   // (0.39) no room to wake yet: again in a second
			After(1000000);
			return;
			}
		lx_k_timer();
		}
	};
static CLxTimer* gTimer;
extern "C" lx_s64 bk_next_timer(void) { return gTimer && gTimer->IsActive() ? gTimer->iAt : 0; }

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

// (0.29) Lines from a Linux thread (the translator's runtime runs on them)
// wait here for the kernel thread: the log file belongs to its file server
// session, which EPOC lets no other thread use. One writer at a time (the
// thread holding the CPU), and one reader (the kernel thread).
const TInt KPendLog = 2048;
static char gPend[KPendLog];
static volatile TUint gPendW, gPendR;
static void LogWrite(const char* aBuf, TInt aLen);
static void LogPending()
	{
	TUint w = gPendW;
	while (gPendR != w)
		{
		char line[300];
		TInt n = 0;
		while (gPendR != w && n < (TInt)sizeof line)
			{
			char c = gPend[gPendR % KPendLog];
			line[n++] = c;
			gPendR++;
			if (c == '\n')
				break;
			}
		LogWrite(line, n);
		}
	}

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
	if (gKernelIdSet && RThread().Id() != gKernelId)
		{
		if (gLogOpen && KPendLog - (gPendW - gPendR) > (TUint)n)
			{
			for (TInt i = 0; i < n; i++)
				gPend[(gPendW + i) % KPendLog] = buf[i];
			gPendW += n;                     // (after the line is in place)
			}
		return;
		}
	StackSample();
	LogPending();
	LogWrite(buf, n);
	}

static void LogWrite(const char* buf, TInt n)
	{
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

// (0.29) how long Linux takes to answer a key: from the key reaching the
// kernel to the next output on the terminal (the echo, or whatever the
// program wrote), for the status line in the log every 30 s
static lx_s64 gKeyAt;
static TUint gEchoN, gEchoMaxUs;
static lx_s64 gEchoSumUs;
static TUint gEchoSlow;                  // over 100 ms

extern "C" int bk_tty_write(lx_tty* tty, const char* buf, int n)
	{
	if (tty->index != 0 || !gSh)
		return n;
	if (gKeyAt && n > 0)
		{
		lx_s64 d = bk_now_mono() - gKeyAt;
		gKeyAt = 0;
		if (d >= 0 && d < 60000000)
			{
			gEchoN++;
			gEchoSumUs += d;
			if ((TUint)d > gEchoMaxUs)
				gEchoMaxUs = (TUint)d;
			if (d > 100000)
				gEchoSlow++;
			}
		}
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
static CPeriodic* gStopTick;

// (0.36) the session is over, and the programs left have ended (or been
// ended): only now does the app hear of it, and close
static void StopNow()
	{
	if (gStopping)
		return;
	gStopping = ETrue;
	CActiveScheduler::Stop();
	}

static TInt StopTick(TAny*)
	{
	if (!lx_k.stopping)
		lx_k_stop_begin();               // (here, not inside the exit that ended the session)
	if (lx_k_stop_done())
		StopNow();
	return 1;
	}

extern "C" void bk_tty_status(lx_tty* tty, int running, int status)
	{
	if (!gSh || tty->index != 0 || running)
		return;
	gSh->exit_status = status;
	bk_log("the session has ended: %s%s%s", gEndWhy ? gEndWhy : "", gEndWhy ? "; " : "", lx_k.end_note[0] ? lx_k.end_note : "(the session was ended)");
	EndNote(lx_k.end_note[0] ? lx_k.end_note : "the session was ended");
	// (0.36) the first program has ended ("exit" at the top shell): the
	// others are asked to end, and given a moment to close their files,
	// before the runner stops (RunL then closes every file and the RFs)
	if (!gStopTick)
		gStopTick = CPeriodic::New(CActive::EPriorityStandard);
	if (gStopTick && !gStopTick->IsActive())
		gStopTick->Start(1000, 100000, TCallBack(StopTick, 0));
	else if (!gStopTick)
		StopNow();
	}

// ---- (0.36) EPOC requests (lx_epoc.c): to the app through the shared chunk -----

static TUint gEpocAns;           // the last answer the kernel was told of

extern "C" int bk_epoc_ready(void) { return gSh && gSh->epoc_app ? 1 : 0; }

extern "C" int bk_epoc_send(const char* req, int len)
	{
	if (!gSh || !gSh->epoc_app)
		return -LX_ENODEV;
	if (len > LX_EPOC_BUF)
		return -LX_E2BIG;
	Mem::Copy(gSh->epoc_req, req, len);
	gSh->epoc_req_len = len;
	gSh->epoc_req_seq++;
	gAppRinger.Ring(&gSh->app_bell);
	return 0;
	}

extern "C" int bk_epoc_answer(char* buf, int max)
	{
	if (!gSh || gSh->epoc_ans_seq != gSh->epoc_req_seq || gEpocAns == gSh->epoc_ans_seq)
		return 0;
	TInt n = gSh->epoc_ans_len;
	if (n < (TInt)sizeof(LxeHeader) || n > max || n > LX_EPOC_BUF)
		{
		gEpocAns = gSh->epoc_ans_seq;
		LxeHeader h;
		Mem::Copy(&h, gSh->epoc_req, sizeof h);
		h.status = LXE_FAILED;
		h.len = 0;
		Mem::Copy(buf, &h, sizeof h);
		return sizeof h;
		}
	Mem::Copy(buf, gSh->epoc_ans, n);
	gEpocAns = gSh->epoc_ans_seq;
	return n;
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
		// (0.39) asleep: anything for Linux, or PsiLinux in front, wakes it
		// first; with no room to wake yet, nothing is taken from the app
		// (the tick looks again each second)
		if (lx_k_asleep && (gSh->quit || !gSh->app_behind || gSh->resize_seq != iResize || gSh->kbd_head != gSh->kbd_tail
			|| OutFree() != iFree || (gSh->epoc_ans_seq == gSh->epoc_req_seq && gSh->epoc_ans_seq != gEpocAns)))
			if (!Wake("the app"))
				{
				if (gSh->quit)
					{
					gEndWhy = "the app ended Linux while it was asleep, with no memory to wake it";
					Quit();
					}
				else if (gSh->bell_magic == PSI_BELL_MAGIC)
					iWaiter->Arm(&gSh->eng_bell);
				return;
				}
		for (TInt round = 0; round < 8; round++)
			{
			if (gSh->quit)
				{
				gEndWhy = "the app ended Linux (End Linux, or the app closed)";
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
				if (!gKeyAt)
					gKeyAt = bk_now_mono();      // (0.29) the echo's time inside Linux starts
				lx_k_boost(1);               // (0.29) whoever reads them gets the CPU first
				lx_k_tty_input(&lx_k.ttys[0], buf, n);
				lx_k_boost(0);
				}
			if (OutFree() != iFree)
				{
				iFree = OutFree();
				lx_k_tty_writable(&lx_k.ttys[0]);
				}
			FbInput();
			if (gSh->epoc_ans_seq == gSh->epoc_req_seq && gSh->epoc_ans_seq != gEpocAns)
				lx_k_epoc_answered();        // (0.36) the app has answered an EPOC request
			if (gSh->bell_magic != PSI_BELL_MAGIC || !iWaiter->Arm(&gSh->eng_bell))
				return;                      // armed (or no bells: the tick looks)
			}
		}
	static void FbInput();
	static TBool Wake(const char* aWhy);
	static TInt Rung(TAny* aSelf) { ((CLxTerm*)aSelf)->Look(); return 0; }
	static TInt Tick(TAny* aSelf) { ((CLxTerm*)aSelf)->Housekeeping(); return 1; }
	void Housekeeping();
	void Quit()
		{
		if (gStopping)
			return;
		gStopping = ETrue;
		if (lx_k_asleep && lx_k_wake("the end") < 0)
			{
			// (0.39) no room to wake: the programs end with the runner, and
			// EPOC closes their files; nothing of theirs is touched
			CActiveScheduler::Stop();
			return;
			}
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
// ---- the Linux screen (lx_fb.c): a global chunk the app draws from --------

static RChunk gFbChunk;
static LxFbHeader* gFb;

extern "C" void* bk_fb_map()
	{
	if (!gFb)
		{
		// (the app may still have the last one open: then it is that one again)
		TInt err = gFbChunk.CreateGlobal(_L(LX_FB_NAME), LX_FB_SIZE, LX_FB_SIZE);
		if (err == KErrAlreadyExists)
			err = gFbChunk.OpenGlobal(_L(LX_FB_NAME), EFalse);
		if (err != KErrNone)
			{
			bk_log("screen: no memory for it (%d)", err);
			return 0;
			}
		gFb = (LxFbHeader*)gFbChunk.Base();
		Mem::FillZ(gFb, LX_FB_HDR);
		gFb->magic = LX_FB_MAGIC;
		}
	gSh->fb_on = 1;
	gSh->fb_seq++;
	gAppRinger.Ring(&gSh->app_bell);
	return gFb;
	}

// No program has the screen now: the app lets go of it too, and its memory
// goes back to EPOC.
extern "C" void bk_fb_release()
	{
	if (!gFb)
		return;
	gFb = 0;
	gFbChunk.Close();
	gSh->fb_on = 0;
	gSh->fb_seq++;
	gAppRinger.Ring(&gSh->app_bell);
	}

extern "C" void bk_fb_notify()
	{
	gAppRinger.Ring(&gSh->app_bell);
	}

// The app's keys and pen, for /dev/input
void CLxTerm::FbInput()
	{
	if (!gFb)
		return;
	TUint head = gFb->in_head;
	while (gFb->in_tail != head)
		{
		const LxInEvent& e = gFb->in[gFb->in_tail & (LX_FB_IN - 1)];
		lx_k_input(e.dev, e.type, e.code, e.value);
		gFb->in_tail++;
		}
	}

// (0.38) Memory back to the Psion when Linux is idle: after a minute with no
// system call, key or output (10 s while PsiLinux is behind another
// program), once until Linux is busy again; and at once when the app asks
// (the launcher's "trim" and "sleep"). lx_k_trim says what goes.
const TInt KIdleTrimFront = 60;
const TInt KIdleTrimBehind = 10;
static TUint gIdleMark;
static TInt gIdleSecs;
static TBool gIdleTrimmed;
static TUint gTrimSeq;
// (0.39) Linux wakes: the app shows "Waking Linux..." meanwhile; with no
// room for its memory yet it stays asleep (LXS_NOROOM) and the app says so;
// memory that cannot be read back ends Linux
TBool CLxTerm::Wake(const char* aWhy)
	{
	TInt r = lx_k_wake(aWhy);
	if (r == 0)
		{
		if (gSh->sleep_state != LXS_AWAKE)
			{
			gSh->sleep_state = LXS_AWAKE;
			gAppRinger.Ring(&gSh->app_bell);
			}
		return ETrue;
		}
	if (r == -LX_ENOMEM)
		{
		if (gSh->sleep_state != LXS_NOROOM)
			{
			gSh->sleep_state = LXS_NOROOM;
			gAppRinger.Ring(&gSh->app_bell);
			}
		return EFalse;
		}
	Fail("Linux's memory could not be read back from the disk (was the card taken out?)");
	User::Exit(KErrCorrupt);
	return EFalse;
	}

// (0.39) Linux sleeps when PsiLinux has been behind for sleep_after seconds
// (Preferences: In the background) with Linux idle for 10 s, or at once
// when the launcher asks ("sleep") and Linux is idle
const TInt KSleepIdle = 10;
static TInt gBehindSecs;
static TUint gSleepReq;
static void AutoSleep(TInt aIdleSecs)
	{
	TBool asked = gSh->sleep_req != gSleepReq;
	gSleepReq = gSh->sleep_req;
	gBehindSecs = gSh->app_behind ? gBehindSecs + 1 : 0;
	if (lx_k_asleep || !gSh->sleep_after || !gSh->app_behind)
		return;
	if (!asked && (gBehindSecs < gSh->sleep_after || aIdleSecs < KSleepIdle))
		return;
	if (gSh->out_head != gSh->out_tail || gSh->kbd_head != gSh->kbd_tail || gSh->epoc_req_seq != gSh->epoc_ans_seq)
		return;
	if (lx_k_sleep(asked ? "the launcher asked" : "behind and idle") < 0)
		return;
	gSh->sleep_freed_kb = lx_k_sleep_freed_kb();
	gSh->sleep_state = LXS_ASLEEP;
	gSh->sleep_seq++;
	gAppRinger.Ring(&gSh->app_bell);
	}

static void IdleTrim()
	{
	if (lx_k_asleep)
		return;
	TUint mark = lx_k_trail_n + gSh->kbd_head + gSh->out_head + lx_cpu.translations;
	if (gSh->trim_seq != gTrimSeq)
		{
		gTrimSeq = gSh->trim_seq;
		lx_k_trim(gSh->app_behind ? "asked, behind" : "asked");
		gIdleMark = mark;
		gIdleSecs = 0;
		gIdleTrimmed = ETrue;
		return;
		}
	if (mark != gIdleMark)
		{
		gIdleMark = mark;
		gIdleSecs = 0;
		gIdleTrimmed = EFalse;
		return;
		}
	if (!gIdleTrimmed && ++gIdleSecs >= (gSh->app_behind ? KIdleTrimBehind : KIdleTrimFront))
		{
		lx_k_trim("idle");
		gIdleTrimmed = ETrue;
		}
	}

static TInt gKFaultAt;           // (testing: <root>\KFAULT makes the kernel thread fault at 20 s)
void CLxTerm::Housekeeping()
	{
	TInt i;
	LogPending();
	if (gKFaultAt && (bk_now_mono() - lx_k.start_mono) / 1000000 >= gKFaultAt)
		{
		static TUint32 words[2];
		gKFaultAt = 0;
		if (!gLogOpen)
			OpenLog(gSh->root);
		bk_log("KFAULT: an unaligned load in the kernel thread, as a test");
		volatile TUint32* p = (volatile TUint32*)((TUint8*)words + 1);
		words[0] = *p;
		bk_log("KFAULT: no alignment fault (%08x); a store to address 4", words[0]);
		*(volatile TUint32*)4 = 0;
		bk_log("KFAULT: no fault at all");
		}
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
		gEndWhy = "the app's heartbeat stopped for 45 s (the app crashed, was killed, or did not run its tick)";
		bk_log("%s", gEndWhy);
		Quit();                              // the app has gone (crashed, or killed)
		return;
		}
	IdleTrim();
	AutoSleep(gIdleSecs);
	if (lx_k_asleep && !gSh->app_behind)
		Wake("in front");
	TInt n = 0;
	TUint used = 0;
	TInt secs = (TInt)((bk_now_mono() - lx_k.start_mono) / 1000000);
	if (secs >= gStatusNext)
		{
		// every 30 s in Alpine mode: memory, and how the translator is doing
		lx_u32 tot, fr;
		bk_mem_info(&tot, &fr);
		gStatusNext = secs + 30;
		if (lx_k.execlog)
			bk_log("%d s: %u KB free; translated %u blocks (%u KB of code), %u flushes; %u syscalls, %u helpers; unaligned: %u faults, %u sites, %u via sites",
				secs, fr / 1024, lx_cpu.translations, lx_cpu.host_words / 256, lx_cpu.flushes, lx_cpu.syscalls, lx_cpu.helpers,
				lx_cpu.fixups, lx_cpu.patched, lx_cpu.fixed_calls);
		if (lx_k.execlog)
			bk_log("  the kernel's stack: %u of %u bytes used at most", StackUsed(), KStackSize);
		if (gEchoN)
			{
			bk_log("  keys: %u answered in %u ms on average, %u ms at most, %u over 100 ms",
				gEchoN, (TUint)(gEchoSumUs / gEchoN / 1000), gEchoMaxUs / 1000, gEchoSlow);
			gEchoN = 0;
			gEchoSumUs = 0;
			gEchoMaxUs = 0;
			gEchoSlow = 0;
			}
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
	StackSample();
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

// (0.36) EPOC's attributes, as the kernel has them
static int Attr(TUint aAtt)
	{
	return ((aAtt & KEntryAttReadOnly) ? LX_ATT_READONLY : 0) | ((aAtt & KEntryAttHidden) ? LX_ATT_HIDDEN : 0)
		| ((aAtt & KEntryAttSystem) ? LX_ATT_SYSTEM : 0) | ((aAtt & KEntryAttArchive) ? LX_ATT_ARCHIVE : 0);
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
	st->attr = Attr(e.iAtt);
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
	TInt r = gFs.Rename(a, b);
	if (r == KErrAlreadyExists && a.CompareF(b) == 0 && a.Compare(b) != 0 && b.Length() + 3 <= b.MaxLength())
		{
		// (0.36) only the case changes ("mv letter Letter"): by way of
		// another name, in case the file server takes it for a clash
		TFileName t(b);
		t.Append(_L("~lx"));
		r = gFs.Rename(a, t);
		if (r == KErrNone && (r = gFs.Rename(t, b)) != KErrNone)
			gFs.Rename(t, a);
		}
	return Err(r);
	}

// (0.36) a file renamed over another: RFs::Replace does it in one call, and
// leaves the old file as it was if it cannot (either of them open, the old
// one read-only)
extern "C" int bk_replace(const char* from, const char* to)
	{
	TFileName a, b;
	Name(a, from);
	Name(b, to);
	return Err(gFs.Replace(a, b));
	}

extern "C" int bk_same_file(const char* a, const char* b)
	{
	TFileName x, y;
	Name(x, a);
	Name(y, b);
	Trim(x);
	Trim(y);
	return x.CompareF(y) == 0;
	}

extern "C" int bk_set_readonly(const char* path, int on)
	{
	TFileName name;
	Name(name, path);
	Trim(name);
	return Err(on ? gFs.SetAtt(name, KEntryAttReadOnly, 0) : gFs.SetAtt(name, 0, KEntryAttReadOnly));
	}

// the runner's folder (PsiLinux's: /psion/bin is its bin\ folder)
extern "C" void bk_self_folder(char* out, int max)
	{
	TParse p;
	TFileName me(RProcess().FileName());
	p.Set(me, NULL, NULL);
	TPtrC dp(p.DriveAndPath());
	TPtr8 o((TUint8*)out, 0, max - 1);
	o.Copy(dp.Left(Min(dp.Length(), max - 1)));
	out[o.Length()] = 0;
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
	st->attr = Attr(e.iAtt);
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
	EndNote(aWhy);
	gSh->state = LXS_FAILED;
	gAppRinger.Ring(&gSh->app_bell);
	}

static void RunL()
	{
	CActiveScheduler* as = new(ELeave) CActiveScheduler;
	CleanupStack::PushL(as);
	CActiveScheduler::Install(as);
	User::LeaveIfError(gKernel.Duplicate(RThread(), EOwnerProcess));
	gKernelId = RThread().Id();
	gKernelIdSet = ETrue;
	// (0.29) its own faults are logged (KernelFault)
	RThread().SetExceptionHandler(lx_exc_entry, KExceptionAbort | KExceptionFault | KExceptionInteger);
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
	if (!gSh->log_on)
		{
		// (0.29) <root>\LOGON: a log as if Keep a log were ticked (testing:
		// the emulator's PsiLinux.ini is out of reach on C:)
		TFileName t;
		Name(t, gSh->root);
		t.Append(_L("LOGON"));
		TEntry e;
		if (gFs.Entry(t, e) == KErrNone)
			gSh->log_on = 1;
		}
	if (gSh->log_on)
		OpenLog(gSh->root);                  // (0.24: only with Tools > Keep a log ticked)
	TickInit();
	bk_log("PsiLinux runner %s: root %s, program %s (tick %d us)", LX_VERSION, gSh->root, gSh->program, gTickUs);
	gTimer = new(ELeave) CLxTimer;
	gTimer->ConstructL();
	lx_k_init(gSh->root);
	SlClean();                               // (0.39) a sleep's files the last session left
	SyncOpen();
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
		Name(t, gSh->root);
		t.Append(_L("KFAULT"));
		if (gFs.Entry(t, e) == KErrNone)
			gKFaultAt = 20;
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
	delete gStopTick;
	gStopTick = 0;
	SlClean();
		bk_log("PsiLinux runner: stopped (the kernel's stack: %u of %u bytes used at most)", StackUsed(), KStackSize);
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
	gStackTop = gStackMin = CurrentSp();    // (0.29: see StackSample)
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
