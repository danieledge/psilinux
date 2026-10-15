// psilinux.cpp - PsiLinux: Linux programs on the Psion Series 5mx.
//
// The program's window holds one view at a time: the terminal (lxterm.cpp),
// or "Remote apps" (a place for the remote GUI view, which is built
// separately: linux/remote). Ctrl+Q and View > Switch view change between
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
#include <eikimage.h>
#include "lxterm.h"
#include "lxhelp.h"
#include "lxicons.h"
#include "psilinux.hrh"
#include <psilinux.rsg>

const TUid KUidPsiLinux = { 0x01000A87 };
_LIT(KLxVersion, "0.21");
_LIT(KIniFile, "C:\\System\\Apps\\PsiLinux\\PsiLinux.ini");

// ---------------------------------------------------------------------------
// The Remote apps view: a place held for it. (The view itself is built
// elsewhere; until then this says so.)
// ---------------------------------------------------------------------------
class CLxRemoteView : public CCoeControl
	{
public:
	void ConstructL(const TRect& aRect)
		{
		CreateWindowL();
		SetRectL(aRect);
		ActivateL();
		}
private:
	void Draw(const TRect&) const
		{
		CWindowGc& gc = SystemGc();
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(KRgbWhite);
		gc.Clear(Rect());
		const CFont* f = iEikonEnv->TitleFont();
		gc.UseFont(f);
		gc.SetPenColor(KRgbBlack);
		TInt y = Rect().Height() / 2 - f->HeightInPixels();
		gc.DrawText(_L("Remote apps"), TRect(0, y, Rect().Width(), y + f->HeightInPixels() + 4),
			f->AscentInPixels(), CGraphicsContext::ECenter);
		gc.DiscardFont();
		const CFont* g = iEikonEnv->NormalFont();
		gc.UseFont(g);
		y += f->HeightInPixels() + 10;
		gc.DrawText(_L("Not available yet - Ctrl+Q goes back to the terminal"),
			TRect(0, y, Rect().Width(), y + g->HeightInPixels() + 4), g->AscentInPixels(), CGraphicsContext::ECenter);
		gc.DiscardFont();
		}
	};

// ---------------------------------------------------------------------------
// Preferences... and Linux disk...
// ---------------------------------------------------------------------------
class CLxPrefsDialog : public CEikDialog
	{
public:
	CLxPrefsDialog(TLxSettings& aSettings) : iSettings(aSettings) {}
private:
	void PreLayoutDynInitL()
		{
		((CEikChoiceList*)Control(ELxPrefStart))->SetCurrentItem(iSettings.iStart);
		((CEikChoiceList*)Control(ELxPrefZoom))->SetCurrentItem(iSettings.iZoom);
		((CEikChoiceList*)Control(ELxPrefTheme))->SetCurrentItem(iSettings.iTheme);
		((CEikChoiceList*)Control(ELxPrefCursor))->SetCurrentItem(iSettings.iCursor);
		SetCheckBoxState(ELxPrefBlink, iSettings.iBlink ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		SetCheckBoxState(ELxPrefStatus, iSettings.iStatus ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		SetCheckBoxState(ELxPrefBold, iSettings.iBold ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		SetCheckBoxState(ELxPrefKeep, iSettings.iKeep ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		TInt kb = iSettings.iKeepKb;
		((CEikChoiceList*)Control(ELxPrefKeepKb))->SetCurrentItem(kb == 512 ? 0 : kb == 2048 ? 2 : 1);
		SetLineDimmedNow(ELxPrefKeepKb, !iSettings.iKeep);
		}
	// the amount is dimmed while the tick box is clear
	void HandleControlStateChangeL(TInt aControlId)
		{
		if (aControlId == ELxPrefKeep)
			SetLineDimmedNow(ELxPrefKeepKb, CheckBoxState(ELxPrefKeep) != CEikButtonBase::ESet);
		}
	TBool OkToExitL(TInt)
		{
		iSettings.iStart = ((CEikChoiceList*)Control(ELxPrefStart))->CurrentItem();
		iSettings.iZoom = ((CEikChoiceList*)Control(ELxPrefZoom))->CurrentItem();
		iSettings.iTheme = ((CEikChoiceList*)Control(ELxPrefTheme))->CurrentItem();
		iSettings.iCursor = ((CEikChoiceList*)Control(ELxPrefCursor))->CurrentItem();
		iSettings.iBlink = CheckBoxState(ELxPrefBlink) == CEikButtonBase::ESet;
		iSettings.iStatus = CheckBoxState(ELxPrefStatus) == CEikButtonBase::ESet;
		iSettings.iBold = CheckBoxState(ELxPrefBold) == CEikButtonBase::ESet;
		iSettings.iKeep = CheckBoxState(ELxPrefKeep) == CEikButtonBase::ESet;
		static const TInt KKeepKb[] = { 512, 1024, 2048 };
		iSettings.iKeepKb = KKeepKb[((CEikChoiceList*)Control(ELxPrefKeepKb))->CurrentItem()];
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
class CLxAppUi : public CEikAppUi
	{
public:
	void ConstructL();
	~CLxAppUi();
private:
	void HandleCommandL(TInt aCommand);
	void DynInitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void HandleWsEventL(const TWsEvent& aEvent, CCoeControl* aDestination);
	void LoadSettings();
	void SaveSettings();
	void SwitchToL(TInt aView);
	void HelpL();
	void AboutL();
	void ButtonPictureL(TInt aId, TInt aIcon);
	void ShowToolBarL(TBool aShow);
	TRect ViewRect(TBool aToolbar) const;
	CLxTermView* iTerm;
	CLxRemoteView* iRemote;
	TInt iView;                   // 0 the terminal, 1 remote apps
	TLxSettings iSettings;
	};

void CLxAppUi::LoadSettings()
	{
	Mem::FillZ(&iSettings, sizeof(iSettings));
	iSettings.iVersion = 1;
	iSettings.iZoom = 1;
	iSettings.iStatus = 1;
	iSettings.iBlink = 1;
	iSettings.iToolbar = 1;
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
	RFs& fs = iCoeEnv->FsSession();
	fs.MkDirAll(KIniFile);
	RFile f;
	if (f.Replace(fs, KIniFile, EFileWrite) == KErrNone)
		{
		f.Write(TPckgC<TLxSettings>(iSettings));
		f.Close();
		}
	}

void CLxAppUi::ConstructL()
	{
	BaseConstructL();
	LoadSettings();
	if (iToolBar)
		{
		TRAPD(err, ButtonPictureL(ELxCmdTbStop, ELxMbmToolStop); ButtonPictureL(ELxCmdTbKeys, ELxMbmToolKeys);
			ButtonPictureL(ELxCmdTbCopy, ELxMbmToolCopy); ButtonPictureL(ELxCmdTbPaste, ELxMbmToolPaste));
		if (!iSettings.iToolbar)
			iToolBar->MakeVisible(EFalse);       // (remembered from last time)
		}
	iTerm = new(ELeave) CLxTermView;
	iTerm->ConstructL(ViewRect(iSettings.iToolbar), iSettings);
	AddToStackL(iTerm);
	iTerm->StartLinuxL();
	}

CLxAppUi::~CLxAppUi()
	{
	SaveSettings();
	if (iRemote)
		{
		if (iView == 1)
			RemoveFromStack(iRemote);
		delete iRemote;
		}
	if (iTerm)
		{
		if (iView == 0)
			RemoveFromStack(iTerm);
		delete iTerm;
		}
	}

// a toolbar button's picture: 24x20, from PsiLinux.mbm, the words beside it
void CLxAppUi::ButtonPictureL(TInt aId, TInt aIcon)
	{
	CEikCommandButton* b = iToolBar ? (CEikCommandButton*)iToolBar->ControlById(aId) : NULL;
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
	if (aToolbar && iToolBar && iToolBar->Position().iX > r.iTl.iX)
		r.iBr.iX = iToolBar->Position().iX;
	return r;
	}

// View > Show toolbar (Shift+Ctrl+B): the terminal takes its room or gives
// it back, and Linux's programs see the new width (SIGWINCH)
void CLxAppUi::ShowToolBarL(TBool aShow)
	{
	if (!iToolBar)
		return;
	if (iToolBar->IsVisible() != aShow)
		iToolBar->MakeVisible(aShow);
	if (aShow)
		{
		iToolBar->DrawNow();
		iCoeEnv->WsSession().Flush();
		}
	iTerm->SetTermRectL(ViewRect(aShow));
	if (iRemote)
		iRemote->SetRectL(ViewRect(aShow));
	iSettings.iToolbar = aShow;
	}

// in front or behind (HandleForegroundEventL is private on ER5): the
// terminal's tick slows down behind, where nothing is drawn
void CLxAppUi::HandleWsEventL(const TWsEvent& aEvent, CCoeControl* aDestination)
	{
	if ((aEvent.Type() == EEventFocusGained || aEvent.Type() == EEventFocusLost) && iTerm)
		iTerm->SetForeground(aEvent.Type() == EEventFocusGained);
	CEikAppUi::HandleWsEventL(aEvent, aDestination);
	}

void CLxAppUi::SwitchToL(TInt aView)
	{
	if (aView == iView)
		return;
	if (aView == 1)
		{
		if (!iRemote)
			{
			iRemote = new(ELeave) CLxRemoteView;
			iRemote->ConstructL(ViewRect(iSettings.iToolbar));
			}
		RemoveFromStack(iTerm);
		iTerm->MakeVisible(EFalse);
		iRemote->MakeVisible(ETrue);
		AddToStackL(iRemote);
		iRemote->DrawNow();
		}
	else
		{
		if (iRemote)
			{
			RemoveFromStack(iRemote);
			iRemote->MakeVisible(EFalse);
			}
		iTerm->MakeVisible(ETrue);
		AddToStackL(iTerm);
		iTerm->DrawNow();
		}
	iView = aView;
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
		if (iToolBar && iToolBar->IsVisible())
			aMenuPane->SetItemButtonState(ELxCmdToolbar, EEikMenuItemSymbolOn);
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
	// (in the Remote apps view, only the commands that make sense there)
	if (iView == 1 && aCommand != EEikCmdExit && aCommand != ELxCmdSwitchView && aCommand != ELxCmdViewTerminal &&
		aCommand != ELxCmdViewRemote && aCommand != ELxCmdHelp && aCommand != ELxCmdAbout && aCommand != ELxCmdPreferences &&
		aCommand != ELxCmdToolbar)
		{
		iEikonEnv->InfoMsg(_L("Not available in this view"));
		return;
		}
	switch (aCommand)
		{
	case EEikCmdExit:
		if (iTerm->Session()->Running())
			iEikonEnv->BusyMsgL(_L("Ending Linux..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(0));
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
	case ELxCmdToolbar: ShowToolBarL(!(iToolBar && iToolBar->IsVisible())); SaveSettings(); break;
	case ELxCmdSelectScreen: iTerm->SelectScreen(); break;
	case EEikCmdZoomIn:
	case ELxCmdZoomIn: iTerm->ZoomBy(1); break;
	case EEikCmdZoomOut:
	case ELxCmdZoomOut: iTerm->ZoomBy(-1); break;
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
	case ELxCmdHelp: HelpL(); break;
	case ELxCmdAbout: AboutL(); break;
	default:
		break;
		}
	}

void CLxAppUi::HelpL()
	{
	CEikDialog* d = new(ELeave) CLxHelpDialog(0);
	d->ExecuteLD(R_LX_HELP_DIALOG);
	}

// the About box: the version, Alpine's (the runner reads /etc/alpine-release),
// and the (c) sign put right (0xa9)
class CLxAboutDialog : public CEikDialog
	{
public:
	CLxAboutDialog(const TDesC& aAlpine) : iAlpine(aAlpine) {}
private:
	void PreLayoutDynInitL()
		{
		TBuf<40> v;
		v.Format(_L("PsiLinux %S"), &KLxVersion);
		((CEikLabel*)Control(ELxAbout1))->SetTextL(v);
		TBuf<80> a;
		if (iAlpine.Length())
			a.Format(_L("Alpine Linux %S (armhf, translated for the ARM710T)"), &iAlpine);
		else
			a.Copy(_L("Alpine Linux: start it to see its version"));
		((CEikLabel*)Control(ELxAbout5))->SetTextL(a);
		CEikLabel* l = (CEikLabel*)Control(ELxAbout2);
		TBuf<100> t(*l->Text());
		TInt i = t.Find(_L("(c)"));
		if (i >= 0)
			{
			TBuf<1> c;
			c.Append((TText)0xa9);
			t.Replace(i, 3, c);
			l->SetTextL(t);
			}
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
