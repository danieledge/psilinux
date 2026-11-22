// psilinux.cpp - PsiLinux: Linux programs on the Psion Series 5mx, Series 7
// and netBook.
//
// The program's window holds one view at a time: the terminal (lxterm.cpp),
// or the Linux screen (lxscreen.cpp: what programs draw on /dev/fb0, X
// among them), which takes the whole screen. A program opening the screen
// brings it to the front; Ctrl+Q and View > Switch view change between
// them. The Linux programs themselves run in psilxrun.exe (linux/runner).
#include <eikenv.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikappui.h>
#include <eikmenup.h>
#include <eikmenu.hrh>
#include <eikdialg.h>
#include <eikchlst.h>
#include <eikchkbx.h>
#include <eikcmds.hrh>
#include <s32file.h>
#include <eiktbar.h>
#include <eikcmbut.h>
#include <eiklabel.h>
#include <eikfnlab.h>
#include <eikproc.h>
#include <eikimage.h>
#include <badesca.h>
#include <e32hal.h>
#include "psisafe.h"                 // settings written beside, then put in place
#include "lxterm.h"
#include "lxscreen.h"
#include "lxhelp.h"
#include "lxicons.h"
#include "psilinux.hrh"
#include <psilinux.rsg>

const TUid KUidPsiLinux = { 0x01000A87 };
_LIT(KLxVersion, "0.46");
_LIT(KIniFile, "C:\\System\\Apps\\PsiLinux\\PsiLinux.ini");

// (0.38) Short of memory: the launcher (PsiHub, which watches the free RAM)
// sends "trim" under 3 MB free and "sleep" under 1.5 MB to programs behind,
// as window-group messages (psion-core ssh/psimem.h; 0.46: its words, and
// PsiLinux on the service's list, lxterm.cpp)
#include "psimem.h"

// (0.43) a zoom level's name as Preferences and the zoom's infoprint give it:
// its size in pixels, or "Smallest" for the ROM's Courier small (level 0)
static void LxSizeName(TInt aFace, TInt aZoom, TDes& aName)
	{
	if (aZoom <= 0 || aZoom >= CTermView::ZoomLevels(aFace))
		aName.Copy(_L("Smallest"));
	else
		aName.Num(CTermView::ZoomPixels(aFace, aZoom));
	}

// ---------------------------------------------------------------------------
// Preferences… and Linux disk…
// ---------------------------------------------------------------------------
class CLxPrefsDialog : public CEikDialog
	{
public:
	CLxPrefsDialog(TLxSettings& aSettings) : iSettings(aSettings) {}
private:
	// (0.29) PsiTerm's typefaces and their sizes: the names from its zoom
	// levels ("Terminus 12"), the typefaces until they repeat the first
	TInt Faces() const
		{
		TBuf<40> first, name;
		CTermView::ZoomName(0, 1, first);
		TInt n = 1;
		for (; n < 16; n++)
			{
			CTermView::ZoomName(n, 1, name);
			if (name == first)
				break;
			}
		return n;
		}
	static void FaceName(TInt aFace, TDes& aName)
		{
		CTermView::ZoomName(aFace, 1, aName);
		TInt sp = aName.LocateReverse(' ');
		if (sp > 0)
			aName.SetLength(sp);
		}
	void FillSizesL(TInt aFace, TInt aZoom)
		{
		CDesCArrayFlat* a = new(ELeave) CDesCArrayFlat(8);
		CleanupStack::PushL(a);
		TInt n = CTermView::ZoomLevels(aFace);
		for (TInt z = 0; z < n; z++)
			{
			// (0.43) the sizes alone ("12"), the typeface being the line above;
			// level 0 is the ROM's Courier small, the smallest of all
			TBuf<40> name;
			LxSizeName(aFace, z, name);
			a->AppendL(name);
			}
		CEikChoiceList* c = (CEikChoiceList*)Control(ELxPrefZoom);
		c->SetArrayL(a);
		CleanupStack::Pop();             // (the choice list has it)
		c->SetCurrentItem(aZoom >= 0 && aZoom < n ? aZoom : (n > 1 ? 1 : 0));
		c->DrawNow();
		}
	void PreLayoutDynInitL()
		{
		((CEikChoiceList*)Control(ELxPrefStart))->SetCurrentItem(iSettings.iStart);
		CDesCArrayFlat* faces = new(ELeave) CDesCArrayFlat(4);
		CleanupStack::PushL(faces);
		TInt nf = Faces();
		for (TInt f = 0; f < nf; f++)
			{
			TBuf<40> name;
			FaceName(f, name);
			faces->AppendL(name);
			}
		((CEikChoiceList*)Control(ELxPrefFace))->SetArrayL(faces);
		CleanupStack::Pop();
		TInt face = iSettings.iSpare[KLxSpareFace];
		if (face < 0 || face >= nf)
			face = 0;
		((CEikChoiceList*)Control(ELxPrefFace))->SetCurrentItem(face);
		FillSizesL(face, iSettings.iZoom);
		// the theme names as PsiTerm has them: on a colour screen its colour
		// schemes as well
		TInt theme = iSettings.iTheme;
		if (iEikonEnv->ScreenDevice()->DisplayMode() >= EColor16)
			((CEikChoiceList*)Control(ELxPrefTheme))->SetArrayL(iEikonEnv->ReadDesCArrayResourceL(R_LX_PT_THEME_COLOUR_ARRAY));
		else if (theme > 3)
			theme = 0;
		((CEikChoiceList*)Control(ELxPrefTheme))->SetCurrentItem(theme);
		((CEikChoiceList*)Control(ELxPrefCursor))->SetCurrentItem(iSettings.iCursor);
		SetCheckBoxState(ELxPrefBlink, iSettings.iBlink ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		SetCheckBoxState(ELxPrefStatus, iSettings.iStatus ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		SetCheckBoxState(ELxPrefBold, iSettings.iBold ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		SetCheckBoxState(ELxPrefKeep, iSettings.iKeep ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		TInt kb = iSettings.iKeepKb;
		((CEikChoiceList*)Control(ELxPrefKeepKb))->SetCurrentItem(kb == 512 ? 0 : kb == 2048 ? 2 : 1);
		SetLineDimmedNow(ELxPrefKeepKb, !iSettings.iKeep);
		((CEikChoiceList*)Control(ELxPrefSleep))->SetCurrentItem(LxSleepChoice(iSettings));
		}
	// the amount is dimmed while the tick box is clear; the sizes follow the typeface
	void HandleControlStateChangeL(TInt aControlId)
		{
		if (aControlId == ELxPrefKeep)
			SetLineDimmedNow(ELxPrefKeepKb, CheckBoxState(ELxPrefKeep) != CEikButtonBase::ESet);
		if (aControlId == ELxPrefFace)
			{
			TInt face = ((CEikChoiceList*)Control(ELxPrefFace))->CurrentItem();
			TInt zoom = ((CEikChoiceList*)Control(ELxPrefZoom))->CurrentItem();
			FillSizesL(face, zoom);
			}
		}
	TBool OkToExitL(TInt)
		{
		iSettings.iStart = ((CEikChoiceList*)Control(ELxPrefStart))->CurrentItem();
		iSettings.iSpare[KLxSpareFace] = ((CEikChoiceList*)Control(ELxPrefFace))->CurrentItem();
		iSettings.iZoom = ((CEikChoiceList*)Control(ELxPrefZoom))->CurrentItem();
		iSettings.iTheme = ((CEikChoiceList*)Control(ELxPrefTheme))->CurrentItem();
		iSettings.iCursor = ((CEikChoiceList*)Control(ELxPrefCursor))->CurrentItem();
		iSettings.iBlink = CheckBoxState(ELxPrefBlink) == CEikButtonBase::ESet;
		iSettings.iStatus = CheckBoxState(ELxPrefStatus) == CEikButtonBase::ESet;
		iSettings.iBold = CheckBoxState(ELxPrefBold) == CEikButtonBase::ESet;
		iSettings.iKeep = CheckBoxState(ELxPrefKeep) == CEikButtonBase::ESet;
		static const TInt KKeepKb[] = { 512, 1024, 2048 };
		iSettings.iKeepKb = KKeepKb[((CEikChoiceList*)Control(ELxPrefKeepKb))->CurrentItem()];
		LxSetSleepChoice(iSettings, ((CEikChoiceList*)Control(ELxPrefSleep))->CurrentItem());
		return ETrue;
		}
	TLxSettings& iSettings;
	};

class CLxDiskDialog : public CEikDialog
	{
public:
	CLxDiskDialog(TInt& aDisk) : iDisk(aDisk) {}
private:
	void PreLayoutDynInitL()
		{
		((CEikChoiceList*)Control(ELxDiskChoice))->SetCurrentItem(iDisk == 'C' ? 1 : 0);
		}
	TBool OkToExitL(TInt)
		{
		iDisk = ((CEikChoiceList*)Control(ELxDiskChoice))->CurrentItem() == 1 ? 'C' : 'D';
		return ETrue;
		}
	TInt& iDisk;
	};

// ---------------------------------------------------------------------------
// The app UI
// ---------------------------------------------------------------------------
// (0.46) The Linux screen's keys, before EIKON's menu bar (the Menu key and
// Ctrl+letters are Linux's there). This filter is on the control stack at
// menu priority, not the screen itself, and it is never visible: a visible
// control there makes CCoeAppUi::IsDisplayingMenuOrDialog() true, and
// EIKON's shutter (TApaTask::EndTask: Free memory's Close, the task list's
// Close, a backup) then only sends Esc and never closes PsiLinux (as Files
// before psion-netbook faebd9c). The stack ignores visibility.
class CLxScreenKeys : public CCoeControl
	{
public:
	CLxScreenKeys(CLxScreenView*& aScreen) : iScreen(aScreen) {}
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
		{
		return iScreen ? iScreen->OfferKeyEventL(aKeyEvent, aType) : EKeyWasNotConsumed;
		}
private:
	CLxScreenView*& iScreen;
	};

class CLxAppUi : public CEikAppUi, public MLxScreenHook
	{
public:
	void ConstructL();
	~CLxAppUi();
	// (0.26) a command tail of "shell" (PsiHub's Terminal): this time Alpine's
	// shell only, whatever Preferences > Start says
	TBool ProcessCommandParametersL(TApaCommand aCommand, TFileName& aDocumentName, const TDesC8& aTail);
private:
	static TInt StartNow(TAny* aSelf);
	CIdle* iStarter;              // (0.26) the first start, after the command line is read
	TBool iShellOnce;
	void HandleCommandL(TInt aCommand);
	void DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void HandleWsEventL(const TWsEvent& aEvent, CCoeControl* aDestination);
	void ProcessMessageL(TUid aUid, const TDesC8& aParams);
	void LoadSettings();
	void SaveSettings();
	void SwitchToL(TInt aView);
	void HelpL();
	void AboutL();
	void ButtonPictureL(CEikToolBar* aBar, TInt aId, TInt aIcon);
	void MakeNetBookBarL();
	CEikToolBar* Bar() const { return iNbBar ? iNbBar : iToolBar; }
	void ShowToolBarL(TBool aShow);
	void ZoomL(TInt aStep);
	TRect ViewRect(TBool aToolbar) const;
	// MLxScreenHook
	void ScreenOnOff(TBool aOn);
	void ScreenPump();
	static TInt WakeRunner(TAny* aSelf) { ((CLxAppUi*)aSelf)->iTerm->Session()->Ring(); return 0; }
	CLxTermView* iTerm;
	CLxScreenView* iScreen;
	CLxScreenKeys* iScreenKeys;   // (0.46) its keys, on the stack while it shows
	TInt iView;                   // 0 the terminal, 1 the Linux screen
	TLxSettings iSettings;
	CEikToolBar* iNbBar;          // (0.43) the netBook's toolbar, on Coda's image (MakeNetBookBarL)
	};

void CLxAppUi::LoadSettings()
	{
	Mem::FillZ(&iSettings, sizeof(iSettings));
	iSettings.iVersion = 1;
	iSettings.iZoom = 1;
	iSettings.iStatus = 1;
	iSettings.iBlink = 1;
	iSettings.iStart = 1;      // (0.30) Alpine's shell only at first (Dan): Preferences > Start offers the others
	iSettings.iToolbar = 0;    // (0.30) the toolbar hidden at first (Dan): Shift+Ctrl+B shows it
	// (0.29) a colour screen starts in PsiTerm's colour default (Clean); a
	// saved choice stays
	if (iEikonEnv->ScreenDevice()->DisplayMode() >= EColor16)
		iSettings.iTheme = KLxThemeColourDefault;
	RFile f;
	if (f.Open(iCoeEnv->FsSession(), KIniFile, EFileRead) == KErrNone)
		{
		TPckg<TLxSettings> p(iSettings);
		TLxSettings saved;
		TPckg<TLxSettings> q(saved);
		if (f.Read(q) == KErrNone && q.Length() == (TInt)sizeof(saved) && saved.iVersion == 1)
			iSettings = saved;
		f.Close();
		(void)p;
		}
	}

void CLxAppUi::SaveSettings()
	{
	// written beside it (PsiLinux.ini~), then put in its place: a full disk
	// or a reset part way leaves the last settings as they were
	PsiSafeWrite(iCoeEnv->FsSession(), KIniFile, TPckgC<TLxSettings>(iSettings));
	}

void CLxAppUi::ConstructL()
	{
	// (0.38) the free RAM before PsiLinux takes any, for /proc/psilinux/mem
	TMemoryInfoV1Buf mi;
	UserHal::MemoryInfo(mi);
	BaseConstructL();
	LoadSettings();
	iEikonEnv->Process()->SetMainDocFileName(_L("PsiLinux"));   // (the toolbar's name box)
	if (iToolBar)
		{
		TRAPD(err, MakeNetBookBarL());
		if (err != KErrNone)
			{
			delete iNbBar;
			iNbBar = NULL;
			}
		if (iNbBar)
			{
			iToolBar->MakeVisible(EFalse);       // (the 5mx's, kept for EIKON, never shown)
			TRAP(err, ButtonPictureL(iNbBar, ELxCmdTbStop, ELxMbmToolStopC); ButtonPictureL(iNbBar, ELxCmdTbKeys, ELxMbmToolKeysC);
				ButtonPictureL(iNbBar, ELxCmdTbCopy, ELxMbmToolCopyC); ButtonPictureL(iNbBar, ELxCmdTbPaste, ELxMbmToolPasteC));
			}
		else
			TRAP(err, ButtonPictureL(iToolBar, ELxCmdTbStop, ELxMbmToolStop); ButtonPictureL(iToolBar, ELxCmdTbKeys, ELxMbmToolKeys);
				ButtonPictureL(iToolBar, ELxCmdTbCopy, ELxMbmToolCopy); ButtonPictureL(iToolBar, ELxCmdTbPaste, ELxMbmToolPaste));
		CEikFileNameLabel* label = (CEikFileNameLabel*)Bar()->ControlById(ELxCmdFileName);
		if (label)
			TRAP(err, label->UpdateL());
		if (iNbBar && iSettings.iToolbar)
			iNbBar->MakeVisible(ETrue);
		if (!iSettings.iToolbar)
			Bar()->MakeVisible(EFalse);          // (remembered from last time)
		}
	iTerm = new(ELeave) CLxTermView;
	iTerm->ConstructL(ViewRect(iSettings.iToolbar), iSettings);
	AddToStackL(iTerm);
	iTerm->Session()->SetScreenHook(this);
	iTerm->Session()->SetStartFree(mi().iFreeRamInBytes);
	iTerm->Session()->MemRegister();        // (0.46) on the memory service's list (lxterm.cpp)
	// (0.26) Linux starts once the program is up, not here: the command
	// line (ProcessCommandParametersL) may still be to come
	iStarter = CIdle::NewL(CActive::EPriorityStandard);
	iStarter->Start(TCallBack(StartNow, this));
	}

TInt CLxAppUi::StartNow(TAny* aSelf)
	{
	CLxAppUi* self = (CLxAppUi*)aSelf;
	if (self->iShellOnce)
		self->iTerm->SetStartOnce(1);
	TRAPD(err, self->iTerm->StartLinuxL());
	(void)err;
	return EFalse;                         // (once)
	}

// (0.38) "trim" and "sleep" from the launcher: Linux keeps running (its
// programs' memory cannot be made again), and the runner gives back what can
// be: the program cache, unused shared images, the translator's code and
// its heap's free end; the window gives back its heap's free end.
// (0.46) PsiHub sends them only to the programs on its list, which PsiLinux
// now joins at its start (CLxSession::MemRegister); and "sleepnow", Free
// memory's "Put PsiLinux to sleep", sleeps Linux if it is idle whatever In
// the background says (the user asked), and is answered.
void CLxAppUi::ProcessMessageL(TUid aUid, const TDesC8& aParams)
	{
	TPsiMemWord word = PsiMemWordOf(aUid, aParams);
	if (aUid.iUid == KPsiMemMsgUid && word == EPsiMemWordNone)
		return;                              // (a word of the service's not for programs: never a file to open)
	if (word == EPsiMemWordNone)
		{
		CEikAppUi::ProcessMessageL(aUid, aParams);
		return;
		}
	iTerm->Session()->Trim();
	// (0.39) "sleep": Linux sleeps now, if it is idle (In the background:
	// Never sleep leaves it awake)
	if (word == EPsiMemWordSleep && iTerm->SleepAfter())
		iTerm->Session()->SleepNow(ETrue);
	if (word == EPsiMemWordSleepNow && !iTerm->Session()->SleepNowAsked())
		PsiMemAnswer(iCoeEnv->WsSession(), KUidPsiLinux.iUid, KPsiMemCannot, _L("Linux is not running"));
	}

TBool CLxAppUi::ProcessCommandParametersL(TApaCommand aCommand, TFileName& aDocumentName, const TDesC8& aTail)
	{
	if (aTail.CompareF(_L8("shell")) == 0)
		iShellOnce = ETrue;
	return CEikAppUi::ProcessCommandParametersL(aCommand, aDocumentName, aTail);
	}

CLxAppUi::~CLxAppUi()
	{
	delete iNbBar;
	delete iStarter;
	SaveSettings();
	if (iTerm)
		iTerm->Session()->SetScreenHook(NULL);
	if (iScreenKeys)
		{
		if (iView == 1)
			RemoveFromStack(iScreenKeys);
		delete iScreenKeys;
		}
	delete iScreen;
	if (iTerm)
		{
		if (iView == 0)
			RemoveFromStack(iTerm);
		delete iTerm;
		}
	}

// (0.43) On Coda's netBook image the toolbar is as Coda makes Psion's own
// programs' (r_lx_toolbar_nb): taller buttons with colour pictures, and Coda's
// battery, gauges and clock, which only Coda's Eikon can make (the hook in
// PsiBatt.dll's image). Coda is told by Z:\System\Libs\PsiBattHook.bin,
// which the image has only together with that hook (as PsiPDF and
// psion-core's psitbar.h). On a 240-line screen (the 5mx) or another image
// the standard toolbar stays.
void CLxAppUi::MakeNetBookBarL()
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TEntry e;
	if (screen.iHeight < 400 || iCoeEnv->FsSession().Entry(_L("Z:\\System\\Libs\\PsiBattHook.bin"), e) != KErrNone)
		return;
	CEikToolBar* bar = new(ELeave) CEikToolBar;
	CleanupStack::PushL(bar);
	bar->ConstructL(this, R_LX_TOOLBAR_NB, TRect(TPoint(0, 0), screen));
	CleanupStack::Pop();
	bar->MakeVisible(EFalse);
	iNbBar = bar;
	}

// a toolbar button's picture: 24x20, from PsiLinux.mbm, the words beside it
void CLxAppUi::ButtonPictureL(CEikToolBar* aBar, TInt aId, TInt aIcon)
	{
	CEikCommandButton* b = aBar ? (CEikCommandButton*)aBar->ControlById(aId) : NULL;
	if (!b)
		return;
	TFileName mbm = Application()->BitmapStoreName();
	CFbsBitmap* bmp = iEikonEnv->CreateBitmapL(mbm, aIcon);
	CleanupStack::PushL(bmp);
	CFbsBitmap* mask = iEikonEnv->CreateBitmapL(mbm, aIcon + 1);
	CleanupStack::PushL(mask);
	b->SetPictureL(bmp, mask);                // (the button owns them now)
	CleanupStack::Pop(2);
	if (b->Picture())
		b->Picture()->SetAlignment(EHCenterVCenter);
	if (b->Label())
		b->Label()->SetAlignment(EHLeftVCenter);
	b->LayoutComponentsL();
	}

// the view's room: up to the toolbar's left edge while it shows (EIKON's
// ClientRect() does not give its width back when it is hidden)
TRect CLxAppUi::ViewRect(TBool aToolbar) const
	{
	TRect r = ClientRect();
	r.iBr.iX = iEikonEnv->ScreenDevice()->SizeInPixels().iWidth;
	if (aToolbar && Bar() && Bar()->Position().iX > r.iTl.iX)
		r.iBr.iX = Bar()->Position().iX;
	return r;
	}

// View > Show toolbar (Shift+Ctrl+B): the terminal takes its room or gives
// it back, and Linux's programs see the new width (SIGWINCH)
void CLxAppUi::ShowToolBarL(TBool aShow)
	{
	CEikToolBar* bar = Bar();
	if (!bar)
		return;
	if (bar->IsVisible() != aShow)
		bar->MakeVisible(aShow);
	if (aShow)
		{
		bar->DrawNow();
		iCoeEnv->WsSession().Flush();
		}
	iTerm->SetTermRectL(ViewRect(aShow));
	iSettings.iToolbar = aShow;
	}

// in front or behind (HandleForegroundEventL is private on ER5): the
// terminal's tick slows down behind, where nothing is drawn
void CLxAppUi::HandleWsEventL(const TWsEvent& aEvent, CCoeControl* aDestination)
	{
	if ((aEvent.Type() == EEventFocusGained || aEvent.Type() == EEventFocusLost) && iTerm)
		{
		iTerm->SetForeground(aEvent.Type() == EEventFocusGained);
		if (aEvent.Type() == EEventFocusGained)
			iTerm->Session()->InFront();     // (0.39) Linux wakes now, if asleep
		if (aEvent.Type() == EEventFocusLost)
			User::Heap().Compress();         // (0.38) behind: the heap's free end back
		}
	CEikAppUi::HandleWsEventL(aEvent, aDestination);
	}

void CLxAppUi::SwitchToL(TInt aView)
	{
	if (aView == iView)
		return;
	if (aView == 1)
		{
		// the Linux screen: the Psion's whole screen, toolbar and all
		if (!iScreen || !iScreen->IsOpen())
			{
			iEikonEnv->InfoMsg(_L("No Linux program is using the screen"));
			return;
			}
		RemoveFromStack(iTerm);
		iTerm->MakeVisible(EFalse);
		if (Bar() && Bar()->IsVisible())
			Bar()->MakeVisible(EFalse);
		iScreen->MakeVisible(ETrue);
		// (before EIKON's menu bar: the Menu key and Ctrl+letters are Linux's
		// there; the view passes Ctrl+Q back for Switch view. 0.46: through
		// an invisible filter, CLxScreenKeys)
		if (!iScreenKeys)
			{
			iScreenKeys = new(ELeave) CLxScreenKeys(iScreen);
			iScreenKeys->MakeVisible(EFalse);
			}
		AddToStackL(iScreenKeys, ECoeStackPriorityMenu);
		iScreen->DrawNow();
		}
	else
		{
		if (iScreenKeys)
			RemoveFromStack(iScreenKeys);
		if (iScreen)
			iScreen->MakeVisible(EFalse);
		if (Bar() && iSettings.iToolbar)
			{
			Bar()->MakeVisible(ETrue);
			Bar()->DrawNow();
			}
		iTerm->MakeVisible(ETrue);
		AddToStackL(iTerm);
		iTerm->DrawNow();
		}
	iView = aView;
	iCoeEnv->WsSession().Flush();
	}

// a program opened /dev/fb0 (X, a game): the Linux screen comes to the front;
// the last one closed it: back to the terminal
void CLxAppUi::ScreenOnOff(TBool aOn)
	{
	if (aOn)
		{
		if (!iScreen)
			{
			CLxScreenView* v = new CLxScreenView;
			TInt err = KErrNoMemory;
			if (v)
				TRAP(err, v->ConstructL(TRect(TPoint(0, 0), iEikonEnv->ScreenDevice()->SizeInPixels())));
			if (err != KErrNone)
				{
				delete v;
				iEikonEnv->InfoMsg(_L("Not enough memory for the Linux screen"));
				return;
				}
			v->MakeVisible(EFalse);
			v->SetWake(TCallBack(WakeRunner, this));
			iScreen = v;
			}
		if (iScreen->Open() == KErrNone)
			{
			TRAPD(err, SwitchToL(1));
			}
		}
	else if (iScreen)
		{
		TRAPD(err, SwitchToL(0));
		iScreen->Close();
		// (0.44, MEM-101) and the view goes: its backed-up window and its
		// five strips (about 150 KB) stayed until PsiLinux ended. Made again
		// when a program next opens the screen. (Not while it is still the
		// view in front: the switch back failed.)
		if (iView != 1)
			{
			delete iScreen;
			iScreen = NULL;
			}
		}
	}

void CLxAppUi::ScreenPump()
	{
	if (iScreen)
		iScreen->Pump();
	}

void CLxAppUi::DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	TBool running = iTerm->Session()->Running();
	switch (aMenuId)
		{
	case R_LX_FILE_MENU:
		aMenuPane->SetItemDimmed(ELxCmdStart, running);
		aMenuPane->SetItemDimmed(ELxCmdEnd, !running);
		break;
	case R_LX_EDIT_MENU:
		aMenuPane->SetItemDimmed(ELxCmdCopy, !iTerm->HasSelection());
		break;
	case R_LX_VIEW_MENU:
		if (iSettings.iBold)
			aMenuPane->SetItemButtonState(ELxCmdBold, EEikMenuItemSymbolOn);
		if (iSettings.iStatus)
			aMenuPane->SetItemButtonState(ELxCmdStatusLine, EEikMenuItemSymbolOn);
		if (Bar() && Bar()->IsVisible())
			aMenuPane->SetItemButtonState(ELxCmdToolbar, EEikMenuItemSymbolOn);
		break;
	case R_LX_TROUBLE_MENU:
		if (iSettings.iSpare[KLxSpareLog])
			aMenuPane->SetItemButtonState(ELxCmdKeepLog, EEikMenuItemSymbolOn);
		break;
	case R_LX_TOOLS_MENU:
		if (!iSettings.iSpare[KLxSpareNoWelcome])
			aMenuPane->SetItemButtonState(ELxCmdWelcome, EEikMenuItemSymbolOn);
		break;
	case R_LX_VIEWS_MENU:
		aMenuPane->SetItemButtonState(iView == 0 ? ELxCmdViewTerminal : ELxCmdViewRemote, EEikMenuItemSymbolOn);
		break;
	default:
		break;
		}
	}

void CLxAppUi::HandleCommandL(TInt aCommand)
	{
	// (in the Linux screen, only the commands that make sense there)
	if (iView == 1 && aCommand != EEikCmdExit && aCommand != ELxCmdSwitchView && aCommand != ELxCmdViewTerminal &&
		aCommand != ELxCmdViewRemote && aCommand != ELxCmdHelp && aCommand != ELxCmdAbout && aCommand != ELxCmdPreferences &&
		aCommand != ELxCmdKeepLog && aCommand != ELxCmdWelcome)
		{
		iEikonEnv->InfoMsg(_L("Not available in this view"));
		return;
		}
	switch (aCommand)
		{
	case EEikCmdExit:
		if (iTerm->Session()->Running())
			iEikonEnv->BusyMsgL(_L("Ending Linux\x85"), EHLeftVBottom, TTimeIntervalMicroSeconds32(0));
		iTerm->Session()->Kill();
		SaveSettings();
		Exit();
		break;
	case ELxCmdStart:
		if (iTerm->Session()->Running())
			iEikonEnv->InfoMsg(_L("Linux is running already"));
		else
			iTerm->StartLinuxL();
		break;
	case ELxCmdEnd:
		if (!iTerm->Session()->Running())
			iEikonEnv->InfoMsg(_L("Linux is not running"));
		else if (iEikonEnv->QueryWinL(_L("Every Linux program will end"), _L("End Linux?")))
			iTerm->Session()->Stop();
		break;
	case ELxCmdTbCopy:
	case ELxCmdCopy: iTerm->CopySelectionL(); break;
	case ELxCmdTbPaste:
	case ELxCmdPaste: iTerm->PasteL(); break;
	case ELxCmdTbStop:
		if (iTerm->Session()->Running())
			iTerm->SendCtrl('c');
		else
			iEikonEnv->InfoMsg(_L("Linux is not running"));
		break;
	case ELxCmdToolbar: ShowToolBarL(!(Bar() && Bar()->IsVisible())); SaveSettings(); break;
	case ELxCmdSelectScreen: iTerm->SelectScreen(); break;
	case EEikCmdZoomIn:
	case ELxCmdZoomIn: ZoomL(1); break;
	case EEikCmdZoomOut:
	case ELxCmdZoomOut: ZoomL(-1); break;
	case ELxCmdBold:
		iSettings.iBold = !iSettings.iBold;
		iTerm->ApplySettingsL();
		break;
	case ELxCmdStatusLine:
		iSettings.iStatus = !iSettings.iStatus;
		iTerm->ApplySettingsL();
		break;
	case ELxCmdSwitchView: SwitchToL(iView ? 0 : 1); break;
	case ELxCmdViewTerminal: SwitchToL(0); break;
	case ELxCmdViewRemote: SwitchToL(1); break;
	case ELxCmdKeyCtrlC: iTerm->SendCtrl('c'); break;
	case ELxCmdKeyCtrlZ: iTerm->SendCtrl('z'); break;
	case ELxCmdKeyCtrlD: iTerm->SendCtrl('d'); break;
	case ELxCmdKeyCtrlBackslash: iTerm->SendCtrl('\\'); break;
	case ELxCmdKeyEsc: iTerm->SendKey(VTERM_KEY_ESCAPE, VTERM_MOD_NONE); break;
	case ELxCmdKeyTab: iTerm->SendKey(VTERM_KEY_TAB, VTERM_MOD_NONE); break;
	case ELxCmdKeyBar: iTerm->SendChar('|'); break;
	case ELxCmdKeyBacktick: iTerm->SendChar('`'); break;
	case ELxCmdKeyTilde: iTerm->SendChar('~'); break;
	case ELxCmdKeyF1: case ELxCmdKeyF2: case ELxCmdKeyF3: case ELxCmdKeyF4: case ELxCmdKeyF5:
	case ELxCmdKeyF6: case ELxCmdKeyF7: case ELxCmdKeyF8: case ELxCmdKeyF9: case ELxCmdKeyF10:
		iTerm->SendKey((VTermKey)VTERM_KEY_FUNCTION(aCommand - ELxCmdKeyF1 + 1), VTERM_MOD_NONE);
		break;
	case ELxCmdScrollBack: iTerm->ScrollBy(iTerm->Rows() - 1); break;
	case ELxCmdScrollForward: iTerm->ScrollBy(-(iTerm->Rows() - 1)); break;
	case ELxCmdScrollLive: iTerm->ScrollTo(0); break;
	case ELxCmdPreferences:
		{
		TInt start = iSettings.iStart;
		CEikDialog* d = new(ELeave) CLxPrefsDialog(iSettings);
		if (d->ExecuteLD(R_LX_PREFS_DIALOG))
			{
			iTerm->ApplySettingsL();
			SaveSettings();
			if (iSettings.iStart != start && iTerm->Session()->Running())
				iEikonEnv->InfoMsg(_L("Starts this way when Linux next starts"));
			}
		break;
		}
	case ELxCmdDisk:
		{
		TInt disk = iSettings.iDisk;
		CEikDialog* d = new(ELeave) CLxDiskDialog(disk);
		if (d->ExecuteLD(R_LX_DISK_DIALOG))
			{
			iSettings.iDisk = disk;
			SaveSettings();
			iEikonEnv->InfoMsg(iTerm->Session()->Running() ? _L("Linux uses this disk when it next starts") : _L("Disk changed"));
			}
		break;
		}
	case ELxCmdReset: iTerm->ResetTerminal(); break;
	case ELxCmdKeepLog:
		// (0.24) off unless ticked; PsiLinux.log, if there is one, stays. The
		// runner reads it when it starts: Linux running now carries on as it began.
		iSettings.iSpare[KLxSpareLog] = !iSettings.iSpare[KLxSpareLog];
		SaveSettings();                     // (0.43: the tick says so: no infoprint)
		break;
	case ELxCmdWelcome:
		// (0.37) Alpine's banner at login (/etc/profile.d/psilinux-welcome.sh,
		// which reads /proc/psilinux/welcome): the next shell to start reads it
		iSettings.iSpare[KLxSpareNoWelcome] = !iSettings.iSpare[KLxSpareNoWelcome];
		SaveSettings();
		iTerm->Session()->SetWelcome(!iSettings.iSpare[KLxSpareNoWelcome]);   // (0.43: no infoprint)
		break;
	case ELxCmdHelp: HelpL(); break;
	case ELxCmdAbout: AboutL(); break;
	default:
		break;
		}
	}

// (0.43) a zoom step says the size it came to, as the guide asks (PsiTerm's
// own other-font sizes say theirs)
void CLxAppUi::ZoomL(TInt aStep)
	{
	TInt was = iSettings.iZoom;
	iTerm->ZoomBy(aStep);
	if (iSettings.iZoom == was)
		return;
	TBuf<40> name, msg;
	LxSizeName(iSettings.iSpare[KLxSpareFace], iSettings.iZoom, name);
	msg.Format(iSettings.iZoom <= 0 ? _L("Text size: smallest") : _L("Text size %S"), &name);
	iEikonEnv->InfoMsg(msg);
	}

void CLxAppUi::HelpL()
	{
	// (0.43) 12 whole lines of text on a 480-line screen, 7 on the 5mx's 240
	CEikDialog* d = new(ELeave) CLxHelpDialog(0);
	d->ExecuteLD(iEikonEnv->ScreenDevice()->SizeInPixels().iHeight >= 400 ? R_LX_HELP_DIALOG_TALL : R_LX_HELP_DIALOG);
	}

// the About box (Coda's layout): the version, Alpine's (the runner reads
// /etc/alpine-release) and the © signs (0xa9) put in here
static void LxCopyrightL(CEikLabel* aLabel)
	{
	if (!aLabel)
		return;
	TBuf<120> t(*aLabel->Text());
	TBuf<1> c;
	c.Append((TText)0xa9);
	for (TInt i = t.Find(_L("(c)")); i >= 0; i = t.Find(_L("(c)")))
		t.Replace(i, 3, c);
	aLabel->SetTextL(t);
	}

class CLxAboutDialog : public CEikDialog
	{
public:
	CLxAboutDialog(const TDesC& aAlpine) : iAlpine(aAlpine) {}
private:
	void PreLayoutDynInitL()
		{
		TBuf<40> v;
		v.Format(_L("PsiLinux %S"), &KLxVersion);
		CEikLabel* l = (CEikLabel*)Control(ELxAbout1);
		l->SetFont(iEikonEnv->TitleFont());
		l->SetTextL(v);
		TBuf<100> a;                         // (the version is up to 32 characters)
		if (iAlpine.Length())
			a.Format(_L("Alpine Linux %S (armhf), translated as it runs"), &iAlpine);
		else
			a.Copy(_L("Alpine Linux - its version shows once it has started"));
		((CEikLabel*)Control(ELxAbout5))->SetTextL(a);
		// "part of Coda" only on Coda's netBook (its image has PsiBattHook.bin),
		// as every bundled program's About (psion-core psiabout.h)
		TEntry e;
		if (iEikonEnv->FsSession().Entry(_L("Z:\\System\\Libs\\PsiBattHook.bin"), e) != KErrNone)
			((CEikLabel*)Control(ELxAbout3))->SetTextL(_L("(c) Dan Edge 2026"));
		LxCopyrightL((CEikLabel*)Control(ELxAbout3));
		LxCopyrightL((CEikLabel*)Control(ELxAbout7));
		}
	const TDesC& iAlpine;
	};

void CLxAppUi::AboutL()
	{
	TBuf<32> alpine;
	iTerm->AlpineRelease(alpine);
	CEikDialog* d = new(ELeave) CLxAboutDialog(alpine);
	d->ExecuteLD(R_LX_ABOUT_DIALOG);
	}

// ---------------------------------------------------------------------------

class CLxDocument : public CEikDocument
	{
public:
	CLxDocument(CEikApplication& aApp) : CEikDocument(aApp) {}
private:
	CEikAppUi* CreateAppUiL() { return new(ELeave) CLxAppUi; }
	};

class CLxApplication : public CEikApplication
	{
private:
	CApaDocument* CreateDocumentL() { return new(ELeave) CLxDocument(*this); }
	TUid AppDllUid() const { return KUidPsiLinux; }
	};

EXPORT_C CApaApplication* NewApplication()
	{
	return new CLxApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
