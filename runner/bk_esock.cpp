// bk_esock.cpp - sockets for PsiLinux's runner: ESOCK, the Psion's own
// Internet connection (PPP, through NIFMAN: the dial-up starts when the
// first socket connects or a name is looked up, as for every Psion program).
//
// The kernel thread must never wait, so every ESOCK request is an active
// object: a socket keeps one receive outstanding into its buffer, has at
// most one send in flight, and connects and accepts in the background. When
// any of them completes, lx_k_event() makes the kernel try its parked calls
// again (lx_net.c), and bk_sock_poll() says what is ready.
//
// While any Linux socket is open, NIFMAN's idle timers are held off (as
// psiglue's NifHoldLink does for the other apps), so the link does not drop
// under a long download; they are given back when the last socket closes.
//
// (0.41) Dialling first. On the Psion Internet route a modem such as the
// PsiConnect Atom must be sent Connection settings' first send (ATD777)
// before it talks PPP; left to itself, a lookup or connect has EPOC's
// NetDial dial with its own modem script, which the Atom takes for a host
// name (NO CARRIER). So before the first lookup, TCP connect or UDP send
// to the outside while the link is down, psion-core's TPsiDial::Start
// (ssh/psidial.h) runs in a thread of its own (it waits on the serial port
// for up to 30 s; the kernel thread never waits), and what asked waits for
// it: a lookup or connect is started once it is done, a UDP send is tried
// again. A link that is already up (another program's) is used and left
// alone. When Linux has had no socket or lookup for KLxIdleEndUs, and when
// PsiLinux stops, TPsiDial::End takes a link it brought up down again, or
// hands it to psihold.exe if "Stay connected" is on.
#include <e32base.h>
#include <es_sock.h>
#include <in_sock.h>
#include <nifman.h>
#include "psidial.h"
extern "C" {
#include "lx_core.h"
}

static RSocketServ gSs;
static TBool gSsOpen;
static RNif gNif;
static TBool gNifOpen, gNifHeld;
static TInt gOpenSocks;

static int NetErr(TInt aErr)
	{
	switch (aErr)
		{
	case KErrNone: return 0;
	case KErrCouldNotConnect: return -LX_ECONNREFUSED;
	case KErrTimedOut: return -LX_ETIMEDOUT;
	case KErrDisconnected: case KErrEof: return -LX_ECONNRESET;
	case KErrNotFound: return -LX_ENOENT;
	case KErrAlreadyExists: case KErrInUse: return -LX_EADDRINUSE;
	case KErrNoMemory: return -LX_ENOMEM;
	case KErrCancel: return -LX_EINTR;
	case KErrNotSupported: return -LX_EOPNOTSUPP;
	case KErrArgument: return -LX_EINVAL;
		}
	return -LX_ENETUNREACH;
	}

static TInt Server()
	{
	if (gSsOpen)
		return KErrNone;
	TInt r = gSs.Connect();
	if (r == KErrNone)
		gSsOpen = ETrue;
	return r;
	}

// the link stays up while Linux has sockets
static void HoldLink(TBool aHold)
	{
	if (aHold && !gNifHeld)
		{
		if (!gNifOpen && gNif.Open() == KErrNone)
			gNifOpen = ETrue;
		if (gNifOpen && gNif.DisableTimers(ETrue) == KErrNone)
			gNifHeld = ETrue;
		}
	else if (!aHold && gNifHeld)
		{
		gNif.DisableTimers(EFalse);
		gNifHeld = EFalse;
		gNif.Close();
		gNifOpen = EFalse;
		}
	}

// network byte order (as Linux keeps it in memory) <-> ESOCK's host order
static TUint32 HostAddr(lx_u32 aNet)
	{
	return ((aNet & 0xff) << 24) | ((aNet & 0xff00) << 8) | ((aNet >> 8) & 0xff00) | (aNet >> 24);
	}
static lx_u32 NetAddr(TUint32 aHost) { return HostAddr(aHost); }
static TUint HostPort(lx_u16 aNet) { return ((aNet & 0xff) << 8) | (aNet >> 8); }
static lx_u16 NetPort(TUint aHost) { return (lx_u16)(((aHost & 0xff) << 8) | ((aHost >> 8) & 0xff)); }

class CLxSock;
// (0.39) sockets and DNS lookups that exist: while any does, Linux does not
// sleep (their requests read into and answer for its programs)
static TInt gNetObjs;
static TBool gDialBusy;
extern "C" int bk_net_busy(void) { return gNetObjs > 0 || gDialBusy; }

// ---- (0.41) dialling first (TPsiDial) --------------------------------------------

// what waits for the dial: a lookup or a connect, started (or failed) once
// it is done
class MLxDialWait
	{
public:
	virtual void DialDone(TInt aErr) = 0;
	MLxDialWait* iNextWait;
	TBool iWaiting;
	};

const TInt KLxIdleEndUs = 20000000;          // no socket or lookup this long: Linux's use has ended
const TInt KLxDialFailHoldUs = 5000000;      // a failed dial answers the next asks at once, this long

static TPsiDial gDial;
static TBool gChecked;      // Start has been done for this spell of use (or the link is ours and up)
static TInt gDialFailed;    // the last Start's failure (0: none), and when
static TUint gDialFailedAt;
static MLxDialWait* gWaits;

static void NetIdle();

class CLxDialer : public CActive
	{
public:
	enum { EJobStart, EJobEnd };
	CLxDialer() : CActive(EPriorityStandard) { CActiveScheduler::Add(this); }
	~CLxDialer() { Cancel(); }
	void Run(TInt aJob);
	TBool Busy() const { return IsActive(); }
	TInt iJob;
	TBuf<120> iWhy;
	void Stop();
private:
	void RunL();
	void DoCancel();
	RThread iThread;
	};

static CLxDialer* gDialer;

static TInt LxDialThread(TAny* aDialer)
	{
	CLxDialer* d = (CLxDialer*)aDialer;
	CTrapCleanup* cleanup = CTrapCleanup::New();
	RFs fs;
	TInt r = KErrNoMemory;
	if (cleanup && fs.Connect() == KErrNone)
		{
		if (d->iJob == CLxDialer::EJobStart)
			r = gDial.Start(fs, d->iWhy);
		else
			{
			gDial.End(fs);
			r = KErrNone;
			}
		fs.Close();
		}
	delete cleanup;
	return r;                                // (the exit reason: RunL reads it)
	}

void CLxDialer::Run(TInt aJob)
	{
	iJob = aJob;
	iWhy.Zero();
	TBuf<24> name;
	name.Format(_L("LxDial%u"), User::TickCount());
	// a start has a heap of its own (it may be killed as PsiLinux stops);
	// an end shares the runner's, as PsiLinkDisconnect's record must
	// outlive it
	TInt r = aJob == EJobStart ? iThread.Create(name, LxDialThread, 8192, 4096, 65536, this)
		: iThread.Create(name, LxDialThread, 8192, &User::Heap(), this, EOwnerProcess);
	if (r != KErrNone)
		{
		bk_log("network: no thread to dial in (%d): the lookup dials by itself", r);
		iStatus = KRequestPending;
		SetActive();
		TRequestStatus* s = &iStatus;
		User::RequestComplete(s, KErrNone);
		iThread.SetHandle(0);
		return;
		}
	if (aJob == EJobStart)
		bk_log("network: checking the link before Linux's first lookup or connect (TPsiDial)");
	gDialBusy = ETrue;
	iThread.Logon(iStatus);
	SetActive();
	iThread.Resume();
	}

void CLxDialer::DoCancel()
	{
	if (iThread.Handle())
		{
		iThread.LogonCancel(iStatus);
		iThread.Kill(KErrCancel);            // (the serial port goes with it)
		iThread.Close();
		}
	gDialBusy = EFalse;
	}

// PsiLinux stopping: a dial in progress is given up, an end is waited for
void CLxDialer::Stop()
	{
	if (IsActive() && iJob == EJobEnd && iThread.Handle())
		{
		User::WaitForRequest(iStatus);       // (bounded: PsiLinkDisconnect waits 10 s at most)
		iThread.Close();
		gDialBusy = EFalse;
		return;                              // (the request is used: Cancel does nothing more)
		}
	Cancel();
	}

void CLxDialer::RunL()
	{
	TInt r = iStatus.Int();
	if (iThread.Handle())
		{
		if (iThread.ExitType() != EExitKill)
			r = KErrDied;
		iThread.Close();
		}
	gDialBusy = EFalse;
	if (iJob == EJobEnd)
		{
		bk_log("network: Linux's use has ended: a link it brought up is down again (or held, Stay connected)");
		lx_k_event();                        // (a UDP send waiting tries again)
		if (gWaits)
			{
			// (something asked while it was hanging up: dial again)
			gChecked = EFalse;
			Run(EJobStart);
			}
		return;
		}
	if (r == KErrNotSupported)
		r = KErrNone;                        // (the modem route: lookups and connects as before)
	if (r == KErrNone)
		{
		gChecked = ETrue;
		gDialFailed = 0;
		if (gDial.Opened())
			bk_log("network: the Psion Internet first send done: PPP comes up with the lookup");
		}
	else
		{
		TBuf8<120> why;
		why.Copy(iWhy);
		why.Append(0);
		bk_log("network: could not connect (%d): %s", r, why.Ptr());
		gChecked = EFalse;
		gDialFailed = r == KErrDied ? KErrCouldNotConnect : r;
		gDialFailedAt = User::TickCount();
		}
	while (gWaits)
		{
		MLxDialWait* w = gWaits;
		gWaits = w->iNextWait;
		w->iNextWait = 0;
		w->iWaiting = EFalse;
		w->DialDone(r);
		}
	lx_k_event();
	if (!gNetObjs)
		NetIdle();
	}

// the idle wait: Linux's use of the network has ended when it has had no
// socket or lookup for KLxIdleEndUs
class CLxNetIdle : public CTimer
	{
public:
	CLxNetIdle() : CTimer(EPriorityStandard) {}
	void ConstructL() { CTimer::ConstructL(); CActiveScheduler::Add(this); }
private:
	void RunL()
		{
		gChecked = EFalse;
		if (gNetObjs || !gDial.Opened() || !gDialer || gDialer->Busy())
			return;
		gDialer->Run(CLxDialer::EJobEnd);
		}
	};
static CLxNetIdle* gNetIdle;

static TBool DialerReady()
	{
	if (!gDialer)
		gDialer = new CLxDialer;
	if (gDialer && !gNetIdle)
		{
		gNetIdle = new CLxNetIdle;
		if (gNetIdle)
			{
			TRAPD(r, gNetIdle->ConstructL());
			if (r != KErrNone)
				{
				delete gNetIdle;
				gNetIdle = 0;
				}
			}
		}
	return gDialer != 0;
	}

// no socket or lookup left: the idle wait starts
static void NetIdle()
	{
	gChecked = EFalse;                       // (the next use checks the link again)
	if (gNetIdle && gDial.Opened() && !(gDialer && gDialer->Busy()))
		{
		gNetIdle->Cancel();
		gNetIdle->After(KLxIdleEndUs);
		}
	}

// Before a lookup, a TCP connect or a UDP send to the outside: KErrNone go
// on now; KErrCompletion wait (aWait, if given, is told; without it, try
// again on lx_k_event); else the error to give.
static TInt DialGate(MLxDialWait* aWait)
	{
	if (gNetIdle)
		gNetIdle->Cancel();
	if (!DialerReady())
		return KErrNone;
	if (gDialer->Busy())
		{
		if (gDialer->iJob == CLxDialer::EJobEnd)
			;                                // (RunL dials again once the end is done)
		else if (!aWait)
			return KErrCompletion;
		}
	else
		{
		if (gChecked)
			return KErrNone;
		if (gDialFailed && PsiDialSince(gDialFailedAt) < KLxDialFailHoldUs)
			return gDialFailed;
		if (gDial.Opened() && PsiLinkActive() == 1)
			{
			gChecked = ETrue;                // (ours, and still up)
			return KErrNone;
			}
		gDialer->Run(CLxDialer::EJobStart);
		if (!gDialer->Busy())
			return KErrNone;
		}
	if (aWait)
		{
		aWait->iNextWait = gWaits;
		gWaits = aWait;
		aWait->iWaiting = ETrue;
		}
	return KErrCompletion;
	}

static void DialUnwait(MLxDialWait* aWait)
	{
	if (!aWait->iWaiting)
		return;
	for (MLxDialWait** p = &gWaits; *p; p = &(*p)->iNextWait)
		if (*p == aWait)
			{
			*p = aWait->iNextWait;
			break;
			}
	aWait->iWaiting = EFalse;
	}

// loopback and this network's own addresses need no dial
static TBool Outside(TUint32 aHost)
	{
	return (aHost >> 24) != 127 && aHost != 0;
	}

// PsiLinux stopping (bk_epoc.cpp, after the kernel has shut down): a dial
// given up, and a link Linux brought up down again
extern "C" void bk_net_end(void)
	{
	if (gNetIdle)
		{
		gNetIdle->Cancel();
		delete gNetIdle;
		gNetIdle = 0;
		}
	if (gDialer)
		{
		gDialer->Stop();
		delete gDialer;
		gDialer = 0;
		}
	gWaits = 0;
	if (gDial.Opened())
		{
		RFs fs;
		if (fs.Connect() == KErrNone)
			{
			gDial.End(fs);
			fs.Close();
			bk_log("network: PsiLinux stopping: a link it brought up is down again (or held, Stay connected)");
			}
		}
	}

class CSockOp : public CActive
	{
public:
	CSockOp(CLxSock& aSock, TInt aKind) : CActive(EPriorityStandard), iSock(aSock), iKind(aKind) { CActiveScheduler::Add(this); }
	~CSockOp() { Cancel(); }
	void Go() { SetActive(); }
	TRequestStatus& Status() { return iStatus; }
private:
	void RunL();
	void DoCancel();
	CLxSock& iSock;
	TInt iKind;
	};

enum { EOpConnect, EOpRecv, EOpSend, EOpAccept };

const TInt KRxSize = 4096;
const TInt KTxSize = 2048;

class CLxSock : public CBase, public MLxDialWait
	{
public:
	~CLxSock();
	void DialDone(TInt aErr);
	TInetAddr iConnTo;          // (0.41) a connect waiting for the dial
	TInt Construct(TInt aKind);
	void Done(TInt aKind, TInt aStatus);
	void StartRecv();
	RSocket iSock;
	TBool iOpen;
	TInt iKind;                 // BK_SOCK_TCP or BK_SOCK_UDP
	TBool iConnecting, iConnected, iListening, iEof;
	TInt iErr;                  // the pending error (SO_ERROR), as -LX_E*
	CSockOp* iConn;
	CSockOp* iRecv;
	CSockOp* iSend;
	TUint8 iRx[KRxSize];
	TPtr8 iRxDes;
	TInt iRxPos, iRxLen;
	TSockXfrLength iRxXfr;
	TInetAddr iRxFrom;          // UDP: who sent the datagram in iRx
	TUint8 iTx[KTxSize];
	TPtr8 iTxDes;
	TBool iSending;
	TInetAddr iTxTo;
	// listening: the accepted socket, waiting for accept()
	CLxSock* iAccepting;
	TBool iAccepted;
	CLxSock() : iRxDes(iRx, 0, KRxSize), iTxDes(iTx, 0, KTxSize) { gNetObjs++; iNextWait = 0; iWaiting = EFalse; }
	};

void CLxSock::DialDone(TInt aErr)
	{
	if (!iConnecting)
		return;
	if (aErr != KErrNone)
		{
		iConnecting = EFalse;
		iErr = -LX_ENETUNREACH;
		return;
		}
	iSock.Connect(iConnTo, iConn->Status());   // (starts the dial-up if the link is down)
	iConn->Go();
	}

void CSockOp::RunL()
	{
	iSock.Done(iKind, iStatus.Int());
	lx_k_event();
	}

void CSockOp::DoCancel()
	{
	switch (iKind)
		{
	case EOpConnect: iSock.iSock.CancelConnect(); break;
	case EOpRecv: iSock.iSock.CancelRecv(); break;
	case EOpSend: iSock.iSock.CancelSend(); break;
	case EOpAccept: iSock.iSock.CancelAccept(); break;
		}
	}

TInt CLxSock::Construct(TInt aKind)
	{
	iKind = aKind;
	iConn = new CSockOp(*this, EOpConnect);
	iRecv = new CSockOp(*this, EOpRecv);
	iSend = new CSockOp(*this, EOpSend);
	if (!iConn || !iRecv || !iSend)
		return KErrNoMemory;
	return KErrNone;
	}

CLxSock::~CLxSock()
	{
	DialUnwait(this);
	gNetObjs--;
	delete iConn;
	delete iRecv;
	delete iSend;
	if (iAccepting)
		delete iAccepting;
	if (iOpen)
		{
		iSock.Close();
		if (--gOpenSocks == 0)
			HoldLink(EFalse);
		}
	if (!gNetObjs)
		NetIdle();
	}

void CLxSock::StartRecv()
	{
	if (!iOpen || iRecv->IsActive() || iEof || iErr || iRxPos < iRxLen)
		return;
	if (iKind == BK_SOCK_TCP && !iConnected)
		return;
	iRxDes.Set(iRx, 0, KRxSize);
	iRxPos = iRxLen = 0;
	if (iKind == BK_SOCK_TCP)
		iSock.RecvOneOrMore(iRxDes, 0, iRecv->Status(), iRxXfr);
	else
		iSock.RecvFrom(iRxDes, iRxFrom, 0, iRecv->Status());
	iRecv->Go();
	}

void CLxSock::Done(TInt aKind, TInt aStatus)
	{
	switch (aKind)
		{
	case EOpConnect:
		iConnecting = EFalse;
		if (aStatus == KErrNone)
			{
			iConnected = ETrue;
			StartRecv();
			}
		else
			iErr = NetErr(aStatus);
		break;
	case EOpRecv:
		if (aStatus == KErrNone)
			{
			iRxPos = 0;
			iRxLen = iRxDes.Length();
			}
		else if (aStatus == KErrEof || aStatus == KErrDisconnected)
			iEof = ETrue;
		else if (aStatus != KErrCancel)
			iErr = NetErr(aStatus);
		break;
	case EOpSend:
		iSending = EFalse;
		if (aStatus != KErrNone && aStatus != KErrCancel)
			iErr = aStatus == KErrEof || aStatus == KErrDisconnected ? -LX_EPIPE : NetErr(aStatus);
		break;
	case EOpAccept:
		if (aStatus == KErrNone)
			iAccepted = ETrue;
		else
			{
			delete iAccepting;
			iAccepting = 0;
			if (aStatus != KErrCancel)
				iErr = NetErr(aStatus);
			}
		break;
		}
	}

extern "C" void* bk_sock_new(int kind, int* err)
	{
	TInt r = Server();
	if (r != KErrNone)
		{
		*err = -LX_ENETDOWN;
		return 0;
		}
	CLxSock* s = new CLxSock;
	if (!s || s->Construct(kind) != KErrNone)
		{
		delete s;
		*err = -LX_ENOMEM;
		return 0;
		}
	r = kind == BK_SOCK_TCP ? s->iSock.Open(gSs, KAfInet, KSockStream, KProtocolInetTcp)
		: s->iSock.Open(gSs, KAfInet, KSockDatagram, KProtocolInetUdp);
	if (r != KErrNone)
		{
		delete s;
		*err = NetErr(r);
		return 0;
		}
	s->iOpen = ETrue;
	if (gOpenSocks++ == 0)
		HoldLink(ETrue);
	return s;
	}

extern "C" void bk_sock_close(void* p) { delete (CLxSock*)p; }

extern "C" int bk_sock_connect(void* p, lx_u32 addr, lx_u16 port)
	{
	CLxSock* s = (CLxSock*)p;
	if (s->iConnecting)
		return -LX_EALREADY;
	TInetAddr a(HostAddr(addr), HostPort(port));
	if (s->iKind == BK_SOCK_UDP)
		{
		// (a UDP "connect" only names the peer)
		s->iTxTo = a;
		s->iConnected = ETrue;
		s->StartRecv();
		return 0;
		}
	s->iConnecting = ETrue;
	s->iErr = 0;
	s->iConnTo = a;
	TInt g = Outside(a.Address()) ? DialGate(s) : KErrNone;
	if (g == KErrCompletion)
		return -LX_EINPROGRESS;              // (DialDone connects, or fails it)
	if (g != KErrNone)
		{
		s->iConnecting = EFalse;
		return -LX_ENETUNREACH;
		}
	s->iSock.Connect(a, s->iConn->Status());   // (starts the dial-up if the link is down)
	s->iConn->Go();
	return -LX_EINPROGRESS;
	}

extern "C" int bk_sock_bind(void* p, lx_u32 addr, lx_u16 port)
	{
	CLxSock* s = (CLxSock*)p;
	TInetAddr a(HostAddr(addr), HostPort(port));
	TInt r = s->iSock.Bind(a);
	if (r == KErrNone && s->iKind == BK_SOCK_UDP)
		{
		s->iConnected = ETrue;
		s->StartRecv();
		}
	return NetErr(r);
	}

static void StartAccept(CLxSock* s)
	{
	if (s->iAccepting || s->iErr)
		return;
	CLxSock* n = new CLxSock;
	if (!n || n->Construct(BK_SOCK_TCP) != KErrNone || n->iSock.Open(gSs) != KErrNone)
		{
		delete n;
		return;
		}
	n->iOpen = ETrue;
	gOpenSocks++;
	s->iAccepting = n;
	s->iAccepted = EFalse;
	s->iSock.Accept(n->iSock, s->iConn->Status());
	// (the accept completes on the listening socket's connect slot)
	s->iConn->Go();
	}

extern "C" int bk_sock_listen(void* p, int backlog)
	{
	CLxSock* s = (CLxSock*)p;
	TInt r = s->iSock.Listen(backlog > 0 ? backlog : 1);
	if (r != KErrNone)
		return NetErr(r);
	s->iListening = ETrue;
	StartAccept(s);
	return 0;
	}

extern "C" void* bk_sock_accept(void* p, int* err, lx_u32* addr, lx_u16* port)
	{
	CLxSock* s = (CLxSock*)p;
	if (s->iErr)
		{
		*err = s->iErr;
		return 0;
		}
	if (!s->iAccepted || !s->iAccepting)
		{
		StartAccept(s);
		*err = -LX_EAGAIN;
		return 0;
		}
	CLxSock* n = s->iAccepting;
	s->iAccepting = 0;
	s->iAccepted = EFalse;
	n->iConnected = ETrue;
	TInetAddr a;
	n->iSock.RemoteName(a);
	*addr = NetAddr(a.Address());
	*port = NetPort(a.Port());
	n->StartRecv();
	StartAccept(s);
	return n;
	}

extern "C" int bk_sock_send(void* p, const char* buf, lx_u32 n)
	{
	CLxSock* s = (CLxSock*)p;
	if (s->iErr)
		{
		TInt e = s->iErr;
		s->iErr = 0;
		return e;
		}
	if (s->iKind == BK_SOCK_TCP && !s->iConnected)
		return s->iConnecting ? -LX_EAGAIN : -LX_ENOTCONN;
	if (s->iSending)
		return -LX_EAGAIN;
	if (s->iKind == BK_SOCK_UDP && Outside(s->iTxTo.Address()))
		{
		TInt g = DialGate(0);
		if (g == KErrCompletion)
			return -LX_EAGAIN;               // (tried again on lx_k_event, once dialled)
		if (g != KErrNone)
			return -LX_ENETUNREACH;
		}
	if (n > (lx_u32)KTxSize)
		n = KTxSize;
	Mem::Copy(s->iTx, buf, n);
	s->iTxDes.Set(s->iTx, n, KTxSize);
	s->iSending = ETrue;
	if (s->iKind == BK_SOCK_UDP)
		s->iSock.SendTo(s->iTxDes, s->iTxTo, 0, s->iSend->Status());
	else
		s->iSock.Send(s->iTxDes, 0, s->iSend->Status());
	s->iSend->Go();
	return (int)n;
	}

extern "C" int bk_sock_recv(void* p, char* buf, lx_u32 n, int peek)
	{
	CLxSock* s = (CLxSock*)p;
	if (s->iRxPos < s->iRxLen)
		{
		TInt k = s->iRxLen - s->iRxPos;
		if ((TInt)n < k)
			k = n;
		Mem::Copy(buf, s->iRx + s->iRxPos, k);
		if (!peek)
			{
			s->iRxPos += k;
			s->StartRecv();
			}
		return k;
		}
	if (s->iEof)
		return 0;
	if (s->iErr)
		{
		TInt e = s->iErr;
		s->iErr = 0;
		return e;
		}
	if (s->iKind == BK_SOCK_TCP && !s->iConnected)
		return s->iConnecting ? -LX_EAGAIN : -LX_ENOTCONN;
	s->StartRecv();
	return -LX_EAGAIN;
	}

extern "C" int bk_sock_sendto(void* p, const char* buf, lx_u32 n, lx_u32 addr, lx_u16 port)
	{
	CLxSock* s = (CLxSock*)p;
	s->iTxTo = TInetAddr(HostAddr(addr), HostPort(port));
	if (!s->iConnected)
		{
		// (a first datagram binds the socket to any port, as on Linux)
		TInetAddr any(KInetAddrAny, 0);
		s->iSock.Bind(any);
		s->iConnected = ETrue;
		s->StartRecv();
		}
	return bk_sock_send(p, buf, n);
	}

extern "C" int bk_sock_recvfrom(void* p, char* buf, lx_u32 n, lx_u32* addr, lx_u16* port)
	{
	CLxSock* s = (CLxSock*)p;
	if (s->iRxPos < s->iRxLen)
		{
		TInt k = s->iRxLen - s->iRxPos;
		if ((TInt)n < k)
			k = n;
		Mem::Copy(buf, s->iRx + s->iRxPos, k);
		if (addr) *addr = NetAddr(s->iRxFrom.Address());
		if (port) *port = NetPort(s->iRxFrom.Port());
		s->iRxPos = s->iRxLen;               // (a datagram is read whole or cut)
		s->StartRecv();
		return k;
		}
	if (s->iErr)
		{
		TInt e = s->iErr;
		s->iErr = 0;
		return e;
		}
	s->StartRecv();
	return -LX_EAGAIN;
	}

extern "C" int bk_sock_shutdown(void* p, int how)
	{
	CLxSock* s = (CLxSock*)p;
	TRequestStatus st;
	RSocket::TShutdown mode = how == LX_SHUT_RD ? RSocket::EStopInput : how == LX_SHUT_WR ? RSocket::EStopOutput : RSocket::EStopInput;
	if (how == LX_SHUT_RDWR)
		return 0;                            // (close does it all)
	s->iSock.Shutdown(mode, st);
	User::WaitForRequest(st);
	return 0;
	}

extern "C" int bk_sock_poll(void* p)
	{
	CLxSock* s = (CLxSock*)p;
	int r = 0;
	if (s->iListening)
		return (s->iAccepted ? LX_POLLIN : 0) | (s->iErr ? LX_POLLERR : 0);
	if (s->iRxPos < s->iRxLen || s->iEof)
		r |= LX_POLLIN;
	if (s->iEof)
		r |= LX_POLLHUP;
	if (s->iErr)
		r |= LX_POLLERR;
	if ((s->iConnected || s->iKind == BK_SOCK_UDP) && !s->iSending && !(s->iKind == BK_SOCK_UDP && gDialBusy))
		r |= LX_POLLOUT;
	if (!r)
		s->StartRecv();
	return r;
	}

extern "C" int bk_sock_error(void* p)
	{
	CLxSock* s = (CLxSock*)p;
	TInt e = s->iErr;
	s->iErr = 0;
	return e;
	}

extern "C" int bk_sock_name(void* p, int peer, lx_u32* addr, lx_u16* port)
	{
	CLxSock* s = (CLxSock*)p;
	TInetAddr a;
	if (peer)
		s->iSock.RemoteName(a);
	else
		s->iSock.LocalName(a);
	*addr = NetAddr(a.Address());
	*port = NetPort(a.Port());
	return 0;
	}

// ---- name lookups ----------------------------------------------------------------

class CLxResolve : public CActive, public MLxDialWait
	{
public:
	CLxResolve() : CActive(EPriorityStandard), iDone(EFalse) { CActiveScheduler::Add(this); iNextWait = 0; iWaiting = EFalse; }
	~CLxResolve() { DialUnwait(this); Cancel(); if (iOpen) iRes.Close(); if (!--gNetObjs) NetIdle(); }
	TInt Start(const char* aName)
		{
		TInt r = Open();
		if (r != KErrNone)
			return r;
		TPtrC8 n((const TUint8*)aName);
		iName.Copy(n.Left(Min(n.Length(), iName.MaxLength())));
		TInt g = DialGate(this);
		if (g == KErrCompletion)
			{
			iSlow = ETrue;                   // (bk_resolve_slow: Linux's poll waits for it)
			iSlowFrom = User::TickCount();
			return KErrNone;                 // (DialDone looks it up)
			}
		if (g != KErrNone)
			{
			Fail(g);
			return KErrNone;
			}
		iRes.GetByName(iName, iEntry, iStatus);   // (starts the dial-up if needed)
		SetActive();
		return KErrNone;
		}
	TInt Open()
		{
		if (iOpen)
			return KErrNone;
		TInt r = Server();
		if (r == KErrNone)
			r = iRes.Open(gSs, KAfInet, KProtocolInetUdp);
		if (r == KErrNone)
			iOpen = ETrue;
		return r;
		}
	void DialDone(TInt aErr)
		{
		if (aErr == KErrNone)
			aErr = Open();
		if (aErr != KErrNone)
			{
			Fail(aErr);
			return;
			}
		iSlowFrom = User::TickCount();       // (a minute more for PPP to come up)
		iRes.GetByName(iName, iEntry, iStatus);   // (PPP comes up with it)
		SetActive();
		bk_log("network: dialled: looking up %s", (const char*)iName.PtrZ());
		}
	void Fail(TInt aErr)
		{
		iDone = ETrue;
		iResult = aErr == KErrNotFound ? KErrCouldNotConnect : aErr;
		}
	TBool iDone;
	TInt iResult;
	TNameEntry iEntry;
	TBool iSlow;                // (0.41) it waited for the dial: PPP is still coming up
	TUint iSlowFrom;
private:
	void RunL()
		{
		iDone = ETrue;
		iResult = iStatus.Int();
		if (iSlow)
			bk_log("network: the lookup after the dial: %d", iResult);
		lx_k_event();
		}
	void DoCancel() { iRes.Cancel(); }
	RHostResolver iRes;
	TBool iOpen;
	THostName iName;
	};

extern "C" void* bk_resolve_start(const char* name, int* err)
	{
	CLxResolve* r = new CLxResolve;
	if (!r)
		{
		*err = -LX_ENOMEM;
		return 0;
		}
	gNetObjs++;
	TInt e = r->Start(name);
	if (e != KErrNone)
		{
		delete r;
		*err = NetErr(e);
		return 0;
		}
	return r;
	}

extern "C" int bk_resolve_poll(void* p, lx_u32* addrs, int max)
	{
	CLxResolve* r = (CLxResolve*)p;
	if (!r->iDone)
		return -LX_EAGAIN;
	if (r->iResult == KErrNotFound)
		return -LX_ENOENT;                   // no such name
	if (r->iResult != KErrNone)
		return NetErr(r->iResult);           // (no link: the server failed)
	if (max < 1)
		return 0;
	addrs[0] = NetAddr(TInetAddr(r->iEntry().iAddr).Address());
	return 1;
	}

extern "C" void bk_resolve_free(void* p) { delete (CLxResolve*)p; }

// (0.41) a lookup that waits for the dial, and for PPP to come up after it:
// Linux's DNS poll waits on for it (lx_fsys.c): while the dial runs (the
// dial is bounded: 30 s for CONNECT), then a minute at most
extern "C" int bk_resolve_slow(void* p)
	{
	CLxResolve* r = (CLxResolve*)p;
	return !r->iDone && r->iSlow && (r->iWaiting || PsiDialSince(r->iSlowFrom) < 60000000);
	}
extern "C" int bk_dns_server(lx_u32*) { return -1; }
