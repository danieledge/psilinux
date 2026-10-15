// lxprobe.cpp - PsiLinux M0 feasibility probe for the Psion Series 5mx.
// A test program only (never released). It answers the questions in
// linux/docs/DESIGN.md before the runner is written:
//   - can code in an RChunk be executed, and does a store to code need an
//     instruction-cache flush (the ARM710T cache is unified)?
//   - which ARMv4T instructions run in user mode (LDRH, BX, UMULL, SWP), and
//     what does EPOC do with "svc 0" and with a CP15 TLS read?
//   - can a thread catch its own faults (SetExceptionHandler) and carry on
//     with a longjmp out of the handler?
//   - can one thread interrupt another that is busy (RaiseException), and can
//     the handler switch to another stack and back (green threads)?
//   - how big can a thread's stack and a local chunk be, where do chunks
//     sit, and how much RAM is really free?
//   - can code run on a stack inside a chunk and still call EPOC?
// Every risky probe runs in its own thread, with a time limit, so a probe
// that dies is reported as its exit type, category and reason.
// The log is D:\LxProbe.log (or C:\ without a card); each line is flushed.
#include <e32base.h>
#include <f32file.h>
#include <e32hal.h>
#include <e32svr.h>

const TInt KLineLen=96;

// ----- assembler helpers ------------------------------------------------
// psj_set/psj_jump: setjmp and longjmp (r4-r11, sp, lr).
// call_on_stack(fn,arg,top): calls fn(arg) with sp=top, then puts sp back.
// green_switch(save,load): saves r4-r11,sp,lr in save, loads them from load.
asm(
"	.text\n"
"	.align 2\n"
"	.global psj_set\n"
"psj_set:\n"
"	stmia r0, {r4-r11, sp, lr}\n"
"	mov r0, #0\n"
"	bx lr\n"
"	.global psj_jump\n"
"psj_jump:\n"
"	ldmia r0, {r4-r11, sp, lr}\n"
"	movs r0, r1\n"
"	moveq r0, #1\n"
"	bx lr\n"
"	.global call_on_stack\n"
"call_on_stack:\n"
"	stmfd sp!, {r4, lr}\n"
"	mov r4, sp\n"
"	mov sp, r2\n"
"	mov ip, r0\n"
"	mov r0, r1\n"
"	mov lr, pc\n"
"	bx ip\n"
"	mov sp, r4\n"
"	ldmfd sp!, {r4, lr}\n"
"	bx lr\n"
"	.global green_switch\n"
"green_switch:\n"
"	stmia r0, {r4-r11, sp, lr}\n"
"	ldmia r1, {r4-r11, sp, lr}\n"
"	bx lr\n"
"	.global read_sp\n"
"read_sp:\n"
"	mov r0, sp\n"
"	bx lr\n"
);
extern "C" int psj_set(TUint32* aBuf);
extern "C" void psj_jump(TUint32* aBuf, int aVal);
extern "C" int call_on_stack(TInt (*aFn)(TAny*), TAny* aArg, TAny* aTop);
extern "C" void green_switch(TUint32* aSave, TUint32* aLoad);
extern "C" TUint32 read_sp();

typedef TInt (*TCodeFn)(TInt,TInt);
_LIT(KRight,"right");
_LIT(KWrong,"WRONG");
_LIT(KOk,"ok");
_LIT(KFail,"FAIL");
static const TDesC* RightOrWrong(TBool aRight) { return aRight ? &KRight() : &KWrong(); }

// ----- the log ----------------------------------------------------------
static RFs* gFs;
static RFile gLog;
static TBool gLogOpen;
static void Say(const TDesC& aText);

static void Say(const TDesC& aText)
	{
	if (gLogOpen)
		{
		TBuf8<KLineLen+2> b;
		b.Copy(aText.Left(Min(aText.Length(),KLineLen)));
		b.Append(_L8("\r\n"));
		gLog.Write(b);
		gLog.Flush();
		}
	}

static void Sayf(TRefByValue<const TDesC> aFmt,...)
	{
	VA_LIST l;
	VA_START(l,aFmt);
	TBuf<256> b;
	b.FormatList(aFmt,l);
	Say(b);
	}

// ----- running a probe in its own thread --------------------------------
static TInt gThreadNo;

// Runs aFn(aArg) in a new thread with the stack size given, waits up to
// aMs, and says how it ended. Returns the thread's exit reason, or
// KErrTimedOut (the thread is killed).
static TInt RunThread(const TDesC& aName, TThreadFunction aFn, TAny* aArg, TInt aStack, TInt aMs, TExitType* aType=NULL)
	{
	RThread t;
	TBuf<32> n;
	n.Format(_L("LxP%d"),++gThreadNo);
	TInt r=t.Create(n,aFn,aStack,KMinHeapSize,0x40000,aArg);
	if (r!=KErrNone)
		{
		Sayf(_L("  %S: thread create (stack %d) failed %d"),&aName,aStack,r);
		return r;
		}
	TRequestStatus st, tst;
	RTimer tm;
	tm.CreateLocal();
	t.Logon(st);
	tm.After(tst,aMs*1000);
	t.Resume();
	User::WaitForRequest(st,tst);
	TInt res;
	if (st==KRequestPending)
		{
		t.Kill(KErrTimedOut);
		User::WaitForRequest(st);
		Sayf(_L("  %S: timed out after %d ms (killed)"),&aName,aMs);
		res=KErrTimedOut;
		if (aType) *aType=EExitKill;
		}
	else
		{
		tm.Cancel();
		User::WaitForRequest(tst);
		TExitCategoryName cat=t.ExitCategory();
		TInt et=t.ExitType();
		res=t.ExitReason();
		if (aType) *aType=(TExitType)et;
		if (et!=EExitKill || res!=KErrNone)
			Sayf(_L("  %S: thread ended type %d (0 kill, 1 term, 2 panic) %S %d"),&aName,et,&cat,res);
		}
	tm.Close();
	t.Close();
	return res;
	}

// ----- 1. code in a chunk ----------------------------------------------
// ARM words for the instruction probes. Each is a function (r0,r1) -> r0.
static const TUint32 KRetConst[]={0xE3A0002A,0xE12FFF1E};          // mov r0,#42; bx lr
static const TUint32 KLdrh[]={0xE1D000B0,0xE12FFF1E};              // ldrh r0,[r0]; bx lr
static const TUint32 KLdrsh[]={0xE1D000F0,0xE12FFF1E};             // ldrsh r0,[r0]; bx lr
static const TUint32 KStrh[]={0xE1C010B0,0xE3A00000,0xE12FFF1E};    // strh r1,[r0]; mov r0,#0; bx lr
static const TUint32 KUmull[]={0xE0832190,0xE1A00003,0xE12FFF1E};   // umull r2,r3,r0,r1; mov r0,r3; bx lr
static const TUint32 KSmull[]={0xE0C32190,0xE1A00003,0xE12FFF1E};   // smull r2,r3,r0,r1; mov r0,r3; bx lr
static const TUint32 KSwp[]={0xE1002091,0xE1A00002,0xE12FFF1E};     // swp r2,r1,[r0]; mov r0,r2; bx lr
static const TUint32 KMrcTls[]={0xEE1D0F70,0xE12FFF1E};            // mrc p15,0,r0,c13,c0,3; bx lr
// mov r7,#20 (getpid); stmfd sp!,{r4-r11,lr}; svc 0; ldmfd ...; bx lr
static const TUint32 KSvc0[]={0xE92D4FF0,0xE3A07014,0xE3A00000,0xE3A01000,0xE3A02000,0xE3A03000,0xEF000000,0xE8BD4FF0,0xE12FFF1E};
// the same with the OABI number swi 0x900014
static const TUint32 KSvcOabi[]={0xE92D4FF0,0xE3A00000,0xE3A01000,0xE3A02000,0xE3A03000,0xEF900014,0xE8BD4FF0,0xE12FFF1E};

static RChunk gCode;

static TCodeFn Put(TInt aOffset, const TUint32* aWords, TInt aCount)
	{
	TUint32* p=(TUint32*)(gCode.Base()+aOffset);
	for (TInt i=0; i<aCount; i++)
		p[i]=aWords[i];
	return (TCodeFn)p;
	}

// guarded call: runs one code word sequence in a thread whose exception
// handler jumps back, so an undefined instruction is reported, not fatal
struct TGuard { TCodeFn iFn; TInt iA, iB; TInt iResult; TInt iExc; TUint32 iJmp[10]; };
static TGuard* gGuard;     // (one guarded call at a time)

static void GuardHandler(TExcType aType)
	{
	gGuard->iExc=aType;
	psj_jump(gGuard->iJmp,1);
	}

static TInt GuardThread(TAny* aArg)
	{
	TGuard* g=(TGuard*)aArg;
	gGuard=g;
	g->iExc=-1;
	RThread().SetExceptionHandler(GuardHandler,0xffffffff);
	if (psj_set(g->iJmp)==0)
		g->iResult=g->iFn(g->iA,g->iB);
	return KErrNone;
	}

static void Guarded(const TDesC& aName, TCodeFn aFn, TInt aA, TInt aB, TInt aExpect, TBool aShowHex=EFalse)
	{
	TGuard g;
	g.iFn=aFn; g.iA=aA; g.iB=aB; g.iResult=0x7777; g.iExc=-2;
	TExitType et=EExitPending;
	TInt r=RunThread(aName,GuardThread,&g,0x2000,3000,&et);
	if (r==KErrNone && et==EExitKill)
		{
		if (g.iExc>=0)
			Sayf(_L("%S: exception %d (6 invalid opcode, 9 access, 10 priv)"),&aName,g.iExc);
		else if (aShowHex)
			Sayf(_L("%S: ran, r0=%08x"),&aName,g.iResult);
		else
			Sayf(_L("%S: ran, r0=%d (%S)"),&aName,g.iResult,RightOrWrong(g.iResult==aExpect));
		}
	}

static void ProbeCode()
	{
	Say(_L("== 1. Code in a chunk; ARMv4T instructions"));
	TInt r=gCode.CreateLocal(0x1000,0x10000);
	if (r!=KErrNone)
		{
		Sayf(_L("CreateLocal: %d"),r);
		return;
		}
	Sayf(_L("Code chunk at %08x"),gCode.Base());
	TCodeFn f=Put(0,KRetConst,2);
	TInt v=f(0,0);
	Sayf(_L("Run from chunk: r0=%d (%S)"),v,RightOrWrong(v==42));
	// rewrite the same code and run at once: the unified cache should need
	// no flush. 100 times, alternating constants.
	TInt wrong=0;
	for (TInt i=0; i<100; i++)
		{
		TUint32* p=(TUint32*)gCode.Base();
		p[0]=0xE3A00000|(i&0xff);       // mov r0,#i
		if (f(0,0)!=(i&0xff))
			wrong++;
		}
	Sayf(_L("Rewrite then run, no flush: %d of 100 stale"),wrong);
	static TUint16 half[2]={0x1234,0x8765};
	Guarded(_L("LDRH"),Put(0x100,KLdrh,2),(TInt)&half[0],0,0x1234);
	Guarded(_L("LDRSH"),Put(0x140,KLdrsh,2),(TInt)&half[1],0,(TInt)(TInt16)0x8765);
	Guarded(_L("STRH"),Put(0x180,KStrh,3),(TInt)&half[0],0x4321,0);
	Sayf(_L("  after STRH: %04x (4321 right)"),half[0]);
	Guarded(_L("UMULL"),Put(0x200,KUmull,3),0x80000000,6,3);
	Guarded(_L("SMULL"),Put(0x240,KSmull,3),-2,0x40000000,-1);
	static TUint32 word=5;
	Guarded(_L("SWP"),Put(0x280,KSwp,3),(TInt)&word,9,5);
	Sayf(_L("  after SWP: %d (9 right)"),word);
	Guarded(_L("MRC p15 TLS"),Put(0x2c0,KMrcTls,2),0,0,0,ETrue);
	}

static void ProbeSvc()
	{
	Say(_L("== 9. svc 0 (Linux EABI getpid) and swi 0x900014 (OABI)"));
	Guarded(_L("svc 0"),Put(0x400,KSvc0,9),0,0,0,ETrue);
	Guarded(_L("swi 0x900014"),Put(0x480,KSvcOabi,8),0,0,0,ETrue);
	}

// ----- 2. memory ------------------------------------------------------
static void ProbeMemory()
	{
	Say(_L("== 2. Memory"));
	TMemoryInfoV1Buf mi;
	UserHal::MemoryInfo(mi);
	Sayf(_L("RAM %d KB, free %d KB (max free %d KB), C: %d KB"),mi().iTotalRamInBytes/1024,
		mi().iFreeRamInBytes/1024,mi().iMaxFreeRamInBytes/1024,mi().iInternalDiskRamInBytes/1024);
	static const TInt KMax[]={1,4,16,32,64,128,256,512,1024};
	for (TUint i=0; i<sizeof(KMax)/sizeof(KMax[0]); i++)
		{
		RChunk c;
		TInt r=c.CreateLocal(0x1000,KMax[i]<<20);
		Sayf(_L("CreateLocal max %d MB: %d at %08x"),KMax[i],r,r==KErrNone ? c.Base() : 0);
		c.Close();
		}
	// two big chunks at once: is the address space per chunk or shared?
	RChunk big1, big2, big3;
	TInt b1=big1.CreateLocal(0x1000,256<<20), b2=big2.CreateLocal(0x1000,256<<20), b3=big3.CreateLocal(0x1000,256<<20);
	Sayf(_L("Three 256 MB chunks: %d %d %d at %08x %08x %08x"),b1,b2,b3,b1 ? 0 : big1.Base(),b2 ? 0 : big2.Base(),b3 ? 0 : big3.Base());
	big1.Close(); big2.Close(); big3.Close();
	// how much can one chunk really get?
	RChunk c;
	TInt r=c.CreateLocal(0,32<<20);
	if (r==KErrNone)
		{
		TInt size=0;
		while (size<(32<<20) && c.Adjust(size+0x10000)==KErrNone)
			size+=0x10000;
		Sayf(_L("One chunk grew to %d KB (in 64 KB steps)"),size/1024);
		// touch every page, and check
		TUint8* b=c.Base();
		for (TInt i=0; i<size; i+=4096)
			b[i]=(TUint8)i;
		TInt bad=0;
		for (TInt j=0; j<size; j+=4096)
			if (b[j]!=(TUint8)j)
				bad++;
		Sayf(_L("  touched every page: %d bad"),bad);
		c.Close();
		}
	else
		Sayf(_L("32 MB chunk: %d"),r);
	// how many chunks of 1 MB maximum can we have at once?
	RChunk cs[64];
	TInt n=0;
	for (n=0; n<64; n++)
		if (cs[n].CreateLocal(0x1000,0x100000)!=KErrNone)
			break;
	Sayf(_L("Chunks (1 MB max each) at once: %d; first %08x last %08x"),n,n ? cs[0].Base() : 0,n ? cs[n-1].Base() : 0);
	for (TInt k=0; k<n; k++)
		cs[k].Close();
	// a global chunk: where?
	RChunk g;
	r=g.CreateGlobal(_L("LxProbeG"),0x1000,0x100000);
	Sayf(_L("Global chunk: %d at %08x; our heap cell at %08x; stack at %08x"),r,r==KErrNone ? g.Base() : 0,User::Alloc(4),read_sp());
	g.Close();
	}

// ----- 3. stacks ------------------------------------------------------
static TInt StackThread(TAny* aArg)
	{
	TInt size=(TInt)aArg;
	// write near the bottom of the stack, 1 KB above its end
	volatile TUint8* sp=(volatile TUint8*)read_sp();
	volatile TUint8* low=sp-size+2048;
	low[0]=0x5a;
	return low[0]==0x5a ? KErrNone : KErrGeneral;
	}

static void ProbeStacks()
	{
	Say(_L("== 3. Thread stacks"));
	static const TInt KSizes[]={0x2000,0x10000,0x40000,0x100000,0x400000,0x800000};
	TBuf<200> l(_L("Stack KB: "));
	for (TUint i=0; i<sizeof(KSizes)/sizeof(KSizes[0]); i++)
		{
		Sayf(_L("  trying a %d KB stack"),KSizes[i]/1024);
		TExitType et=EExitPending;
		TInt r=RunThread(_L("stack"),StackThread,(TAny*)KSizes[i],KSizes[i],3000,&et);
		l.AppendFormat(_L("%d:%S "),KSizes[i]/1024,(r==KErrNone && et==EExitKill) ? &KOk() : &KFail());
		}
	Say(l);
	}

// ----- 4. EPOC calls on a stack in a chunk ----------------------------
static TInt OnChunkStack(TAny* aArg)
	{
	TInt* out=(TInt*)aArg;
	TUint32 sp=read_sp();
	RFs fs;
	TInt r=fs.Connect();
	if (r==KErrNone)
		{
		RFile f;
		r=f.Replace(fs,_L("C:\\LxProbe.tmp"),EFileWrite);
		if (r==KErrNone)
			{
			r=f.Write(_L8("hello from a chunk stack\n"));
			f.Close();
			}
		fs.Delete(_L("C:\\LxProbe.tmp"));
		fs.Close();
		}
	User::After(20000);
	TAny* cell=User::Alloc(1000);
	User::Free(cell);
	out[0]=r;
	out[1]=sp;
	return 77;
	}

static TInt ChunkStackThread(TAny* aArg)
	{
	RChunk c;
	TInt r=c.CreateLocal(0x10000,0x10000);
	if (r!=KErrNone)
		return r;
	TInt v=call_on_stack(OnChunkStack,aArg,c.Base()+0x10000);
	c.Close();
	return v==77 ? KErrNone : KErrGeneral;
	}

static void ProbeChunkStack()
	{
	Say(_L("== 4. EPOC calls with sp in a chunk"));
	TInt out[2]={-99,0};
	TInt r=RunThread(_L("chunk stack"),ChunkStackThread,out,0x2000,5000);
	Sayf(_L("File write, After, Alloc with sp=%08x: thread %d, file %d"),out[1],r,out[0]);
	}

// ----- 5. faults caught by the thread itself --------------------------
struct TFault { TUint32 iJmp[10]; TInt iCount; TInt iType[4]; TUint32 iHandlerSp[4]; TUint32 iDump[48]; TUint32 iFaultSp; };
static TFault* gFault;

static void FaultHandler(TExcType aType)
	{
	TFault* f=gFault;
	TInt n=f->iCount;
	if (n<4)
		{
		f->iType[n]=aType;
		f->iHandlerSp[n]=read_sp();
		if (n==0)
			{
			TUint32* s=(TUint32*)read_sp();
			for (TInt i=0; i<48; i++)
				f->iDump[i]=s[i];
			}
		}
	f->iCount++;
	psj_jump(f->iJmp,1);
	}

static TInt FaultThread(TAny* aArg)
	{
	TFault* f=(TFault*)aArg;
	gFault=f;
	RThread().SetExceptionHandler(FaultHandler,KExceptionFault|KExceptionAbort|KExceptionInteger);
	for (TInt i=0; i<3; i++)
		{
		if (psj_set(f->iJmp)==0)
			{
			f->iFaultSp=read_sp();
			// an unmapped address in the user area, a write to ROM, a
			// misaligned word read
			if (i==0)
				{ volatile TInt* p=(volatile TInt*)0x3f000000; TInt x=*p; (void)x; }
			else if (i==1)
				{ volatile TInt* p=(volatile TInt*)0x50000100; *p=1; }
			else
				{ volatile TInt* p=(volatile TInt*)0x7fff0000; *p=1; }
			}
		}
	// still alive: a normal call works
	RFs fs;
	TInt r=fs.Connect();
	fs.Close();
	return r;
	}

static void ProbeFaults()
	{
	Say(_L("== 5. Faults caught with SetExceptionHandler (read 3f000000, write ROM 50000100, write 7fff0000)"));
	TFault f;
	Mem::FillZ(&f,sizeof(f));
	TInt r=RunThread(_L("faults"),FaultThread,&f,0x4000,4000);
	Sayf(_L("3 null reads: handler ran %d times, types %d %d %d, thread %d"),f.iCount,f.iType[0],f.iType[1],f.iType[2],r);
	Sayf(_L("  sp at fault %08x, in handler %08x (%d bytes below)"),f.iFaultSp,f.iHandlerSp[0],f.iFaultSp-f.iHandlerSp[0]);
	for (TInt i=0; i<48; i+=8)
		Sayf(_L("  +%02x: %08x %08x %08x %08x %08x %08x %08x %08x"),i*4,f.iDump[i],f.iDump[i+1],f.iDump[i+2],f.iDump[i+3],
			f.iDump[i+4],f.iDump[i+5],f.iDump[i+6],f.iDump[i+7]);
	}

// ----- 5b. a fault reported, then the thread killed (no "Program closed")
struct TFault2 { volatile TInt iFaulted; TInt iType; RSemaphore iNever; };
static TFault2* gFault2;

static void ReportHandler(TExcType aType)
	{
	gFault2->iType=aType;
	gFault2->iFaulted=1;
	gFault2->iNever.Wait();         // the core kills us
	}

static TInt ReportThread(TAny* aArg)
	{
	gFault2=(TFault2*)aArg;
	RThread().SetExceptionHandler(ReportHandler,KExceptionFault|KExceptionAbort|KExceptionInteger);
	volatile TInt* p=(volatile TInt*)0x3f000000;
	*p=1;
	return KErrNone;
	}

static void ProbeFaultKill()
	{
	Say(_L("== 5b. Fault reported by the handler, then the thread killed"));
	TFault2 f;
	f.iFaulted=0;
	f.iNever.CreateLocal(0);
	RThread t;
	TInt r=t.Create(_L("LxFaultKill"),ReportThread,0x2000,KMinHeapSize,0x10000,&f);
	if (r!=KErrNone)
		return;
	TRequestStatus st;
	t.Logon(st);
	t.Resume();
	for (TInt i=0; i<50 && !f.iFaulted; i++)
		User::After(20000);
	if (f.iFaulted)
		t.Kill(139);
	User::WaitForRequest(st);
	TExitCategoryName cat=t.ExitCategory();
	Sayf(_L("Faulted %d (type %d); killed: exit %d %S %d"),f.iFaulted,f.iType,t.ExitType(),&cat,t.ExitReason());
	t.Close();
	f.iNever.Close();
	}

// ----- 6. interrupting a busy thread ----------------------------------
// A worker spins; another thread raises EExcUserInterrupt on it. The
// handler counts, and every third time switches to a second stack (a green
// thread) that adds to its own counter and switches back.
struct TBusy
	{
	volatile TInt iSpins;
	volatile TInt iInterrupts;
	volatile TInt iGreenRuns;
	volatile TInt iStop;
	TUint32 iMain[10], iGreen[10];
	TUint32 iPcSeen;
	TUint32 iHandlerSp, iLoopSp;
	TUint32 iDump[32];
	TInt iStarted;
	};
static TBusy* gBusy;

static void GreenBody()
	{
	for (;;)
		{
		gBusy->iGreenRuns++;
		green_switch(gBusy->iGreen,gBusy->iMain);
		}
	}

static void BusyHandler(TExcType aType)
	{
	TBusy* b=gBusy;
	if (aType!=EExcUserInterrupt)
		return;
	b->iInterrupts++;
	if (b->iInterrupts==1)
		{
		b->iHandlerSp=read_sp();
		TUint32* s=(TUint32*)read_sp();
		for (TInt i=0; i<32; i++)
			b->iDump[i]=s[i];
		}
	if (b->iInterrupts%3==0)
		green_switch(b->iMain,b->iGreen);
	}

static TInt BusyThread(TAny* aArg)
	{
	TBusy* b=(TBusy*)aArg;
	gBusy=b;
	// the green thread's first switch starts GreenBody on its own stack
	static TUint32 greenStack[1024];
	Mem::FillZ(b->iGreen,sizeof(b->iGreen));
	b->iGreen[8]=(TUint32)&greenStack[1024];   // sp
	b->iGreen[9]=(TUint32)GreenBody;           // lr
	RThread().SetExceptionHandler(BusyHandler,KExceptionUserInterrupt);
	b->iLoopSp=read_sp();
	b->iStarted=1;
	while (!b->iStop)
		b->iSpins++;
	return KErrNone;
	}

static void ProbeInterrupt()
	{
	Say(_L("== 6. RaiseException on a busy thread"));
	TBusy b;
	Mem::FillZ(&b,sizeof(b));
	RThread t;
	TInt r=t.Create(_L("LxBusy"),BusyThread,0x4000,KMinHeapSize,0x10000,&b);
	if (r!=KErrNone)
		{
		Sayf(_L("create %d"),r);
		return;
		}
	TRequestStatus st;
	t.Logon(st);
	t.SetPriority(EPriorityLess);
	t.Resume();
	User::After(50000);
	TInt raised=0, errs=0;
	Sayf(_L("  busy thread started %d, spins %d"),b.iStarted,b.iSpins);
	for (TInt i=0; i<30; i++)
		{
		if (i<4)
			Sayf(_L("  raise %d (handler ran %d, green %d)"),i,b.iInterrupts,b.iGreenRuns);
		if (t.RaiseException(EExcUserInterrupt)==KErrNone)
			raised++;
		else
			errs++;
		User::After(20000);
		}
	b.iStop=1;
	RTimer tm;
	tm.CreateLocal();
	TRequestStatus tst;
	tm.After(tst,3000000);
	User::WaitForRequest(st,tst);
	if (st==KRequestPending)
		{
		t.Kill(0);
		User::WaitForRequest(st);
		Say(_L("  busy thread did not stop: killed"));
		}
	else
		{
		tm.Cancel();
		User::WaitForRequest(tst);
		}
	tm.Close();
	TExitCategoryName cat=t.ExitCategory();
	Sayf(_L("Raised %d (%d failed): handler ran %d, green runs %d, spins %d; exit %d %S %d"),
		raised,errs,b.iInterrupts,b.iGreenRuns,b.iSpins,t.ExitType(),&cat,t.ExitReason());
	Sayf(_L("  loop sp %08x, handler sp %08x (%d below)"),b.iLoopSp,b.iHandlerSp,b.iLoopSp-b.iHandlerSp);
	for (TInt i=0; i<32; i+=8)
		Sayf(_L("  +%02x: %08x %08x %08x %08x %08x %08x %08x %08x"),i*4,b.iDump[i],b.iDump[i+1],b.iDump[i+2],b.iDump[i+3],
			b.iDump[i+4],b.iDump[i+5],b.iDump[i+6],b.iDump[i+7]);
	t.Close();
	}

// ----- 7. speed -------------------------------------------------------
static void ProbeSpeed()
	{
	Say(_L("== 7. Speed (only real on a 5mx)"));
	TUint t0=User::TickCount();
	volatile TInt x=0;
	for (TInt i=0; i<2000000; i++)
		x+=i;
	TUint t1=User::TickCount();
	TAny* a=User::Alloc(65536);
	TAny* b=User::Alloc(65536);
	TUint t2=User::TickCount();
	if (a && b)
		for (TInt j=0; j<64; j++)
			Mem::Copy(a,b,65536);
	TUint t3=User::TickCount();
	User::Free(a);
	User::Free(b);
	Sayf(_L("2M loop: %d ticks; 4 MB copy: %d ticks (64 ticks a second)"),t1-t0,t3-t2);
	}

// ----- 8. a request round trip between two threads ---------------------
// The runner's design: a Linux thread fills in a request and completes a
// TRequestStatus of the kernel thread, then waits on its own; the kernel
// thread answers the same way. This times N round trips.
struct TRpc { RThread iMain, iClient; TRequestStatus* iMainSt; TRequestStatus iClientSt; volatile TInt iCount; TInt iN; };

static TInt RpcClient(TAny* aArg)
	{
	TRpc* r=(TRpc*)aArg;
	for (TInt i=0; i<r->iN; i++)
		{
		r->iClientSt=KRequestPending;
		r->iCount++;
		TRequestStatus* s=r->iMainSt;
		r->iMain.RequestComplete(s,i);
		User::WaitForRequest(r->iClientSt);
		}
	return KErrNone;
	}

static void ProbeRpc()
	{
	Say(_L("== 8. Request round trips between two threads"));
	TRpc r;
	r.iN=2000;
	r.iCount=0;
	r.iMain.Duplicate(RThread(),EOwnerProcess);
	TRequestStatus st;
	r.iMainSt=&st;
	st=KRequestPending;
	TInt e=r.iClient.Create(_L("LxRpc"),RpcClient,0x2000,KMinHeapSize,0x10000,&r);
	if (e!=KErrNone)
		{
		Sayf(_L("create %d"),e);
		return;
		}
	TRequestStatus dead;
	r.iClient.Logon(dead);
	TUint t0=User::TickCount();
	r.iClient.Resume();
	for (TInt i=0; i<r.iN; i++)
		{
		User::WaitForRequest(st);
		st=KRequestPending;
		r.iMainSt=&st;
		TRequestStatus* cs=&r.iClientSt;
		r.iClient.RequestComplete(cs,KErrNone);
		}
	User::WaitForRequest(dead);
	TUint t1=User::TickCount();
	Sayf(_L("%d round trips: %d ticks (64 a second); client exit %d"),r.iN,t1-t0,dead.Int());
	r.iClient.Close();
	r.iMain.Close();
	}

// ----- 10. alignment faults (Alpine mode, DBT probe) ----------------------
// Alpine's ARMv6 programs load words from unaligned addresses (ARMv6 allows
// it). The ARM710T with EPOC's alignment checking faults instead. Can a
// thread's own exception handler find the faulting instruction's saved
// registers, do the access itself, and carry on after it?
// al_probe(addr, kind, value): kind 0 ldr r5,[r0]; 1 ldrh r5,[r0];
// 2 str r2,[r0]; 3 strh r2,[r0]. Returns r5 (0x55555555 for a store).
// r4-r11 hold marks (0xA4A40004...) so the handler can find them.
asm(
"	.text\n"
"	.align 2\n"
"	.global al_probe\n"
"al_probe:\n"
"	stmfd sp!, {r4-r11, lr}\n"
"	ldr r4, =0xA4A40004\n"
"	ldr r5, =0x55555555\n"
"	ldr r6, =0xA6A60006\n"
"	ldr r7, =0xA7A70007\n"
"	ldr r8, =0xA8A80008\n"
"	ldr r9, =0xA9A90009\n"
"	ldr r10, =0xAAAA000A\n"
"	ldr r11, =0xABAB000B\n"
"	ldr r12, =0xACAC000C\n"
"	str r3, [sp, #-4]!\n"       // (out: value, flags after)
"	ldr r12, =0xACAC000C\n"
"	adr r3, al_table\n"
"	ldr r3, [r3, r1, lsl #2]\n"
"	.word 0xE328F206\n"          // msr cpsr_f, #0x60000000 (Z and C set)
"	mov pc, r3\n"
"al_table:\n"
"	.word al_pc_ldr, al_pc_ldrh, al_pc_str, al_pc_strh, al_pc_post, al_pc_pre\n"
"	.global al_pc_post\n"
"al_pc_post:\n"
"	ldr r5, [r0], #4\n"
"	b 1f\n"
"	.global al_pc_pre\n"
"al_pc_pre:\n"
"	ldr r5, [r0, #4]!\n"
"	b 1f\n"
"	.global al_pc_ldr\n"
"al_pc_ldr:\n"
"	ldr r5, [r0]\n"
"	b 1f\n"
"	.global al_pc_ldrh\n"
"al_pc_ldrh:\n"
"	.word 0xE1D050B0\n"   // ldrh r5,[r0] (as an ARMv3 assembler will not take it)

"	b 1f\n"
"	.global al_pc_str\n"
"al_pc_str:\n"
"	str r2, [r0]\n"
"	b 1f\n"
"	.global al_pc_strh\n"
"al_pc_strh:\n"
"	.word 0xE1C020B0\n"   // strh r2,[r0]

"1:	.word 0xE10F6000\n"         // mrs r6, cpsr
"	ldr r3, [sp], #4\n"
"	str r5, [r3]\n"
"	str r6, [r3, #4]\n"
"	mov r0, r5\n"
"	ldmfd sp!, {r4-r11, pc}\n"
"	.ltorg\n"
);
asm(
"	.text\n"
"	.align 2\n"
"	.global al_handler_asm\n"
"al_handler_asm:\n"
"	.word 0xE10F1000\n"          // mrs r1, cpsr (the flags the faulting code had?)
"	stmfd sp!, {r1, lr}\n"
"	bl AlignHandlerC\n"
"	ldmfd sp!, {r1, lr}\n"
"	.word 0xE128F001\n"          // msr cpsr_f, r1
"	mov pc, lr\n"
);
extern "C" void al_handler_asm(TExcType aType);
extern "C" TUint32 al_probe(TUint32 aAddr, TInt aKind, TUint32 aValue, TUint32* aOut);
extern "C" TUint32 al_pc_ldr[], al_pc_ldrh[], al_pc_str[], al_pc_strh[], al_pc_post[], al_pc_pre[];

struct TAlProbe
	{
	TUint32 iJmp[10];
	TInt iMode;                 // 0: dump and jump back; 1: fix up and return
	volatile TInt iCount;
	TInt iType;
	TUint32 iHandlerSp, iCallSp;
	TInt iWords;
	TUint32 iDump[160];
	TInt iPcIdx, iR4Idx;        // where the saved pc and r4 were found (-1: not)
	TUint32 iPcSaved;
	TInt iFixed, iUnfixable;
	TInt iRight, iWrong, iFlagsLost;
	TUint32 iLostWhich[4];
	TUint32 iEntryFlags;
	TUint32 iBaseSeen[2], iBaseGiven;
	TUint32 iFirstWrong[4];
	TInt iRounds;               // mode 2: faults taken; its ticks (1/64 s)
	TUint iTicks, iTicksPlain;
	};
static TAlProbe* gAlign;

static TUint32 AlPc(TInt aKind)
	{
	return aKind==0 ? (TUint32)al_pc_ldr : aKind==1 ? (TUint32)al_pc_ldrh : aKind==2 ? (TUint32)al_pc_str : aKind==3 ? (TUint32)al_pc_strh : aKind==4 ? (TUint32)al_pc_post : (TUint32)al_pc_pre;
	}

static TInt gAlKind;            // the access being probed

extern "C" void AlignHandlerC(TExcType aType, TUint32 aFlags)
	{
	TAlProbe* a=gAlign;
	if (a->iCount==0)
		a->iEntryFlags=aFlags;
	TUint32* s=(TUint32*)read_sp();
	TInt n=(TInt)(a->iCallSp-(TUint32)s)/4+16;   // (the probe's own frame, and a little above)
	if (n>160) n=160;
	if (n<0) n=0;
	a->iCount++;
	if (a->iMode==0)
		{
		a->iType=aType;
		a->iHandlerSp=(TUint32)s;
		a->iWords=n;
		for (TInt i=0; i<n; i++)
			a->iDump[i]=s[i];
		psj_jump(a->iJmp,1);
		}
	// mode 1: EUSER's trampoline left [r3..r12, lr, r0, r1, r2, pc] above
	// User::HandleException's frame, whose saved lr points at the
	// trampoline's "pop {r3-r12,lr}; pop {r0,r1,r2,pc}". Find that lr.
	TUint32 pc=AlPc(gAlKind);
	TInt ip=-1, i4=-1;
	for (TInt i=0; i+15<n; i++)
		{
		TUint32 w=s[i];
		if (w>=0x50000000 && w<0x51000000 && (w&3)==0 && ((TUint32*)w)[0]==0xE8BD5FF8 && ((TUint32*)w)[1]==0xE8BD8007)
			{
			i4=i+2;                 // r3 at i+1, r4 at i+2
			ip=i+1+14;              // pc
			break;
			}
		}
	if (ip<0 || s[ip]!=pc)
		{
		a->iUnfixable++;
		a->iPcIdx=ip;
		a->iPcSaved=ip>=0 ? s[ip] : 0;
		psj_jump(a->iJmp,2);
		}
	if (gAlKind>=4)
		{
		a->iBaseSeen[gAlKind-4]=s[i4+10];   // r0 as the handler sees it
		a->iUnfixable++;
		psj_jump(a->iJmp,3);
		}
	// r0 is at r4's slot + 10 (r4..r12, lr, r0), r2 at + 12
	TUint8* addr=(TUint8*)s[i4+10];
	if (gAlKind==0)
		s[i4+1]=addr[0]|(addr[1]<<8)|(addr[2]<<16)|(addr[3]<<24);
	else if (gAlKind==1)
		s[i4+1]=addr[0]|(addr[1]<<8);
	else
		{
		TUint32 v=s[i4+12];         // r2
		addr[0]=(TUint8)v;
		addr[1]=(TUint8)(v>>8);
		if (gAlKind==2)
			{
			addr[2]=(TUint8)(v>>16);
			addr[3]=(TUint8)(v>>24);
			}
		}
	a->iPcIdx=ip;
	a->iR4Idx=i4;
	a->iPcSaved=s[ip]-pc;
	s[ip]+=4;                       // (the trampoline returns to the next instruction)                       // carry on after the instruction
	a->iFixed++;
	}

static void AlignHandler0(TExcType aType) { AlignHandlerC(aType,0); }

static TInt AlignThread(TAny* aArg)
	{
	TAlProbe* a=(TAlProbe*)aArg;
	gAlign=a;
	TUint32 pad[64];                // (room above the probe's frame for the dump)
	for (TInt p=0; p<64; p++)
		pad[p]=p;
	RThread().SetExceptionHandler(a->iMode ? al_handler_asm : AlignHandler0,0xffffffff);
	static TUint8 buf[64];
	a->iCallSp=read_sp();
	if (a->iMode==2)
		{
		// the cost of an alignment fault, fixed up: 3000 of them (an Alpine
		// program's unaligned accesses each cost one until the site is
		// patched), and the same accesses done a byte at a time
		TUint32 out[2];
		TUint t0=User::TickCount();
		for (TInt i=0; i<3000; i++)
			{
			gAlKind=i&3;
			if (psj_set(a->iJmp)!=0)
				continue;
			al_probe((TUint32)(buf+1+(i&1)),i&3,0xC0DE0000+i,out);
			a->iRounds++;
			}
		TUint t1=User::TickCount();
		volatile TUint32 sum=0;
		for (TInt j=0; j<3000; j++)
			{
			const TUint8* q=buf+1+(j&1);
			sum+=q[0]|(q[1]<<8)|(q[2]<<16)|(q[3]<<24);
			}
		a->iTicks=t1-t0;
		a->iTicksPlain=User::TickCount()-t1;
		return pad[5];
		}
	if (a->iMode==0)
		{
		for (TInt i=0; i<16; i++)
			buf[i]=(TUint8)(0x10+i);
		gAlKind=0;
		if (psj_set(a->iJmp)==0)
			{
			TUint32 out[2];
			TUint32 v=al_probe((TUint32)(buf+1),0,0,out);
			a->iType=-1;                // no fault: the value says what the CPU did
			a->iDump[0]=v;
			}
		return pad[5];
		}
	for (TInt i=0; i<48; i++)
		{
		TInt kind=i%4, off=1+(i/4)%3;
		for (TInt j=0; j<16; j++)
			buf[j]=(TUint8)(0x20+j+i);
		gAlKind=kind;
		TUint32 val=0xC0DE0000+i;
		if (psj_set(a->iJmp)!=0)
			continue;                   // (the handler gave up)
		TUint32 out[2]={0,0};
		TUint32 r=al_probe((TUint32)(buf+off),kind,val,out);
		if ((out[1]&0xf0000000)!=0x60000000)
			{
			if (a->iFlagsLost<4)
				a->iLostWhich[a->iFlagsLost]=(kind<<28)|(off<<24)|(a->iCount<<8)|(out[1]>>28);
			a->iFlagsLost++;
			}
		TUint32 want;
		TBool ok;
		if (kind==0)
			ok=r==(TUint32)(buf[off]|(buf[off+1]<<8)|(buf[off+2]<<16)|(buf[off+3]<<24)), want=0;
		else if (kind==1)
			ok=r==(TUint32)(buf[off]|(buf[off+1]<<8)), want=0;
		else
			{
			ok=buf[off]==(TUint8)val && buf[off+1]==(TUint8)(val>>8) && (kind==3 || (buf[off+2]==(TUint8)(val>>16) && buf[off+3]==(TUint8)(val>>24)));
			ok=ok && r==0x55555555;
			want=val;
			}
		if (ok)
			a->iRight++;
		else
			{
			if (a->iWrong<4)
				a->iFirstWrong[a->iWrong]=(kind<<28)|(off<<24)|(r&0xffffff);
			a->iWrong++;
			}
		(void)want;
		}
	for (TInt k=4; k<6; k++)
		{
		gAlKind=k;
		a->iBaseGiven=(TUint32)(buf+1);
		TUint32 out[2];
		if (psj_set(a->iJmp)==0)
			al_probe((TUint32)(buf+1),k,0,out);
		}
	return pad[5];
	}

static void ProbeAlign()
	{
	Say(_L("== 10. Alignment faults (DBT): what the handler sees"));
	TAlProbe* a=new TAlProbe;
	if (!a)
		return;
	Mem::FillZ(a,sizeof(TAlProbe));
	a->iMode=0;
	TInt r=RunThread(_L("align dump"),AlignThread,a,0x4000,4000);
	if (a->iType==-1)
		Sayf(_L("Unaligned ldr did not fault: r5=%08x (bytes 11 12 13 14 at +1)"),a->iDump[0]);
	else
		{
		Sayf(_L("Fault type %d (EExcAlignment=%d), thread %d; probe pc %08x"),a->iType,EExcAlignment,r,(TUint32)al_pc_ldr);
		Sayf(_L("  handler sp %08x, call sp %08x, %d words"),a->iHandlerSp,a->iCallSp,a->iWords);
		for (TInt i=0; i<a->iWords; i+=8)
			Sayf(_L("  +%03x: %08x %08x %08x %08x %08x %08x %08x %08x"),i*4,a->iDump[i],a->iDump[i+1],a->iDump[i+2],a->iDump[i+3],
				a->iDump[i+4],a->iDump[i+5],a->iDump[i+6],a->iDump[i+7]);
		}
	Say(_L("== 10b. Alignment faults: fix up in the handler and carry on (48 accesses)"));
	Mem::FillZ(a,sizeof(TAlProbe));
	a->iMode=1;
	TExitType et=EExitPending;
	r=RunThread(_L("align fix"),AlignThread,a,0x4000,8000,&et);
	Sayf(_L("Handler ran %d, fixed %d, gave up %d; results right %d wrong %d; thread exit %d %d"),
		a->iCount,a->iFixed,a->iUnfixable,a->iRight,a->iWrong,et,r);
	Sayf(_L("  base register after a faulting writeback: given %08x; post-index #4 sees %08x, pre-index #4! sees %08x"),a->iBaseGiven,a->iBaseSeen[0],a->iBaseSeen[1]);
	Sayf(_L("  flags at the handler's entry (first fault): %08x (want 6xxxxxxx)"),a->iEntryFlags);
	Sayf(_L("  saved pc slot %d (fault pc +%d), r4 slot %d; flags lost %d (%08x %08x %08x); first wrong %08x %08x"),a->iPcIdx,a->iPcSaved,a->iR4Idx,a->iFlagsLost,a->iLostWhich[0],a->iLostWhich[1],a->iLostWhich[2],a->iFirstWrong[0],a->iFirstWrong[1]);
	delete a;
	}

// ----- 11. writing code and running it at once (a DBT's code cache) ------
// 4096 small functions in a 128 KB chunk: write all, run all; write new
// constants, run all; then write-one-run-one. Counts stale results. (The
// ARM710T cache is unified: no flush should be needed. The emulator runs
// its own JIT, so only a 5mx answers this for the hardware.)
static void ProbeCodeCache()
	{
	Say(_L("== 11. Code cache: write then run, 4096 functions, no flush"));
	RChunk c;
	TInt r=c.CreateLocal(0x20000,0x20000);
	if (r!=KErrNone)
		{
		Sayf(_L("chunk %d"),r);
		return;
		}
	TUint32* p=(TUint32*)c.Base();
	TInt stale[3]={0,0,0};
	TUint t0=User::TickCount();
	for (TInt round=0; round<3; round++)
		{
		for (TInt i=0; i<4096; i++)
			{
			TUint32* f=p+i*8;
			TUint32 v=(TUint32)(i*7+round*1000);
			f[0]=0xE3A00000|(v&0xff);                 // mov r0,#lo
			f[1]=0xE3800C00|((v>>8)&0xff);            // orr r0,r0,#hi<<8
			f[2]=0xE12FFF1E;                          // bx lr
			if (round==2)
				{
				TInt got=((TCodeFn)f)(0,0);
				if (got!=(TInt)(v&0xffff))
					stale[2]++;
				}
			}
		if (round<2)
			for (TInt j=0; j<4096; j++)
				{
				TUint32 v=(TUint32)(j*7+round*1000);
				if (((TCodeFn)(p+j*8))(0,0)!=(TInt)(v&0xffff))
					stale[round]++;
				}
		}
	TUint t1=User::TickCount();
	Sayf(_L("Stale: %d, %d (rewritten), %d (write-one-run-one) of 4096; %d ticks"),stale[0],stale[1],stale[2],t1-t0);
	c.Close();
	}

// ----- 12. a suspended thread's registers (RThread::Context) -------------
static TInt SpinThread(TAny* aArg)
	{
	volatile TInt* stop=(volatile TInt*)aArg;
	while (!*stop)
		;
	return KErrNone;
	}

static void ProbeContext()
	{
	Say(_L("== 12. RThread::Context of a suspended thread"));
	volatile TInt stop=0;
	RThread t;
	TInt r=t.Create(_L("LxSpin"),SpinThread,0x2000,KMinHeapSize,0x10000,(TAny*)&stop);
	if (r!=KErrNone)
		return;
	TRequestStatus st;
	t.Logon(st);
	t.SetPriority(EPriorityLess);
	t.Resume();
	User::After(50000);
	t.Suspend();
	TBuf8<128> ctx;
	ctx.SetLength(0);
	t.Context(ctx);
	Sayf(_L("Context: %d bytes; SpinThread at %08x"),ctx.Length(),(TUint32)SpinThread);
	const TUint32* w=(const TUint32*)ctx.Ptr();
	for (TInt i=0; i+8<=ctx.Length()/4; i+=8)
		Sayf(_L("  +%02x: %08x %08x %08x %08x %08x %08x %08x %08x"),i*4,w[i],w[i+1],w[i+2],w[i+3],w[i+4],w[i+5],w[i+6],w[i+7]);
	stop=1;
	t.Resume();
	User::WaitForRequest(st);
	t.Close();
	}

// ----- 13. the cost of alignment faults (last: it may restart the machine) --
static void ProbeAlignCost()
	{
	Say(_L("== 13. Alignment faults: the cost of 1500 (if the machine restarts here, that is the answer)"));
	TAlProbe* a=new TAlProbe;
	if (!a)
		return;
	Mem::FillZ(a,sizeof(TAlProbe));
	a->iMode=2;
	TExitType et=EExitPending;
	TInt r=RunThread(_L("align cost"),AlignThread,a,0x4000,60000,&et);
	Sayf(_L("%d accesses, %d of them faults (fixed), in %d ticks (1/64 s); bytewise: %d ticks; gave up %d; exit %d %d"),
		a->iRounds,a->iCount,a->iTicks,a->iTicksPlain,a->iUnfixable,et,r);
	delete a;
	}

// ----- the program -------------------------------------------------------
static void RunAll()
	{
	TEntry de;
	if (gFs->Entry(_L("D:\\LXDBT.TXT"),de)==KErrNone)
		{
		// only the Alpine-mode (DBT) probes
		ProbeAlign();
		ProbeCodeCache();
		ProbeContext();
		ProbeAlignCost();
		Say(_L("== done"));
		return;
		}
	ProbeCode();
	ProbeMemory();
	ProbeStacks();
	ProbeChunkStack();
	ProbeFaultKill();
	ProbeFaults();
	ProbeSpeed();
	ProbeRpc();
	ProbeAlign();
	ProbeCodeCache();
	ProbeContext();
	ProbeAlignCost();
	// These two restart the machine (see DESIGN.md): only with D:\\LXDANGER.TXT
	TEntry e;
	if (gFs->Entry(_L("D:\\LXDANGER.TXT"),e)==KErrNone)
		{
		ProbeSvc();
		ProbeInterrupt();
		}
	else
		Say(_L("(svc 0 and RaiseException probes skipped: they restart the machine)"));
	Say(_L("== done"));
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* tc=CTrapCleanup::New();
	RFs fs;
	if (fs.Connect()!=KErrNone)
		return KErrGeneral;
	gFs=&fs;
	TDriveInfo d;
	TFileName name(_L("C:\\LxProbe.log"));
	if (fs.Drive(d,EDriveD)==KErrNone && d.iType!=EMediaNotPresent)
		name[0]='D';
	if (gLog.Replace(fs,name,EFileWrite|EFileShareAny)==KErrNone)
		gLogOpen=ETrue;
	Sayf(_L("PsiLinux probe 0.3, log %S"),&name);
	RunAll();
	if (gLogOpen)
		gLog.Close();
	fs.Close();
	delete tc;
	return KErrNone;
	}
