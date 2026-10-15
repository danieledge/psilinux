// lxterm.cpp - PsiLinux's terminal view and its session with the runner.
// The drawing, colours, history, selection and clipboard code is PsiTerm's
// (app/psiterm.cpp, CTermView), kept as close to it as possible so that
// fixes can be carried between them.
#include <e32keys.h>
#include <e32hal.h>
#include <baclipb.h>
#include <txtetext.h>
#include <eikapp.h>
#include <eikappui.h>
#include <estlib.h>
#include "lxterm.h"

_LIT(KFontCourier, "Courier");
_LIT(KFontTerminus, "Terminus");         // from PsiTerm.gdr, beside the program
_LIT(KFontFile, "PsiTerm.gdr");
_LIT(KRunnerExe, "psilxrun.exe");

// as PsiTerm: Courier 8 (the most rows), then Terminus 12 to 18
const TInt KZoomFace[KLxZoomLevels] = { 1, 0, 0, 0, 0 };
const TInt KZoomPixels[KLxZoomLevels] = { 8, 12, 14, 16, 18 };
const TInt KDefaultZoom = 1;
const TInt KScrollbackLines = 300;
const TInt KScrollbackCols = 128;
const TInt KClipMax = 16384;
const TInt KTickUs = 500000;

// ===========================================================================
// CLxSession
// ===========================================================================

CLxSession::CLxSession(CLxTermView& aView) : iView(aView), iLastState(-1)
	{
	}

CLxSession::~CLxSession()
	{
	Kill();
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
	Mem::FillZ(iShared, sizeof(LxShared));
	iShared->magic = LX_SHARED_MAGIC;
	iShared->version = LX_SHARED_VERSION;
	iShared->rows = aRows;
	iShared->cols = aCols;
	iShared->bell_magic = PSI_BELL_MAGIC;
	iShared->reserve = iReserve;
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
	if (iShared->info_seq != iLastInfo)
		{
		iLastInfo = iShared->info_seq;
		iView.SessionInfoChanged();
		}
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
// CLxTermView
// ===========================================================================

CLxTermView::CLxTermView()
	{
	iForeground = ETrue;
	}

CLxTermView::~CLxTermView()
	{
	delete iSession;
	delete iTick;
	if (iVt)
		vterm_free(iVt);
	User::Free(iSbCells);
	User::Free(iSbCols);
	ReleaseFont();
	if (iFontFileLoaded)
		iCoeEnv->ScreenDevice()->RemoveFile(iFontFileId);
	CloseSTDLIB();                           // (libvterm's C library)
	}

void CLxTermView::ReleaseFont()
	{
	if (iFont)
		{
		iCoeEnv->ReleaseScreenFont(iFont);
		iFont = NULL;
		}
	}

void CLxTermView::ConstructL(const TRect& aRect, TLxSettings& aSettings)
	{
	iSettings = &aSettings;
	CreateBackedUpWindowL(iCoeEnv->RootWin(), EGray16);
	SetRectL(aRect);
	EnableDragEvents();
	iVt = vterm_new(24, 80);
	User::LeaveIfNull(iVt);
	vterm_set_utf8(iVt, 1);
	vterm_output_set_callback(iVt, &CLxTermView::CbOutput, this);
	iScreen = vterm_obtain_screen(iVt);
	Mem::FillZ(&iCallbacks, sizeof(iCallbacks));
	iCallbacks.damage = &CLxTermView::CbDamage;
	iCallbacks.moverect = &CLxTermView::CbMoveRect;
	iCallbacks.movecursor = &CLxTermView::CbMoveCursor;
	iCallbacks.settermprop = &CLxTermView::CbSetTermProp;
	iCallbacks.bell = &CLxTermView::CbBell;
	iCallbacks.sb_pushline = &CLxTermView::CbPushLine;
	iCallbacks.sb_clear = &CLxTermView::CbClearScrollback;
	iSbCells = (TSbCell*)User::Alloc(KScrollbackLines * KScrollbackCols * sizeof(TSbCell));
	iSbCols = (short*)User::Alloc(KScrollbackLines * sizeof(short));
	if (!iSbCells || !iSbCols)
		{
		User::Free(iSbCells);
		User::Free(iSbCols);
		iSbCells = NULL;
		iSbCols = NULL;
		}
	SbInit(&iSb, iSbCells, iSbCols, KScrollbackLines, KScrollbackCols);
	vterm_screen_set_callbacks(iScreen, &iCallbacks, this);
	vterm_screen_enable_altscreen(iScreen, 1);
	vterm_screen_set_damage_merge(iScreen, VTERM_DAMAGE_SCROLL);
	VTermColor fg, bg;
	vterm_color_rgb(&fg, 0, 0, 0);
	vterm_color_rgb(&bg, 255, 255, 255);
	vterm_state_set_default_colors(vterm_obtain_state(iVt), &fg, &bg);
	vterm_screen_reset(iScreen, 1);
	iCurVisible = ETrue;
	{
	TParse parse;
	parse.Set(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName(), NULL, NULL);
	TFileName fontFile(parse.DriveAndPath());
	fontFile.Append(KFontFile);
	if (iCoeEnv->ScreenDevice()->AddFile(fontFile, iFontFileId) == KErrNone)
		iFontFileLoaded = ETrue;
	}
	iSession = new(ELeave) CLxSession(*this);
	SetFontL(iSettings->iZoom);
	ActivateL();
	DrawNow();
	StartTick();
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
	LinuxRoot(iCoeEnv->FsSession(), iSettings->iDisk, root);
	iEnded = EFalse;
	// Alpine mode (linux/docs/ALPINE.md): Alpine's root is in \Linux\Alpine\;
	// it boots (BusyBox init, OpenRC), or goes straight to a shell
	TInt r;
	iSession->SetReserve(ReserveBytes());          // (Start passes it on)
	TBool alpine = iSettings->iStart == 0 || iSettings->iStart == 1;
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
			msg.Append(_L8(". Install PsiLinux-alpine-root.sis on that disk. Starting PsiLinux]\r\n"));
			LocalMessage(msg);
			alpine = EFalse;
			}
		}
	if (alpine)
		{
		root.Append(_L("Alpine\\"));
		// (shell only: bash if it starts, else BusyBox sh: psilinux-shell;
		// an Alpine root from before 0.21 has none, and the runner then
		// starts /bin/sh -l)
		if (iSettings->iStart == 0)
			r = iSession->Start(root, _L8("/sbin/init"), EFalse, iRows, iCols);
		else
			r = iSession->Start(root, _L8("/usr/sbin/psilinux-shell"), EFalse, iRows, iCols);
		}
	else
		r = iSession->Start(root, _L8("/bin/sh"), ETrue, iRows, iCols);
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
	}

void CLxTermView::SessionStateChanged()
	{
	TInt state = iSession->State();
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
	SessionInfoChanged();
	}

void CLxTermView::SessionInfoChanged()
	{
	if (!IsActivated() || !iStatusH || iPaintGc)
		return;
	ActivateGc();
	DrawStatus(SystemGc());
	DeactivateGc();
	}

void CLxTermView::LocalMessage(const TDesC8& aText)
	{
	FeedTerminal(aText.Ptr(), aText.Length());
	}

void CLxTermView::ZoomBy(TInt aStep)
	{
	TInt z = (iSettings->iZoom + aStep + KLxZoomLevels) % KLxZoomLevels;
	if (z != iSettings->iZoom)
		SetFontL(z);
	}

void CLxTermView::SetFontL(TInt aZoom)
	{
	if (aZoom < 0 || aZoom >= KLxZoomLevels)
		aZoom = KDefaultZoom;
	iSettings->iZoom = aZoom;
	ReleaseFont();
	TInt pixels = KZoomPixels[aZoom];
	TInt twips = iCoeEnv->ScreenDevice()->VerticalPixelsToTwips(pixels);
	if (KZoomFace[aZoom] == 0)
		{
		TFontSpec spec(KFontTerminus, twips);
		iFont = iCoeEnv->CreateScreenFontL(spec);
		if (iFont->FontSpecInTwips().iTypeface.iName.CompareF(KFontTerminus) != 0)
			ReleaseFont();
		}
	if (!iFont)
		{
		TFontSpec spec(KFontCourier, twips);
		iFont = iCoeEnv->CreateScreenFontL(spec);
		}
	iCellW = 1;
	const TText probe[] = { 'M', 'W', 'm', 'w', '@', '#', '0' };
	for (TUint i = 0; i < sizeof(probe) / sizeof(probe[0]); i++)
		{
		TInt w = iFont->CharWidthInPixels(probe[i]);
		if (w > iCellW)
			iCellW = w;
		}
	iCellH = iFont->HeightInPixels();
	if (iCellH < 6)
		iCellH = 6;
	iAscent = iFont->AscentInPixels();
	iMono = ETrue;
	for (TUint g = 32; g < 256 && iMono; g++)
		{
		if (g >= 127 && g < 160)
			continue;
		if (iFont->CharWidthInPixels((TChar)g) != iCellW)
			iMono = EFalse;
		}
	Layout();
	}

void CLxTermView::Layout()
	{
	TSize size = Rect().Size();
	iStatusH = 0;
	if (iSettings->iStatus)
		{
		iStatusH = iEikonEnv->AnnotationFont()->HeightInPixels() + 4;
		size.iHeight -= iStatusH;
		}
	iCols = size.iWidth / iCellW;
	iRows = size.iHeight / iCellH;
	if (iCols > 160) iCols = 160;
	if (iRows > 60) iRows = 60;
	if (iCols < 20) iCols = 20;
	if (iRows < 5) iRows = 5;
	iOriginX = (size.iWidth - iCols * iCellW) / 2;
	iOriginY = (size.iHeight - iRows * iCellH) / 2;
	iScrollOffset = 0;
	iSelActive = EFalse;
	iCacheValid = EFalse;
	vterm_set_size(iVt, iRows, iCols);
	vterm_screen_flush_damage(iScreen);
	if (iSession)
		iSession->Resize(iRows, iCols);
	iDamaged = EFalse;
	if (IsActivated())
		DrawNow();
	}

void CLxTermView::SetTermRectL(const TRect& aRect)
	{
	if (aRect == Rect())
		return;
	SetRectL(aRect);
	SetFontL(iSettings->iZoom);
	}

void CLxTermView::ApplySettingsL()
	{
	iBlinkHidden = EFalse;
	iCacheValid = EFalse;
	SetFontL(iSettings->iZoom);
	StartTick();
	if (iSession)
		iSession->SetReserve(ReserveBytes());
	}

// Preferences: "Keep memory for other programs" (DECISIONS.md 51)
TUint CLxTermView::ReserveBytes() const
	{
	if (!iSettings->iKeep)
		return 0;
	TInt kb = iSettings->iKeepKb ? iSettings->iKeepKb : 1024;
	return (TUint)kb * 1024;
	}

void CLxTermView::FeedTerminal(const TUint8* aBytes, TInt aLen)
	{
	TBool outer = (iPaintGc == NULL) && IsActivated();
	if (outer)
		BeginPaint();
	vterm_input_write(iVt, (const char*)aBytes, aLen);
	if (outer)
		EndPaint();
	}

// ----- libvterm's callbacks --------------------------------------------------

int CLxTermView::CbDamage(VTermRect aRect, void* aUser)
	{
	CLxTermView* self = (CLxTermView*)aUser;
	if (self->iScrollOffset > 0)
		{
		self->iNeedFull = ETrue;
		return 1;
		}
	if (self->iPaintGc)
		self->DrawCells(*self->iPaintGc, aRect.start_row, aRect.start_col, aRect.end_row, aRect.end_col);
	else
		self->AddDamage(aRect.start_row, aRect.start_col, aRect.end_row, aRect.end_col);
	return 1;
	}

int CLxTermView::CbMoveRect(VTermRect aDest, VTermRect aSrc, void* aUser)
	{
	CLxTermView* self = (CLxTermView*)aUser;
	if (self->iScrollOffset > 0)
		{
		self->iNeedFull = ETrue;
		return 1;
		}
	if (!self->iPaintGc)
		return 0;
	TInt w = self->iCellW, h = self->iCellH;
	TRect src(TPoint(self->iOriginX + aSrc.start_col * w, self->iOriginY + aSrc.start_row * h),
		TPoint(self->iOriginX + aSrc.end_col * w, self->iOriginY + aSrc.end_row * h));
	TPoint offset((aDest.start_col - aSrc.start_col) * w, (aDest.start_row - aSrc.start_row) * h);
	self->iPaintGc->CopyRect(offset, src);
	return 1;
	}

int CLxTermView::CbPushLine(int aCols, const VTermScreenCell* aCells, void* aUser)
	{
	CLxTermView* self = (CLxTermView*)aUser;
	TSbCell line[KScrollbackCols];
	if (aCols > KScrollbackCols)
		aCols = KScrollbackCols;
	for (TInt i = 0; i < aCols; i++)
		{
		const VTermScreenCell& c = aCells[i];
		TInt fg, bg;
		self->CellColours(c, fg, bg);
		TUint ch = c.chars[0];
		line[i].iCh = (ch == (TUint)-1 || ch > 0xFFFF) ? 0xFFFF : (unsigned short)ch;
		line[i].iGrey = (unsigned char)((fg << 4) | bg);
		line[i].iFlags = (unsigned char)((c.attrs.bold ? KSbBold : 0) | (c.attrs.underline ? KSbUnderline : 0)
			| (c.attrs.strike ? KSbStrike : 0) | (c.width > 1 ? KSbWide : 0));
		}
	SbPush(&self->iSb, line, aCols);
	self->iLinesPushed++;
	if (self->iScrollOffset > 0)
		{
		if (self->iScrollOffset < self->iSb.iCount)
			self->iScrollOffset++;
		self->iNeedFull = ETrue;
		}
	return 1;
	}

int CLxTermView::CbClearScrollback(void* aUser)
	{
	CLxTermView* self = (CLxTermView*)aUser;
	SbClear(&self->iSb);
	if (self->iScrollOffset)
		{
		self->iScrollOffset = 0;
		self->iNeedFull = ETrue;
		}
	return 1;
	}

int CLxTermView::CbMoveCursor(VTermPos aPos, VTermPos, int aVisible, void* aUser)
	{
	CLxTermView* self = (CLxTermView*)aUser;
	self->iCurRow = aPos.row;
	self->iCurCol = aPos.col;
	self->iCurVisible = aVisible;
	return 1;
	}

int CLxTermView::CbSetTermProp(VTermProp aProp, VTermValue* aVal, void* aUser)
	{
	CLxTermView* self = (CLxTermView*)aUser;
	if (aProp == VTERM_PROP_CURSORVISIBLE)
		self->iCurVisible = aVal->boolean;
	else if (aProp == VTERM_PROP_MOUSE)
		self->iMouseMode = aVal->number;
	return 1;
	}

int CLxTermView::CbBell(void* aUser)
	{
	CLxTermView* self = (CLxTermView*)aUser;
	TUint now = User::TickCount();
	if (now - self->iLastBell < 16)
		return 1;
	self->iLastBell = now;
	CEikonEnv::Beep();
	return 1;
	}

// libvterm's answers (keys as escape sequences, replies to queries)
void CLxTermView::CbOutput(const char* aBytes, size_t aLen, void* aUser)
	{
	CLxTermView* self = (CLxTermView*)aUser;
	self->SendBytes(TPtrC8((const TUint8*)aBytes, aLen));
	}

void CLxTermView::SendBytes(const TDesC8& aBytes)
	{
	if (iSession && iSession->Running())
		iSession->Write(aBytes);
	}

// ----- drawing (PsiTerm's) -----------------------------------------------------

void CLxTermView::AddDamage(TInt aRow0, TInt aCol0, TInt aRow1, TInt aCol1)
	{
	if (!iDamaged)
		{
		iDmgRow0 = aRow0; iDmgCol0 = aCol0; iDmgRow1 = aRow1; iDmgCol1 = aCol1;
		iDamaged = ETrue;
		return;
		}
	if (aRow0 < iDmgRow0) iDmgRow0 = aRow0;
	if (aCol0 < iDmgCol0) iDmgCol0 = aCol0;
	if (aRow1 > iDmgRow1) iDmgRow1 = aRow1;
	if (aCol1 > iDmgCol1) iDmgCol1 = aCol1;
	}

void CLxTermView::BeginPaint()
	{
	ActivateGc();
	iPaintGc = &SystemGc();
	iPaintGc->UseFont(iFont);
	if (iDrawnCurVisible && iDrawnCurRow < iRows && iDrawnCurCol < iCols)
		DrawOneCell(*iPaintGc, iDrawnCurRow, iDrawnCurCol);
	iDrawnCurVisible = EFalse;
	if (iDamaged)
		{
		iDamaged = EFalse;
		DrawCells(*iPaintGc, iDmgRow0, iDmgCol0, iDmgRow1, iDmgCol1);
		}
	}

void CLxTermView::EndPaint()
	{
	vterm_screen_flush_damage(iScreen);
	if (iNeedFull)
		{
		iNeedFull = EFalse;
		DrawAll(*iPaintGc);
		}
	DrawCursor(*iPaintGc);
	iPaintGc->DiscardFont();
	iPaintGc = NULL;
	DeactivateGc();
	}

void CLxTermView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	gc.UseFont(iFont);
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(Grey(15));
	gc.DrawRect(Rect());
	CONST_CAST(CLxTermView*, this)->iDamaged = EFalse;
	DrawAll(gc);
	DrawCursor(gc);
	DrawStatus(gc);
	gc.UseFont(iFont);
	}

void CLxTermView::DrawAll(CWindowGc& aGc) const
	{
	DrawCells(aGc, 0, 0, iRows, iCols);
	DrawScrollIndicator(aGc);
	}

void CLxTermView::DrawScrollIndicator(CWindowGc& aGc) const
	{
	if (iScrollOffset <= 0)
		return;
	TBuf<40> text;
	text.Format(_L(" history -%d/%d "), iScrollOffset, iSb.iCount);
	TInt w = iFont->TextWidthInPixels(text);
	TInt x = Rect().iBr.iX - w - 2;
	TRect box(TPoint(x, iOriginY), TSize(w, iCellH));
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(Grey(0));
	aGc.DrawRect(box);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(Grey(15));
	aGc.DrawText(text, TPoint(x, iOriginY + iAscent));
	}

void CLxTermView::DrawCells(CWindowGc& aGc, TInt aRow0, TInt aCol0, TInt aRow1, TInt aCol1) const
	{
	if (aRow0 < 0) aRow0 = 0;
	if (aCol0 < 0) aCol0 = 0;
	if (aRow1 > iRows) aRow1 = iRows;
	if (aCol1 > iCols) aCol1 = iCols;
	TBuf<160> run;
	const TUint KRunFlags = KSbBold | KSbUnderline | KSbStrike;
	for (TInt r = aRow0; r < aRow1; r++)
		{
		TInt c = aCol0;
		TLook look;
		TInt byte = -1;
		TBool have = EFalse;
		while (c < aCol1)
			{
			if (!have)
				{
				if (!GetLook(r, c, look))
					{
					c++;
					continue;
					}
				byte = TextByte(look);
				}
			have = EFalse;
			if (byte < 0)
				{
				DrawOneCell(aGc, r, c, look);
				c++;
				continue;
				}
			TInt fg = look.iFg, bg = look.iBg;
			TUint flags = look.iFlags & KRunFlags;
			TBool ink = (byte != ' ');
			TInt start = c;
			run.Zero();
			run.Append((TText)byte);
			for (c++; c < aCol1; c++)
				{
				TLook next;
				if (!GetLook(r, c, next))
					break;
				TInt b = TextByte(next);
				if (b < 0 || next.iFg != fg || next.iBg != bg || (next.iFlags & KRunFlags) != flags)
					{
					look = next;
					byte = b;
					have = ETrue;
					break;
					}
				run.Append((TText)b);
				if (b != ' ')
					ink = ETrue;
				}
			TBool bold = (flags & KSbBold) != 0 || iSettings->iBold;
			TBool ul = (flags & KSbUnderline) != 0;
			TBool st = (flags & KSbStrike) != 0;
			TInt x0 = iOriginX + start * iCellW;
			TInt y0 = RowY(r);
			TInt x1 = x0 + run.Length() * iCellW;
			aGc.SetPenStyle(CGraphicsContext::ENullPen);
			aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
			TInt tf, tb;
			ThemePair(fg, bg, tf, tb);
			aGc.SetBrushColor(TRgb::Gray16(tb));
			aGc.DrawRect(TRect(x0, y0, x1, y0 + iCellH));
			if (!ink && !ul && !st)
				continue;
			aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
			aGc.SetPenStyle(CGraphicsContext::ESolidPen);
			aGc.SetPenColor(TRgb::Gray16(tf));
			if (ink)
				{
				TPoint base(x0, y0 + iAscent);
				if (iMono)
					{
					aGc.DrawText(run, base);
					if (bold)
						aGc.DrawText(run, base + TPoint(1, 0));
					}
				else
					{
					for (TInt i = 0; i < run.Length(); i++)
						{
						if (run[i] == ' ')
							continue;
						TPtrC one(run.Mid(i, 1));
						TPoint at(x0 + i * iCellW, base.iY);
						aGc.DrawText(one, at);
						if (bold)
							aGc.DrawText(one, at + TPoint(1, 0));
						}
					}
				}
			if (ul)
				aGc.DrawLine(TPoint(x0, y0 + iCellH - 1), TPoint(x1, y0 + iCellH - 1));
			if (st)
				aGc.DrawLine(TPoint(x0, y0 + iCellH / 2), TPoint(x1, y0 + iCellH / 2));
			}
		}
	}

TBool CLxTermView::GetLook(TInt aRow, TInt aCol, TLook& aLook) const
	{
	TInt line = ViewLine(aRow);
	if (line >= iLinesPushed)
		{
		VTermPos pos;
		pos.row = line - iLinesPushed;
		pos.col = aCol;
		VTermScreenCell cell;
		if (!vterm_screen_get_cell(iScreen, pos, &cell))
			return EFalse;
		aLook.iCh = cell.chars[0];
		aLook.iWidth = cell.width;
		aLook.iFlags = (cell.attrs.bold ? KSbBold : 0) | (cell.attrs.underline ? KSbUnderline : 0)
			| (cell.attrs.strike ? KSbStrike : 0);
		CLxTermView* self = CONST_CAST(CLxTermView*, this);
		if (!iCacheValid || (TInt)cell.attrs.reverse != iCacheRev
			|| !vterm_color_is_equal(&cell.fg, &iCacheFg) || !vterm_color_is_equal(&cell.bg, &iCacheBg))
			{
			CellColours(cell, self->iCacheF, self->iCacheB);
			self->iCacheFg = cell.fg;
			self->iCacheBg = cell.bg;
			self->iCacheRev = cell.attrs.reverse;
			self->iCacheValid = ETrue;
			}
		aLook.iFg = iCacheF;
		aLook.iBg = iCacheB;
		}
	else
		{
		int cols;
		const TSbCell* sb = SbLine(&iSb, iLinesPushed - line, &cols);
		if (sb && aCol < cols)
			{
			const TSbCell& c = sb[aCol];
			aLook.iCh = (c.iCh == 0xFFFF) ? (TUint)-1 : c.iCh;
			aLook.iWidth = (c.iFlags & KSbWide) ? 2 : 1;
			aLook.iFg = c.iGrey >> 4;
			aLook.iBg = c.iGrey & 0x0f;
			aLook.iFlags = c.iFlags & (KSbBold | KSbUnderline | KSbStrike);
			}
		else
			{
			aLook.iCh = 0;
			aLook.iWidth = 1;
			aLook.iFg = 0;
			aLook.iBg = 15;
			aLook.iFlags = 0;
			}
		}
	if (iSelActive)
		{
		TInt l0 = iSelLine0, c0 = iSelCol0, l1 = iSelLine1, c1 = iSelCol1;
		SbOrder(&l0, &c0, &l1, &c1);
		if (SbInSelection(line, aCol, l0, c0, l1, c1))
			{
			TInt t = aLook.iFg;
			aLook.iFg = aLook.iBg;
			aLook.iBg = t;
			TInt d = aLook.iFg - aLook.iBg;
			if (d < 0) d = -d;
			if (d < 6)
				{
				aLook.iBg = 0;
				aLook.iFg = 15;
				}
			}
		}
	return ETrue;
	}

static TBool IsBlankChar(TUint aCh)
	{
	return aCh == 0 || aCh == ' ' || aCh == (TUint)-1 || aCh == 0xA0 ||
		(aCh >= 0x2000 && aCh <= 0x200A) || aCh == 0x202F || aCh == 0x205F || aCh == 0x3000;
	}

TInt CLxTermView::TextByte(const TLook& aLook) const
	{
	TUint ch = aLook.iCh;
	if (IsBlankChar(ch))
		return ' ';
	if (aLook.iWidth != 1)
		return -1;
	if (PsiBoxSegments(ch) > 0)
		return -1;
	if ((ch >= 0x2580 && ch <= 0x259F) || (ch >= 0x2800 && ch <= 0x28FF))
		return -1;
	TInt byte = PsiMapToCodePage(ch);
	if (byte == 0)
		return ' ';
	if (byte < 0)
		return '?';
	return byte;
	}

// colour -> one of 16 greys (PsiTerm's mapping, without its calibration):
// the 256 xterm colours become greys by brightness, text kept dark enough
// to read on the light screen
static TInt GreyOf(const VTermScreen* aScreen, VTermColor aCol)
	{
	vterm_screen_convert_color_to_rgb(aScreen, &aCol);
	TUint x = aCol.rgb.red * 30 + aCol.rgb.green * 59 + aCol.rgb.blue * 11;
	return (TInt)((x * 9869u) >> 24);
	}

void CLxTermView::CellColours(const VTermScreenCell& aCell, TInt& aFg, TInt& aBg) const
	{
	TInt fg = 0;
	TInt bg = 15;
	TBool fgDefault = VTERM_COLOR_IS_DEFAULT_FG(&aCell.fg);
	TBool bgDefault = VTERM_COLOR_IS_DEFAULT_BG(&aCell.bg);
	if (!fgDefault)
		{
		fg = GreyOf(iScreen, aCell.fg);
		if (bgDefault)
			fg = (fg * 6 + 7) / 15;
		}
	if (!bgDefault)
		bg = GreyOf(iScreen, aCell.bg);
	if (aCell.attrs.reverse)
		{
		TInt t = fg; fg = bg; bg = t;
		}
	TInt diff = fg - bg;
	if (diff < 0) diff = -diff;
	if (diff < 6)
		fg = (bg >= 8) ? 0 : 15;
	aFg = fg;
	aBg = bg;
	}

void CLxTermView::DrawOneCell(CWindowGc& aGc, TInt aRow, TInt aCol) const
	{
	TLook look;
	if (GetLook(aRow, aCol, look))
		DrawOneCell(aGc, aRow, aCol, look);
	}

void CLxTermView::DrawOneCell(CWindowGc& aGc, TInt aRow, TInt aCol, const TLook& aLook) const
	{
	TInt fg = aLook.iFg, bg = aLook.iBg;
	TRect box(TPoint(iOriginX + aCol * iCellW, RowY(aRow)), TSize(iCellW, iCellH));
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	TInt tf, tb;
	ThemePair(fg, bg, tf, tb);
	aGc.SetBrushColor(TRgb::Gray16(tb));
	aGc.DrawRect(box);
	TUint ch = aLook.iCh;
	if (IsBlankChar(ch))
		return;
	TRgb fgRgb = TRgb::Gray16(tf);
	TInt seg = PsiBoxSegments(ch);
	if (seg > 0)
		{
		aGc.SetPenStyle(CGraphicsContext::ESolidPen);
		aGc.SetPenColor(fgRgb);
		aGc.SetPenSize(TSize((seg & KBoxHeavy) ? 2 : 1, (seg & KBoxHeavy) ? 2 : 1));
		TInt cx = box.iTl.iX + iCellW / 2;
		TInt cy = box.iTl.iY + iCellH / 2;
		TInt x0 = box.iTl.iX, x1 = box.iBr.iX;
		TInt y0 = box.iTl.iY, y1 = box.iBr.iY;
		if (seg & KBoxDouble)
			{
			if (seg & (KBoxLeft | KBoxRight))
				{
				TInt a = (seg & KBoxLeft) ? x0 : cx;
				TInt b = (seg & KBoxRight) ? x1 : cx + 1;
				aGc.DrawLine(TPoint(a, cy - 1), TPoint(b, cy - 1));
				aGc.DrawLine(TPoint(a, cy + 1), TPoint(b, cy + 1));
				}
			if (seg & (KBoxUp | KBoxDown))
				{
				TInt a = (seg & KBoxUp) ? y0 : cy;
				TInt b = (seg & KBoxDown) ? y1 : cy + 1;
				aGc.DrawLine(TPoint(cx - 1, a), TPoint(cx - 1, b));
				aGc.DrawLine(TPoint(cx + 1, a), TPoint(cx + 1, b));
				}
			}
		else
			{
			if (seg & KBoxLeft)  aGc.DrawLine(TPoint(x0, cy), TPoint(cx + 1, cy));
			if (seg & KBoxRight) aGc.DrawLine(TPoint(cx, cy), TPoint(x1, cy));
			if (seg & KBoxUp)    aGc.DrawLine(TPoint(cx, y0), TPoint(cx, cy + 1));
			if (seg & KBoxDown)  aGc.DrawLine(TPoint(cx, cy), TPoint(cx, y1));
			}
		aGc.SetPenSize(TSize(1, 1));
		return;
		}
	if (ch >= 0x2580 && ch <= 0x259F)
		{
		TRect r = box;
		TInt level = fg;
		switch (ch)
			{
		case 0x2580: r.iBr.iY = box.iTl.iY + iCellH / 2; break;
		case 0x2584: r.iTl.iY = box.iTl.iY + iCellH / 2; break;
		case 0x258C: r.iBr.iX = box.iTl.iX + iCellW / 2; break;
		case 0x2590: r.iTl.iX = box.iTl.iX + iCellW / 2; break;
		case 0x2591: level = (fg * 1 + bg * 3) / 4; break;
		case 0x2592: level = (fg + bg) / 2; break;
		case 0x2593: level = (fg * 3 + bg * 1) / 4; break;
		case 0x2594: r.iBr.iY = box.iTl.iY + (iCellH + 7) / 8; break;
		case 0x2595: r.iTl.iX = box.iBr.iX - (iCellW + 7) / 8; break;
		default:
			if (ch >= 0x2596)
				{
				static const TUint8 KQuad[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
				TInt q = KQuad[ch - 0x2596];
				TInt mx = box.iTl.iX + iCellW / 2, my = box.iTl.iY + iCellH / 2;
				aGc.SetBrushColor(Grey(level));
				if (q & 1) aGc.DrawRect(TRect(box.iTl.iX, box.iTl.iY, mx, my));
				if (q & 2) aGc.DrawRect(TRect(mx, box.iTl.iY, box.iBr.iX, my));
				if (q & 4) aGc.DrawRect(TRect(box.iTl.iX, my, mx, box.iBr.iY));
				if (q & 8) aGc.DrawRect(TRect(mx, my, box.iBr.iX, box.iBr.iY));
				return;
				}
			if (ch >= 0x2581 && ch <= 0x2587)
				r.iTl.iY = box.iBr.iY - (iCellH * (TInt)(ch - 0x2580)) / 8;
			else if (ch >= 0x2589 && ch <= 0x258F)
				r.iBr.iX = box.iTl.iX + (iCellW * (TInt)(0x2590 - ch)) / 8;
			break;
			}
		aGc.SetBrushColor(Grey(level));
		aGc.DrawRect(r);
		return;
		}
	if (ch >= 0x2800 && ch <= 0x28FF)
		{
		TUint bits = ch - 0x2800;
		static const TInt8 dx[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
		static const TInt8 dy[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
		aGc.SetBrushColor(fgRgb);
		TInt stepX = iCellW / 2;
		TInt stepY = iCellH / 4;
		if (stepY < 1) stepY = 1;
		for (TInt i = 0; i < 8; i++)
			if (bits & (1 << i))
				{
				TInt px = box.iTl.iX + dx[i] * stepX + stepX / 2 - 1;
				TInt py = box.iTl.iY + dy[i] * stepY + stepY / 2;
				aGc.DrawRect(TRect(TPoint(px, py), TSize(2, 2)));
				}
		return;
		}
	TInt byte = PsiMapToCodePage(ch);
	if (byte == 0)
		return;
	if (byte < 0)
		byte = '?';
	TBuf<1> text;
	text.Append((TText)byte);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(fgRgb);
	TPoint base(box.iTl.iX, box.iTl.iY + iAscent);
	aGc.DrawText(text, base);
	if ((aLook.iFlags & KSbBold) || iSettings->iBold)
		aGc.DrawText(text, base + TPoint(1, 0));
	if (aLook.iFlags & KSbUnderline)
		aGc.DrawLine(TPoint(box.iTl.iX, box.iBr.iY - 1), TPoint(box.iBr.iX, box.iBr.iY - 1));
	if (aLook.iFlags & KSbStrike)
		aGc.DrawLine(TPoint(box.iTl.iX, box.iTl.iY + iCellH / 2), TPoint(box.iBr.iX, box.iTl.iY + iCellH / 2));
	}

TInt CLxTermView::Theme(TInt aGrey) const
	{
	if (aGrey < 0) aGrey = 0;
	if (aGrey > 15) aGrey = 15;
	switch (iSettings->iTheme)
		{
	case 1: return 15 - aGrey;
	case 2: return aGrey < 8 ? 0 : 15;
	case 3: return 2 + (aGrey * 11 + 7) / 15;
	default: return aGrey;
		}
	}

void CLxTermView::ThemePair(TInt aFg, TInt aBg, TInt& aThemedFg, TInt& aThemedBg) const
	{
	if (iSettings->iTheme == 2 && aFg != aBg)
		{
		aThemedFg = aFg < aBg ? 0 : 15;
		aThemedBg = aFg < aBg ? 15 : 0;
		return;
		}
	aThemedFg = Theme(aFg);
	aThemedBg = Theme(aBg);
	}

void CLxTermView::DrawCursor(CWindowGc& aGc) const
	{
	CLxTermView* self = CONST_CAST(CLxTermView*, this);
	self->iDrawnCurRow = iCurRow;
	self->iDrawnCurCol = iCurCol;
	self->iDrawnCurVisible = iCurVisible && !iBlinkHidden && iScrollOffset == 0;
	if (!iDrawnCurVisible || iCurRow >= iRows || iCurCol >= iCols)
		return;
	TRect box(TPoint(iOriginX + iCurCol * iCellW, RowY(iCurRow)), TSize(iCellW, iCellH));
	if (iSettings->iCursor == 1)
		box.iTl.iY = box.iBr.iY - (iCellH >= 12 ? 2 : 1) - iCellH / 12;
	else if (iSettings->iCursor == 2)
		box.iBr.iX = box.iTl.iX + 2;
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetDrawMode(CGraphicsContext::EDrawModeNOTSCREEN);
	aGc.DrawRect(box);
	aGc.SetDrawMode(CGraphicsContext::EDrawModePEN);
	}

// ----- the status line ---------------------------------------------------------
// What runs (the foreground program, how many programs, their memory) on the
// left; the time on the right.
void CLxTermView::StatusText(TDes& aText, TInt& aSplit) const
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
		}
	aSplit = aText.Length();
	TTime now;
	now.HomeTime();
	TDateTime t = now.DateTime();
	aText.AppendFormat(_L("%02d:%02d"), t.Hour(), t.Minute());
	}

void CLxTermView::DrawStatus(CWindowGc& aGc) const
	{
	if (!iStatusH)
		return;
	TBuf<120> text;
	TInt split;
	StatusText(text, split);
	CONST_CAST(CLxTermView*, this)->iStatusDrawn = text;
	const CFont* font = iEikonEnv->AnnotationFont();
	TRect bar(Rect().iTl.iX, Rect().iBr.iY - iStatusH, Rect().iBr.iX, Rect().iBr.iY);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(Grey(4));
	aGc.DrawRect(bar);
	aGc.UseFont(font);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(Grey(15));
	TInt y = bar.iTl.iY + 2 + font->AscentInPixels();
	TPtrC left(text.Left(split));
	TPtrC right(text.Mid(split));
	aGc.DrawText(left, TPoint(bar.iTl.iX + 6, y));
	aGc.DrawText(right, TPoint(bar.iBr.iX - 6 - font->TextWidthInPixels(right), y));
	aGc.DiscardFont();
	}

TInt CLxTermView::TickCallback(TAny* aSelf)
	{
	((CLxTermView*)aSelf)->Tick();
	return 1;
	}

void CLxTermView::Tick()
	{
	if (!IsActivated() || iPaintGc || !iForeground)
		return;
	if (iSettings->iBlink && iCurVisible && iScrollOffset == 0)
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

void CLxTermView::StartTick()
	{
	if (!iTick)
		iTick = CPeriodic::New(CActive::EPriorityLow);
	if (!iTick)
		return;
	iTick->Cancel();
	iBlinkHidden = EFalse;
	TInt us = iForeground ? KTickUs : 4 * KTickUs;
	iTick->Start(us, us, TCallBack(TickCallback, this));
	}

void CLxTermView::SetForeground(TBool aForeground)
	{
	if (aForeground == iForeground)
		return;
	iForeground = aForeground;
	StartTick();
	if (iForeground)
		Tick();
	}

// ----- keys ----------------------------------------------------------------------

void CLxTermView::SendKey(VTermKey aKey, TInt aMod)
	{
	vterm_keyboard_key(iVt, aKey, (VTermModifier)aMod);
	}

void CLxTermView::SendChar(TUint aChar)
	{
	vterm_keyboard_unichar(iVt, aChar, VTERM_MOD_NONE);
	}

void CLxTermView::SendCtrl(TUint aLetter)
	{
	TBuf8<1> b;
	b.Append((TUint8)(aLetter & 0x1f));
	SendBytes(b);
	}

void CLxTermView::ResetTerminal()
	{
	SbClear(&iSb);
	iScrollOffset = 0;
	iSelActive = EFalse;
	vterm_screen_reset(iScreen, 1);
	vterm_screen_flush_damage(iScreen);
	DrawNow();
	}

TKeyResponse CLxTermView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasNotConsumed;
	TUint code = aKeyEvent.iCode;
	TUint mods = aKeyEvent.iModifiers;
	if (code == EKeyMenu)
		return EKeyWasNotConsumed;
	if (iEnded || !iSession->Running())
		{
		// nothing running: Enter starts Linux
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

// ----- history, the pen, the clipboard (PsiTerm's) --------------------------------

void CLxTermView::ScrollBy(TInt aLines)
	{
	ScrollTo(iScrollOffset + aLines);
	}

void CLxTermView::ScrollTo(TInt aOffset)
	{
	if (aOffset > iSb.iCount)
		aOffset = iSb.iCount;
	if (aOffset < 0)
		aOffset = 0;
	if (aOffset == iScrollOffset)
		return;
	iScrollOffset = aOffset;
	iCacheValid = EFalse;
	DrawNow();
	}

void CLxTermView::RepaintRows(TInt aRow0, TInt aRow1)
	{
	if (aRow0 > aRow1)
		{
		TInt t = aRow0; aRow0 = aRow1; aRow1 = t;
		}
	if (aRow0 < 0) aRow0 = 0;
	if (aRow1 >= iRows) aRow1 = iRows - 1;
	if (aRow0 > aRow1 || !IsActivated())
		return;
	BeginPaint();
	DrawCells(*iPaintGc, aRow0, 0, aRow1 + 1, iCols);
	DrawScrollIndicator(*iPaintGc);
	EndPaint();
	}

void CLxTermView::ClearSelection()
	{
	if (!iSelActive)
		return;
	iSelActive = EFalse;
	TInt l0 = iSelLine0, c0 = iSelCol0, l1 = iSelLine1, c1 = iSelCol1;
	SbOrder(&l0, &c0, &l1, &c1);
	RepaintRows(l0 - ViewLine(0), l1 - ViewLine(0));
	}

void CLxTermView::SelectScreen()
	{
	ClearSelection();
	iSelLine0 = ViewLine(0);
	iSelCol0 = 0;
	iSelLine1 = ViewLine(iRows - 1);
	iSelCol1 = iCols - 1;
	iSelActive = ETrue;
	DrawNow();
	}

void CLxTermView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	TPoint p = aEvent.iPosition;
	TInt col = (p.iX - iOriginX) / iCellW;
	TInt row = (p.iY - iOriginY) / iCellH;
	if (p.iY < iOriginY) row = -1;
	if (col < 0) col = 0;
	if (col >= iCols) col = iCols - 1;
	TInt rowC = row < 0 ? 0 : (row >= iRows ? iRows - 1 : row);
	// a program that asked for the mouse (vim, less, htop...): a tap is a
	// click, a drag is the wheel. Shift+pen still selects text.
	TBool mouse = (iMouseMode != VTERM_PROP_MOUSE_NONE) && iSession->Running() && iScrollOffset == 0
		&& !(aEvent.iModifiers & EModifierShift);
	if (aEvent.iType == TPointerEvent::EButton1Down && mouse && row >= 0 && row < iRows)
		{
		ClearSelection();
		iPenMode = EPenMouse;
		iPenY0 = p.iY;
		iPenRow0 = row;
		iPenCol0 = col;
		iPenWheeled = EFalse;
		return;
		}
	if (iPenMode == EPenMouse)
		{
		if (aEvent.iType == TPointerEvent::EDrag)
			{
			TInt steps = (p.iY - iPenY0) / iCellH;
			if (steps != 0)
				{
				vterm_mouse_move(iVt, iPenRow0, iPenCol0, VTERM_MOD_NONE);
				for (TInt i = 0; i < (steps < 0 ? -steps : steps) && i < 20; i++)
					{
					vterm_mouse_button(iVt, steps > 0 ? 4 : 5, ETrue, VTERM_MOD_NONE);
					vterm_mouse_button(iVt, steps > 0 ? 4 : 5, EFalse, VTERM_MOD_NONE);
					}
				iPenY0 += steps * iCellH;
				iPenWheeled = ETrue;
				}
			}
		else if (aEvent.iType == TPointerEvent::EButton1Up)
			{
			if (!iPenWheeled)
				{
				vterm_mouse_move(iVt, iPenRow0, iPenCol0, VTERM_MOD_NONE);
				vterm_mouse_button(iVt, 1, ETrue, VTERM_MOD_NONE);
				vterm_mouse_button(iVt, 1, EFalse, VTERM_MOD_NONE);
				}
			iPenMode = EPenNone;
			}
		return;
		}
	switch (aEvent.iType)
		{
	case TPointerEvent::EButton1Down:
		ClearSelection();
		if (col >= iCols - 1 && iSb.iCount > 0)
			{
			iPenMode = EPenScroll;
			iPenY0 = p.iY;
			iPenOffset0 = iScrollOffset;
			}
		else
			{
			iPenMode = EPenSelect;
			iSelLine0 = iSelLine1 = ViewLine(rowC);
			iSelCol0 = iSelCol1 = col;
			}
		break;
	case TPointerEvent::EDrag:
		if (iPenMode == EPenScroll)
			ScrollTo(iPenOffset0 + (p.iY - iPenY0) / iCellH);
		else if (iPenMode == EPenSelect)
			{
			if (row < 0)
				ScrollBy(1);
			else if (row >= iRows)
				ScrollBy(-1);
			TInt line = ViewLine(rowC);
			if (line == iSelLine1 && col == iSelCol1 && iSelActive)
				break;
			TInt oldRow = iSelLine1 - ViewLine(0);
			iSelLine1 = line;
			iSelCol1 = col;
			TBool wasActive = iSelActive;
			iSelActive = (iSelLine1 != iSelLine0 || iSelCol1 != iSelCol0) || wasActive;
			if (iSelActive)
				RepaintRows(wasActive ? oldRow : iSelLine0 - ViewLine(0), rowC);
			}
		break;
	case TPointerEvent::EButton1Up:
		if (iPenMode == EPenSelect && iSelActive)
			iEikonEnv->InfoMsg(_L("Shift+Ctrl+C copies the selection"));
		iPenMode = EPenNone;
		break;
	default:
		break;
		}
	}

int CLxTermView::SelLineFn(void* aCtx, int aLine, unsigned int* aChars, int aMaxCols)
	{
	CLxTermView* self = (CLxTermView*)aCtx;
	if (aLine >= self->iLinesPushed)
		{
		TInt r = aLine - self->iLinesPushed;
		if (r >= self->iRows)
			return 0;
		TInt n = self->iCols < aMaxCols ? self->iCols : aMaxCols;
		for (TInt c = 0; c < n; c++)
			{
			VTermPos pos;
			pos.row = r;
			pos.col = c;
			VTermScreenCell cell;
			aChars[c] = vterm_screen_get_cell(self->iScreen, pos, &cell) ? cell.chars[0] : 0;
			}
		return n;
		}
	int cols;
	const TSbCell* sb = SbLine(&self->iSb, self->iLinesPushed - aLine, &cols);
	if (!sb)
		return 0;
	if (cols > aMaxCols)
		cols = aMaxCols;
	for (TInt c = 0; c < cols; c++)
		aChars[c] = (sb[c].iCh == 0xFFFF) ? (unsigned int)-1 : sb[c].iCh;
	return cols;
	}

int CLxTermView::SelMapFn(unsigned int aCh)
	{
	TInt seg = PsiBoxSegments(aCh);
	if (seg > 0)
		{
		TBool h = (seg & (KBoxLeft | KBoxRight)) != 0;
		TBool v = (seg & (KBoxUp | KBoxDown)) != 0;
		return (h && v) ? '+' : (h ? '-' : '|');
		}
	if (aCh >= 0x2580 && aCh <= 0x259F)
		return '#';
	if (aCh >= 0x2800 && aCh <= 0x28FF)
		return '.';
	TInt b = PsiMapToCodePage(aCh);
	return b < 0 ? '?' : b;
	}

void CLxTermView::CopySelectionL()
	{
	if (!iSelActive)
		{
		iEikonEnv->InfoMsg(_L("Nothing to copy - drag the pen over some text first"));
		return;
		}
	HBufC8* buf = HBufC8::NewLC(KClipMax);
	TPtr8 p(buf->Des());
	TInt n = SbSelectionText(&CLxTermView::SelLineFn, this, &CLxTermView::SelMapFn,
		iSelLine0, iSelCol0, iSelLine1, iSelCol1,
		(unsigned char*)p.Ptr(), KClipMax, CEditableText::EParagraphDelimiter);
	p.SetLength(n);
	CClipboard* cb = CClipboard::NewForWritingLC(iCoeEnv->FsSession());
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	text->InsertL(0, p);
	text->CopyToStoreL(cb->Store(), cb->StreamDictionary(), 0, text->DocumentLength());
	cb->CommitL();
	CleanupStack::PopAndDestroy(3);
	TBuf<40> msg;
	if (n == 1)
		msg.Copy(_L("Copied 1 character"));
	else
		msg.Format(_L("Copied %d characters"), n);
	iEikonEnv->InfoMsg(msg);
	}

// Types the clipboard (as a bracketed paste when the program asked for one)
void CLxTermView::PasteL()
	{
	CClipboard* cb = NULL;
	TRAPD(err, cb = CClipboard::NewForReadingL(iCoeEnv->FsSession()));
	if (err != KErrNone || !cb)
		{
		iEikonEnv->InfoMsg(_L("Nothing to paste"));
		return;
		}
	CleanupStack::PushL(cb);
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	text->PasteFromStoreL(cb->Store(), cb->StreamDictionary(), 0);
	TInt len = text->DocumentLength();
	if (len > KClipMax)
		len = KClipMax;
	HBufC* buf = HBufC::NewLC(len);
	TPtr p(buf->Des());
	text->Extract(p, 0, len);
	if (iScrollOffset > 0)
		ScrollTo(0);
	if (len == 0)
		iEikonEnv->InfoMsg(_L("Nothing to paste"));
	vterm_keyboard_start_paste(iVt);
	for (TInt i = 0; i < p.Length(); i++)
		{
		TUint c = p[i];
		if (c == CEditableText::EParagraphDelimiter || c == CEditableText::ELineBreak)
			SendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
		else if (c == CEditableText::ETabCharacter)
			SendKey(VTERM_KEY_TAB, VTERM_MOD_NONE);
		else if (c == CEditableText::ENonBreakingSpace)
			SendChar(' ');
		else if (c >= 0x20)
			SendChar(PsiCodePageToUnicode(c));
		}
	vterm_keyboard_end_paste(iVt);
	CleanupStack::PopAndDestroy(3);
	}
