// lxterm.cpp - PsiLinux's terminal view and its session with the runner.
//
// (0.29) The terminal is PsiTerm's (lxterm.h): its drawing, fonts, themes,
// history, selection and clipboard come from PsiTerm's psiterm.cpp as they
// are, in lxptview.cpp (tools/ptview.py). Here are the parts of PsiTerm's
// CTermView that are PsiLinux's own, and CLxTermView, the terminal with
// PsiLinux's session in place of SSH.
#include <e32keys.h>
#include <e32hal.h>
#include <baclipb.h>
#include <txtetext.h>
#include <eikapp.h>
#include <eikappui.h>
#include <estlib.h>
#include "lxterm.h"
#include "lxepocsv.h"

_LIT(KRunnerExe, "psilxrun.exe");
_LIT(KLxFontFile, "PsiTerm.gdr");          // PsiTerm's fonts, beside the program
const TInt KLxScrollbackLines = 300;       // (as PsiTerm's KScrollbackLines)
const TInt KLxScrollbackCols = 128;
const TInt KLxTickUs = 500000;             // the status line, the cursor's blink
const TInt KTickUs = KLxTickUs;            // (CLxSession's tick)

// ===========================================================================
// CLxSession
// ===========================================================================

CLxSession::CLxSession(CLxTermView& aView) : iView(aView), iLastState(-1)
	{
	}

CLxSession::~CLxSession()
	{
	Kill();
	delete iEpoc;
	delete iTick;
	delete iWaiter;                          // (before the chunk it points into)
	iRinger.Close();
	if (iChunkOpen)
		iChunk.Close();
	}

TInt CLxSession::Start(const TDesC& aRoot, const TDesC8& aProgram, TBool aLogin, TInt aRows, TInt aCols)
	{
	if (iRunning)
		return KErrInUse;
	if (!iChunkOpen)
		{
		TInt r = iChunk.CreateGlobal(_L(LX_SHARED_NAME), sizeof(LxShared), sizeof(LxShared));
		if (r == KErrAlreadyExists)
			r = iChunk.OpenGlobal(_L(LX_SHARED_NAME), EFalse);
		if (r != KErrNone)
			return r;
		iChunkOpen = ETrue;
		}
	iShared = (LxShared*)iChunk.Base();
	if (iEpoc)
		iEpoc->Forget();                     // (0.36) a message still up from the last session answers nobody
	Mem::FillZ(iShared, sizeof(LxShared));
	iShared->magic = LX_SHARED_MAGIC;
	iShared->version = LX_SHARED_VERSION;
	iShared->rows = aRows;
	iShared->cols = aCols;
	iShared->bell_magic = PSI_BELL_MAGIC;
	iShared->reserve = iReserve;
	iShared->log_on = iLogOn ? 1 : 0;
	iShared->welcome_off = iWelcomeOff ? 1 : 0;
	// (0.36) EPOC requests: this window answers them
	if (!iEpoc)
		{
		TRAPD(err, iEpoc = CLxEpoc::NewL());
		if (iEpoc)
			iEpoc->SetAnswered(TCallBack(EpocAnswered, this));
		}
	iShared->epoc_app = iEpoc ? 1 : 0;
	TPtr8 root((TUint8*)iShared->root, 0, sizeof(iShared->root) - 1);
	root.Copy(aRoot);
	TPtr8 prog((TUint8*)iShared->program, 0, sizeof(iShared->program) - 1);
	prog.Copy(aProgram);
	// "-l": a login shell, which reads /etc/profile (BusyBox init takes none)
	if (aLogin)
		{
		iShared->args[0] = '-';
		iShared->args[1] = 'l';
		}
	// the runner, beside this program
	TParse p;
	TFileName app(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName());
	p.Set(KRunnerExe, &app, NULL);
	TInt r = iProcess.Create(p.FullName(), KNullDesC);
	if (r != KErrNone)
		return r;
	iProcessOpen = ETrue;
	iRunning = ETrue;
	iLastState = -1;
	if (!iWaiter)
		iWaiter = new CPsiBellWaiter(TCallBack(BellCallback, this));
	if (!iTick)
		iTick = CPeriodic::New(CActive::EPriorityLow);
	if (iTick && !iTick->IsActive())
		iTick->Start(KTickUs, KTickUs, TCallBack(TickCallback, this));
	iProcess.Resume();
	Arm();
	return KErrNone;
	}

void CLxSession::Ring()
	{
	if (iShared)
		iRinger.Ring(&iShared->eng_bell);
	}

void CLxSession::Arm()
	{
	if (!iWaiter || !iShared)
		return;
	for (TInt i = 0; i < 8 && iWaiter->Arm(&iShared->app_bell); i++)
		Pump();
	}

TInt CLxSession::BellCallback(TAny* aSelf)
	{
	CLxSession* s = (CLxSession*)aSelf;
	s->Pump();
	s->Arm();
	return 0;
	}

TInt CLxSession::EpocAnswered(TAny* aSelf)
	{
	((CLxSession*)aSelf)->Ring();
	return 0;
	}

TInt CLxSession::TickCallback(TAny* aSelf)
	{
	CLxSession* s = (CLxSession*)aSelf;
	s->Beat();
	s->Pump();
	return 1;
	}

void CLxSession::Pump()
	{
	if (!iShared)
		return;
	// (0.37) the heartbeat, here too: a steady stream of output keeps the
	// bell's (higher priority) callback busy, and the tick, at
	// EPriorityLow, may not run for a long time; an app that pumps is alive
	Beat();
	TBool took = EFalse;
	TUint head = iShared->out_head;
	while (iShared->out_tail != head)
		{
		TUint tail = iShared->out_tail;
		TInt at = tail % LX_OUT_SIZE;
		TInt n = head - tail;
		if (n > LX_OUT_SIZE - at)
			n = LX_OUT_SIZE - at;
		iView.SessionOutput(iShared->out + at, n);
		iShared->out_tail = tail + n;
		took = ETrue;
		}
	if (took)
		Ring();                              // (room in 'out': a writer may be waiting)
	if (iScreenHook)
		{
		TBool on = iShared->fb_on && iRunning;
		if (iShared->fb_seq != iFbSeq || on != iFbOn)
			{
			iFbSeq = iShared->fb_seq;
			iFbOn = on;
			iScreenHook->ScreenOnOff(on);
			}
		if (on)
			iScreenHook->ScreenPump();
		}
	if (iShared->info_seq != iLastInfo)
		{
		iLastInfo = iShared->info_seq;
		iView.SessionInfoChanged();
		}
	// (0.36) an EPOC request from Linux (the epoc command)
	if (iEpoc && iRunning && iEpoc->Serve(*iShared))
		Ring();
	TInt state = iShared->state;
	if (iProcessOpen && iRunning && iProcess.ExitType() != EExitPending && state < LXS_EXITED)
		{
		// the runner died without saying so
		TExitCategoryName cat = iProcess.ExitCategory();
		TPtr8 msg((TUint8*)iShared->message, 0, sizeof(iShared->message) - 1);
		msg.Format(_L8("The Linux runner stopped (%d)"), iProcess.ExitReason());
		iShared->message[msg.Length()] = 0;
		iShared->state = state = LXS_FAILED;
		}
	if (state != iLastState)
		{
		iLastState = state;
		if (state >= LXS_EXITED)
			{
			iRunning = EFalse;
			if (iProcessOpen)
				{
				// give it a moment to end by itself
				for (TInt i = 0; i < 20 && iProcess.ExitType() == EExitPending; i++)
					User::After(50000);
				if (iProcess.ExitType() == EExitPending)
					iProcess.Kill(0);
				iProcess.Close();
				iProcessOpen = EFalse;
				}
			}
		iView.SessionStateChanged();
		}
	}

void CLxSession::Write(const TDesC8& aBytes)
	{
	if (!iShared || !iRunning)
		return;
	for (TInt i = 0; i < aBytes.Length(); i++)
		{
		TInt waited = 0;
		while (iShared->kbd_head - iShared->kbd_tail >= LX_KBD_SIZE && waited < 200)
			{
			Ring();
			User::After(5000);
			waited++;
			}
		if (iShared->kbd_head - iShared->kbd_tail >= LX_KBD_SIZE)
			{
			CEikonEnv::Static()->InfoMsg(_L("Linux is busy - some keys were not sent"));
			break;
			}
		iShared->kbd[iShared->kbd_head % LX_KBD_SIZE] = aBytes[i];
		iShared->kbd_head++;
		}
	Ring();
	}

void CLxSession::Resize(TInt aRows, TInt aCols)
	{
	if (!iShared)
		return;
	iShared->rows = aRows;
	iShared->cols = aCols;
	iShared->resize_seq++;
	Ring();
	}

void CLxSession::Stop()
	{
	if (!iShared || !iRunning)
		return;
	iShared->quit = 1;
	Ring();
	}

void CLxSession::Kill()
	{
	if (iRunning && iShared)
		{
		Stop();
		for (TInt i = 0; i < 30 && iProcessOpen && iProcess.ExitType() == EExitPending; i++)
			User::After(100000);
		}
	if (iProcessOpen)
		{
		if (iProcess.ExitType() == EExitPending)
			iProcess.Kill(0);
		iProcess.Close();
		iProcessOpen = EFalse;
		}
	iRunning = EFalse;
	}

// ===========================================================================
// PsiTerm's CTermView: the parts that are PsiLinux's
// ===========================================================================
// PsiTerm's terminal calls these; in PsiTerm they are its SSH session's.
// Every CTermView in PsiLinux is a CLxTermView.

static CLxTermView& Lx(CTermView* aView) { return *(CLxTermView*)aView; }
static const CLxTermView& Lx(const CTermView* aView) { return *(const CLxTermView*)aView; }

CTermView::CTermView()
	{
	iTbBusy = -1;
	iHostIndex = -1;
	}

CTermView::~CTermView()
	{
	delete iTick;
	delete iBenchTimer;
	if (iVt)
		vterm_free(iVt);
	User::Free(iSbCells);
	User::Free(iSbCols);
	ReleaseFont();
	if (iFontFileLoaded)
		iCoeEnv->ScreenDevice()->RemoveFile(iFontFileId);
	for (TInt f = 0; f < iExtraFonts; f++)
		iCoeEnv->ScreenDevice()->RemoveFile(iExtraFontId[f]);
	CloseSTDLIB();                           // (libvterm's C library)
	}

// PsiTerm's ConstructL, without the serial line, SSH, recent commands and
// the start screen
void CTermView::ConstructL(const TRect& aRect, const TPsiSettings& aSettings)
	{
	iSettings = aSettings;
	iTabRow = -1;
	{
	TTimeIntervalMicroSeconds32 period;
	iTickUsReal = (UserHal::TickPeriod(period) == KErrNone && period.Int() > 0) ? period.Int() : 15625;
	}
	iForeground = ETrue;
	// the screen's own mode: 16 greys on the 5mx, 256 colours on the netBook
	TDisplayMode mode = iCoeEnv->ScreenDevice()->DisplayMode();
	iColour = (mode >= EColor16);
	CreateBackedUpWindowL(iCoeEnv->RootWin(), iColour ? mode : EGray16);
	SetRectL(aRect);
	EnableDragEvents();
	iVt = vterm_new(24, 80);
	User::LeaveIfNull(iVt);
	vterm_set_utf8(iVt, 1);
	vterm_output_set_callback(iVt, &CTermView::CbOutput, this);
	iScreen = vterm_obtain_screen(iVt);
	Mem::FillZ(&iCallbacks, sizeof(iCallbacks));
	iCallbacks.damage = &CTermView::CbDamage;
	iCallbacks.moverect = &CTermView::CbMoveRect;
	iCallbacks.movecursor = &CTermView::CbMoveCursor;
	iCallbacks.settermprop = &CTermView::CbSetTermProp;
	iCallbacks.bell = &CTermView::CbBell;
	iCallbacks.sb_pushline = &CTermView::CbPushLine;
	iCallbacks.sb_clear = &CTermView::CbClearScrollback;
	iSbCells = (TSbCell*)User::Alloc(KLxScrollbackLines * KLxScrollbackCols * sizeof(TSbCell));
	iSbCols = (short*)User::Alloc(KLxScrollbackLines * sizeof(short));
	if (!iSbCells || !iSbCols)
		{
		User::Free(iSbCells);
		User::Free(iSbCols);
		iSbCells = NULL;
		iSbCols = NULL;
		}
	SbInit(&iSb, iSbCells, iSbCols, KLxScrollbackLines, KLxScrollbackCols);
	vterm_screen_set_callbacks(iScreen, &iCallbacks, this);
	vterm_screen_enable_altscreen(iScreen, 1);
	vterm_screen_set_damage_merge(iScreen, VTERM_DAMAGE_SCROLL);
	VTermColor fg, bg;
	vterm_color_rgb(&fg, 0, 0, 0);
	vterm_color_rgb(&bg, 255, 255, 255);
	vterm_state_set_default_colors(vterm_obtain_state(iVt), &fg, &bg);
	vterm_screen_reset(iScreen, 1);
	iCurVisible = ETrue;
	TParse parse;
	parse.Set(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName(), NULL, NULL);
	{
	TFileName fontFile(parse.DriveAndPath());
	fontFile.Append(KLxFontFile);
	if (iCoeEnv->ScreenDevice()->AddFile(fontFile, iFontFileId) == KErrNone)
		iFontFileLoaded = ETrue;
	}
	LoadFontFolders();
	ApplyFontL();
	ActivateL();
	DrawNow();
	StartTick();
	// PsiTerm's drawing benchmark, when Bench.txt is beside the program
	TFileName bench(parse.DriveAndPath());
	bench.Append(_L("Bench.txt"));
	TEntry e;
	if (iCoeEnv->FsSession().Entry(bench, e) == KErrNone)
		{
		iBenchTimer = CPeriodic::New(CActive::EPriorityLow);
		if (iBenchTimer)
			iBenchTimer->Start(3000000, 3000000, TCallBack(BenchCallback, this));
		}
	}

void CTermView::LocalMessage(const TDesC8& aText)
	{
	FeedTerminal(aText.Ptr(), aText.Length());
	}

// the bytes libvterm makes of a key, to the runner
void CTermView::WriteToHost(const TDesC8& aBytes)
	{
	Lx(this).SendBytes(aBytes);
	}

void CTermView::StatusText(TDes& aText, TInt& aSplit) const
	{
	Lx(this).LxStatusText(aText, aSplit);
	}

TKeyResponse CTermView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	return Lx(this).LxKeyL(aKeyEvent, aType);
	}

void CTermView::ApplyAppearanceL()
	{
	iBlinkHidden = EFalse;
	iCacheValid = EFalse;
	ApplyFontL();                            // lays out again and redraws
	StartTick();
	}

TInt CTermView::TickCallback(TAny* aSelf)
	{
	((CTermView*)aSelf)->Tick();
	return 1;
	}

TInt CTermView::BenchCallback(TAny* aSelf)
	{
	CTermView* self = (CTermView*)aSelf;
	self->iBenchTimer->Cancel();
	self->RunBench();
	return 0;
	}

// Every half second, in front: the cursor's blink and the status line
// (PsiTerm's tick, without SSH and tmux)
void CTermView::Tick()
	{
	if (!IsActivated() || iPaintGc || !iForeground)
		return;
	if (iSettings.iBlink && iCurVisible && iScrollOffset == 0)
		{
		iBlinkHidden = !iBlinkHidden;
		BeginPaint();
		EndPaint();
		}
	else if (iBlinkHidden)
		{
		iBlinkHidden = EFalse;
		BeginPaint();
		EndPaint();
		}
	if (iStatusH)
		{
		TBuf<120> text;
		TInt split;
		StatusText(text, split);
		if (text != iStatusDrawn)
			{
			ActivateGc();
			DrawStatus(SystemGc());
			DeactivateGc();
			}
		}
	}

TBool CTermView::TickBusy() const
	{
	return iForeground;
	}

void CTermView::StartTick()
	{
	if (!iTick)
		iTick = CPeriodic::New(CActive::EPriorityLow);
	if (!iTick)
		return;
	iTick->Cancel();
	iBlinkHidden = EFalse;
	iTickUs = iForeground ? KLxTickUs : 4 * KLxTickUs;
	iTick->Start(iTickUs, iTickUs, TCallBack(TickCallback, this));
	}

void CTermView::SetForeground(TBool aForeground)
	{
	if (aForeground == iForeground)
		return;
	iForeground = aForeground;
	StartTick();
	if (iForeground)
		Tick();
	}

// no serial line in PsiLinux
void CTermView::SerialDataL(const TDesC8& /*aData*/)
	{
	}

void CTermView::SerialError(TInt /*aError*/)
	{
	}

// no tmux in PsiLinux's terminal: no tab strip (iTabsTop is never set)
TInt CTermView::TabStripHeight() const
	{
	return 0;
	}

void CTermView::DrawTabs(CWindowGc& /*aGc*/) const
	{
	}

TBool CTermView::TabPenDownL(TInt /*aX*/)
	{
	return EFalse;
	}

// ===========================================================================
// CLxTermView
// ===========================================================================

CLxTermView::CLxTermView()
	{
	iStartOnce = -1;
	}

CLxTermView::~CLxTermView()
	{
	delete iCloser;
	delete iSession;
	}

void CLxTermView::ToPsiTerm()
	{
	TPsiSettings& s = iSettings;
	TLxSettings& l = *iLxSettings;
	s.iZoom = l.iZoom;
	s.iTheme = l.iTheme;
	s.iCursor = l.iCursor;
	s.iBlink = l.iBlink;
	s.iStatus = l.iStatus;
	s.iBold = l.iBold;
	s.iFace = l.iSpare[KLxSpareFace];
	s.iFontName.Zero();                      // (the zoom levels: no Other font)
	s.iFontPixels = 0;
	s.iDisplay = 0;
	s.iBell = 0;                             // (the bell beeps)
	s.iTmuxTabs = 0;
	s.iStartScreen = 0;
	s.iKeepLog = l.iSpare[KLxSpareLog];
	}

void CLxTermView::ConstructL(const TRect& aRect, TLxSettings& aSettings)
	{
	iLxSettings = &aSettings;
	iSession = new(ELeave) CLxSession(*this);
	TPsiSettings s;
	Mem::FillZ(&s, sizeof(s));
	iSettings = s;
	ToPsiTerm();
	CTermView::ConstructL(aRect, iSettings);
	// the screen's greys as calibrated for PsiTerm, PsiMail and PsiWeb
	// (C:\System\Data\PsiGrey.ini), as PsiTerm draws with them
	PsiGrey grey;
	PsiGreyLoad(iCoeEnv->FsSession(), grey);
	SetGreys(grey);
	}

// (PsiTerm's Layout tells its SSH session the new size; here the runner,
// whose terminal (the kernel's tty) sends SIGWINCH)
void CLxTermView::SizeToRunner()
	{
	const LxShared* sh = iSession ? iSession->Shared() : NULL;
	if (sh && (sh->rows != iRows || sh->cols != iCols))
		iSession->Resize(iRows, iCols);
	}

void CLxTermView::ApplySettingsL()
	{
	ToPsiTerm();
	ApplyAppearanceL();
	SizeToRunner();
	if (iSession)
		iSession->SetReserve(ReserveBytes());
	}

void CLxTermView::ZoomBy(TInt aStep)
	{
	CTermView::ZoomBy(aStep);
	iLxSettings->iZoom = iSettings.iZoom;    // (saved with the settings)
	SizeToRunner();
	}

void CLxTermView::SetTermRectL(const TRect& aRect)
	{
	CTermView::SetTermRectL(aRect);
	SizeToRunner();
	}

// Preferences: "Keep memory for other programs" (DECISIONS.md 51)
TUint CLxTermView::ReserveBytes() const
	{
	if (!iLxSettings->iKeep)
		return 0;
	TInt kb = iLxSettings->iKeepKb ? iLxSettings->iKeepKb : 1024;
	return (TUint)kb * 1024;
	}

// The Linux folder: <disk>:\Linux\, on the disk chosen (the card if there is one)
static void LinuxRoot(RFs& aFs, TInt aDisk, TDes& aRoot)
	{
	TChar disk = (TChar)(aDisk ? aDisk : 'D');
	TInt drive;
	TDriveInfo di;
	if (RFs::CharToDrive(disk, drive) != KErrNone || aFs.Drive(di, drive) != KErrNone || di.iType == EMediaNotPresent)
		disk = 'C';
	aRoot.Zero();
	aRoot.Append(disk);
	aRoot.Append(_L(":\\Linux\\"));
	}

void CLxTermView::StartLinuxL()
	{
	TFileName root;
	LinuxRoot(iCoeEnv->FsSession(), iLxSettings->iDisk, root);
	iEnded = EFalse;
	// Alpine mode (linux/docs/ALPINE.md): Alpine's root is in \Linux\Alpine\;
	// it boots (BusyBox init, OpenRC), or goes straight to a shell
	TInt r;
	iSession->SetReserve(ReserveBytes());          // (Start passes it on)
	// (Keep a log; and while Coda's debug mode is on, C:\System\Data\PsiDiag.on:
	// Coda's Control panel > Diagnostics, whose PsiDiag.exe copies the log's
	// new lines into the system log)
	TEntry diag;
	iSession->SetLogOn(iLxSettings->iSpare[KLxSpareLog] != 0 ||
		iCoeEnv->FsSession().Entry(_L("C:\\System\\Data\\PsiDiag.on"), diag) == KErrNone);
	iSession->SetWelcome(iLxSettings->iSpare[KLxSpareNoWelcome] == 0);
	// (0.26) SetStartOnce() for this start only, else Preferences > Start
	TInt start = iStartOnce >= 0 ? iStartOnce : iLxSettings->iStart;
	iStartOnce = -1;
	TBool alpine = start == 0 || start == 1;
	if (alpine)
		{
		// (not installed: PsiLinux's own programs instead, and say so)
		TFileName img(root);
		img.Append(_L("Alpine\\ROOT.IMG"));
		TEntry e;
		if (iCoeEnv->FsSession().Entry(img, e) != KErrNone)
			{
			TBuf8<200> msg;
			TBuf8<64> img8;
			img8.Copy(img.Left(Min(img.Length(), 64)));
			msg.Copy(_L8("[Alpine Linux is not installed: there is no "));
			msg.Append(img8);
			msg.Append(_L8(". Install PsiLinux-alpine-root.sis and its parts on that disk. Starting PsiLinux]\r\n"));
			LocalMessage(msg);
			alpine = EFalse;
			}
		}
	if (alpine)
		{
		root.Append(_L("Alpine\\"));
		if (start == 0)
			r = iSession->Start(root, _L8("/sbin/init"), EFalse, iRows, iCols);
		else
			r = iSession->Start(root, _L8("/usr/sbin/psilinux-shell"), EFalse, iRows, iCols);
		}
	else
		r = iSession->Start(root, _L8("/bin/sh"), ETrue, iRows, iCols);
	iSshActive = iSession->Running();        // (PsiTerm's bell rings only in a live session)
	if (r != KErrNone)
		{
		TBuf8<120> msg;
		msg.Format(_L8("\r\n[PsiLinux: could not start the runner (%d)]\r\n"), r);
		LocalMessage(msg);
		iEnded = ETrue;
		}
	if (IsActivated())
		DrawNow();
	}

void CLxTermView::AlpineRelease(TDes& aOut)
	{
	const LxShared* sh = iSession ? iSession->Shared() : NULL;
	if (sh && sh->release[0])
		{
		TPtrC8 p((const TUint8*)sh->release);
		iAlpine.Copy(p.Left(Min(p.Length(), iAlpine.MaxLength())));
		}
	aOut.Copy(iAlpine.Left(Min(iAlpine.Length(), aOut.MaxLength())));
	}

void CLxTermView::SessionOutput(const TUint8* aBytes, TInt aLen)
	{
	FeedTerminal(aBytes, aLen);
	if (iKeyAt)
		EchoDrawn();
	}

void CLxTermView::SessionStateChanged()
	{
	TInt state = iSession->State();
	// (0.36) "exit" (or ^D) at the top shell: Linux has ended by itself, and
	// its programs have been ended and their files closed (the runner waits
	// for that): PsiLinux closes, back to where you were. A failure, or a
	// shell killed by a signal, stays on the screen to be read.
	if (state == LXS_EXITED && (iSession->ExitStatus() & 0x7f) == 0)
		{
		iEnded = ETrue;
		if (!iCloser)
			iCloser = CIdle::New(CActive::EPriorityStandard);
		if (iCloser && !iCloser->IsActive())
			{
			iCloser->Start(TCallBack(CloseApp, this));
			iSshActive = EFalse;
			return;
			}
		}
	if (state == LXS_EXITED || state == LXS_FAILED)
		{
		iEnded = ETrue;
		TBuf8<220> msg;
		if (state == LXS_FAILED)
			{
			TPtrC8 why((const TUint8*)iSession->Shared()->message);
			msg.Format(_L8("\r\n[%S]\r\n[Enter starts Linux again]\r\n"), &why);
			}
		else
			msg.Copy(_L8("\r\n[Linux has ended. Enter starts it again]\r\n"));
		LocalMessage(msg);
		}
	iSshActive = iSession->Running();        // (PsiTerm's bell rings only in a live session)
	SessionInfoChanged();
	}

TInt CLxTermView::CloseApp(TAny*)
	{
	CEikonEnv::Static()->EikAppUi()->HandleCommandL(EEikCmdExit);
	return 0;
	}

void CLxTermView::SessionInfoChanged()
	{
	if (!IsActivated() || !iStatusH || iPaintGc)
		return;
	ActivateGc();
	DrawStatus(SystemGc());
	DeactivateGc();
	}

// keys and libvterm's answers, to the runner; the echo's time starts here
void CLxTermView::SendBytes(const TDesC8& aBytes)
	{
	if (!iSession || !iSession->Running())
		return;
	if (!iKeyAt)
		iKeyAt = User::TickCount() | 1;      // (0: none waiting)
	iSession->Write(aBytes);
	}

// (0.29) the output after a key has been drawn: its echo time
void CLxTermView::EchoDrawn()
	{
	TInt ms = (TInt)((User::TickCount() | 1) - iKeyAt) * (iTickUsReal / 100) / 10;
	iKeyAt = 0;
	if (ms < 0 || ms > 60000)
		return;
	iEchoLastMs = ms;
	iEchoAvgMs = iEchoAvgMs ? (iEchoAvgMs * 7 + ms) / 8 : ms;
	}

// ----- the status line ---------------------------------------------------------
// What runs (the foreground program, how many programs, their memory) on the
// left; the time on the right. With Keep a log on, the echo time too.
void CLxTermView::LxStatusText(TDes& aText, TInt& aSplit) const
	{
	aText.Zero();
	const LxShared* sh = iSession ? iSession->Shared() : NULL;
	TInt state = iSession ? iSession->State() : LXS_EXITED;
	if (!sh || (!iSession->Running() && state < LXS_EXITED))
		aText.Copy(_L("Linux is not running   Enter: start it"));
	else if (state == LXS_STARTING)
		aText.Copy(_L("Starting Linux..."));
	else if (state == LXS_FAILED)
		aText.Copy(_L("Linux could not start   Enter: try again"));
	else if (state == LXS_EXITED)
		aText.Copy(_L("Linux has ended   Enter: start it again"));
	else
		{
		TBuf<32> fg;
		TPtrC8 name((const TUint8*)sh->fg_name);
		fg.Copy(name.Left(Min(name.Length(), 24)));
		if (fg.Length() == 0)
			fg.Copy(_L("sh"));
		TInt used = (TInt)(sh->mem_used / 1024), freeKb = (TInt)(sh->mem_free / 1024);
		if (sh->nprocs == 1)
			aText.Format(_L("\x95 %S   1 program, %d KB   %d KB free"), &fg, used, freeKb);
		else
			aText.Format(_L("\x95 %S   %d programs, %d KB   %d KB free"), &fg, sh->nprocs, used, freeKb);
		if (sh->log_on && iEchoLastMs)
			aText.AppendFormat(_L("   echo %d ms, avg %d"), iEchoLastMs, iEchoAvgMs);
		}
	aSplit = aText.Length();
	TTime now;
	now.HomeTime();
	TDateTime t = now.DateTime();
	aText.AppendFormat(_L("%02d:%02d"), t.Hour(), t.Minute());
	}

// ----- keys ----------------------------------------------------------------------
// As PsiTerm's, without its SSH, tmux, snippets and recent commands: and
// with nothing running, Enter starts Linux.
TKeyResponse CLxTermView::LxKeyL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasNotConsumed;
	TUint code = aKeyEvent.iCode;
	TUint mods = aKeyEvent.iModifiers;
	if (code == EKeyMenu)
		return EKeyWasNotConsumed;
	if (iEnded || !iSession->Running())
		{
		if (code == EKeyEnter)
			{
			LocalMessage(_L8("\r\n"));
			StartLinuxL();
			}
		return EKeyWasConsumed;
		}
	// Shift+Pg Up/Pg Dn/Home/End move through the history
	if ((mods & EModifierShift) && !(mods & EModifierCtrl))
		{
		switch (code)
			{
		case EKeyPageUp:   ScrollBy(iRows - 1); return EKeyWasConsumed;
		case EKeyPageDown: ScrollBy(-(iRows - 1)); return EKeyWasConsumed;
		case EKeyHome:     ScrollTo(iSb.iCount); return EKeyWasConsumed;
		case EKeyEnd:      ScrollTo(0); return EKeyWasConsumed;
		default: break;
			}
		}
	if (iSelActive)
		ClearSelection();
	if (iScrollOffset > 0)
		ScrollTo(0);
	TInt vm = VTERM_MOD_NONE;
	if (mods & EModifierShift) vm |= VTERM_MOD_SHIFT;
	if (mods & EModifierCtrl) vm |= VTERM_MOD_CTRL;
	switch (code)
		{
	case EKeyUpArrow:    SendKey(VTERM_KEY_UP, vm); break;
	case EKeyDownArrow:  SendKey(VTERM_KEY_DOWN, vm); break;
	case EKeyLeftArrow:  SendKey(VTERM_KEY_LEFT, vm); break;
	case EKeyRightArrow: SendKey(VTERM_KEY_RIGHT, vm); break;
	case EKeyPageUp:     SendKey(VTERM_KEY_PAGEUP, vm); break;
	case EKeyPageDown:   SendKey(VTERM_KEY_PAGEDOWN, vm); break;
	case EKeyHome:       SendKey(VTERM_KEY_HOME, vm); break;
	case EKeyEnd:        SendKey(VTERM_KEY_END, vm); break;
	case EKeyEnter:      SendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE); break;
	case EKeyBackspace:  SendKey(VTERM_KEY_BACKSPACE, VTERM_MOD_NONE); break;
	case EKeyDelete:     SendKey(VTERM_KEY_DEL, VTERM_MOD_NONE); break;
	case EKeyEscape:     SendKey(VTERM_KEY_ESCAPE, VTERM_MOD_NONE); break;
	case EKeyTab:        SendKey(VTERM_KEY_TAB, vm & VTERM_MOD_SHIFT); break;
	default:
		if (code >= EKeyF1 && code <= EKeyF12)
			{
			SendKey((VTermKey)VTERM_KEY_FUNCTION(code - EKeyF1 + 1), VTERM_MOD_NONE);
			break;
			}
		if (code < 0x20)
			{
			SendCtrl(code | 0x40);
			break;
			}
		if (code < 0x100)
			{
			if ((mods & EModifierCtrl) && ((code >= 'a' && code <= 'z') ||
				(code >= 'A' && code <= 'Z') || code == '\\' || code == ']' ||
				code == '[' || code == '^' || code == '_' || code == '@'))
				{
				SendCtrl(code);
				break;
				}
			SendChar(PsiCodePageToUnicode(code));
			break;
			}
		return EKeyWasNotConsumed;
		}
	return EKeyWasConsumed;
	}

// (PsiTerm's pen gives a program that asked for the mouse its clicks only
// while logged in: here, while Linux runs)
TBool CTermView::SshLoggedIn() const
	{
	CLxSession* s = ((CLxTermView*)CONST_CAST(CTermView*, this))->Session();
	return s && s->Running();
	}

// (a terminal title changed: PsiTerm asks tmux for its windows; not here)
void CTermView::TmuxQueryKick()
	{
	}

// ----- the rest of PsiTerm's CTermView: SSH, the serial line, tmux, Other
// font (psiterm.h's inline functions call some of these; PsiLinux never does)
void CTermView::RingSsh()
	{
	}

void CTermView::RunToolDialogL()
	{
	}

TInt CTermView::ParseTabList(const TDesC8& /*aText*/, TTmuxTab* /*aTabs*/, TInt& aCurrent)
	{
	aCurrent = -1;
	return 0;
	}

void CSerialPort::Close()
	{
	}

TBool CFontDialog::FaceHeights(const TDesC& /*aName*/, TInt* /*aPixels*/, TInt& aCount, TBool& aProportional)
	{
	aCount = 0;                              // (Other font...: not in PsiLinux)
	aProportional = EFalse;
	return EFalse;
	}
