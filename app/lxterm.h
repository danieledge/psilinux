// lxterm.h - PsiLinux's terminal view and its Linux session.
//
// CLxTermView is the terminal: libvterm, drawn into a 16-grey window the way
// PsiTerm draws it (the code is PsiTerm's CTermView, without the serial
// line, SSH and tmux), with the history, pen selection and the clipboard.
// It is one control so that PsiLinux can have other views beside it (a
// "Remote apps" view, Ctrl+Q).
//
// CLxSession is the link to psilxrun.exe, the runner: the global chunk
// (lxshared.h), the doorbells (psibell.h) and the runner process.
#ifndef LXTERM_H
#define LXTERM_H

#include <coecntrl.h>
#include <coemain.h>
#include <eikenv.h>
#include <f32file.h>
extern "C" {
#include "psishared.h"
#include "lxshared.h"
}
#include "psibell.h"
#include "vterm.h"
#include "termsb.h"

// the settings PsiLinux keeps (PsiLinux.ini). Saved whole: never change its
// size; new options take the spare fields.
struct TLxSettings
	{
	TInt iVersion;           // 1
	TInt iZoom;              // font size, 0..KLxZoomLevels-1
	TInt iTheme;             // 0 classic, 1 inverted, 2 high contrast, 3 soft
	TInt iCursor;            // 0 block, 1 underline, 2 bar
	TInt iBlink;             // the cursor blinks
	TInt iStatus;            // the status line is shown
	TInt iBold;              // all text bold
	TInt iToolbar;           // the toolbar is shown
	TInt iDisk;              // the disk the Linux files are on: 'C', 'D'... (0 = the card if there is one)
	TInt iStart;             // what starts: 0 Alpine Linux (BusyBox init, OpenRC), 1 Alpine's shell only, 2 PsiLinux
	TInt iKeep;              // 0.21: keep memory for other programs (the reserve)
	TInt iKeepKb;            // ...how much: 512, 1024 or 2048 (0, from an older file: 1024)
	TInt iSpare[12];         // iSpare[KLxSpareLog]: 0.24, Tools > Keep a log (0: off, as in older files)
	};

const TInt KLxSpareLog = 0;

const TInt KLxZoomLevels = 5;

class CLxTermView;

// The link to the runner. One session at a time.
class CLxSession : public CBase
	{
public:
	CLxSession(CLxTermView& aView);
	~CLxSession();
	TInt Start(const TDesC& aRoot, const TDesC8& aProgram, TBool aLogin, TInt aRows, TInt aCols);
	void Stop();                       // asks the runner to end every program
	void Kill();                       // ...and ends it, if it will not
	TBool Running() const { return iRunning; }
	TInt State() const { return iShared ? iShared->state : LXS_EXITED; }
	TInt ExitStatus() const { return iShared ? iShared->exit_status : 0; }
	const LxShared* Shared() const { return iShared; }
	void SetLogOn(TBool aOn) { iLogOn = aOn; }   // (0.24) Keep a log: passed on at the next Start
	void SetReserve(TUint aBytes) { iReserve = aBytes; if (iShared) iShared->reserve = aBytes; }   // the runner reads it at each growth
	void Write(const TDesC8& aBytes);  // typed bytes to the terminal
	void Resize(TInt aRows, TInt aCols);
	void Pump();                       // output to the view, the runner's state
	void Beat() { if (iShared) iShared->app_beat++; }
private:
	static TInt BellCallback(TAny* aSelf);
	static TInt TickCallback(TAny* aSelf);
	void Ring();
	void Arm();
	CLxTermView& iView;
	RChunk iChunk;
	TUint iReserve;              // bytes to keep for other programs (Preferences)
	TBool iLogOn;                // Tools > Keep a log (0.24)
	TBool iChunkOpen;
	LxShared* iShared;
	RProcess iProcess;
	TBool iProcessOpen;
	TBool iRunning;
	CPsiBellWaiter* iWaiter;
	TPsiBellRinger iRinger;
	CPeriodic* iTick;
	TInt iLastState;
	TUint iLastInfo;
	};

class CLxTermView : public CCoeControl
	{
public:
	CLxTermView();
	~CLxTermView();
	void ConstructL(const TRect& aRect, TLxSettings& aSettings);
	// the session
	void StartLinuxL();
	void SessionOutput(const TUint8* aBytes, TInt aLen);
	void SessionStateChanged();
	void SessionInfoChanged();
	CLxSession* Session() { return iSession; }
	void AlpineRelease(TDes& aOut);    // the Alpine version the runner found, or nothing
	// commands
	void SendKey(VTermKey aKey, TInt aMod);
	void SendChar(TUint aChar);
	void SendCtrl(TUint aLetter);
	void SendBytes(const TDesC8& aBytes);
	void ZoomBy(TInt aStep);
	void SetFontL(TInt aZoom);
	void ApplySettingsL();             // appearance changed
	TUint ReserveBytes() const;        // memory to keep for other programs (0: none)
	void SetTermRectL(const TRect& aRect);
	void ResetTerminal();
	void CopySelectionL();
	void PasteL();
	void SelectScreen();
	void ScrollBy(TInt aLines);
	void ScrollTo(TInt aOffset);
	TBool HasSelection() const { return iSelActive; }
	TInt Cols() const { return iCols; }
	TInt Rows() const { return iRows; }
	void SetForeground(TBool aForeground);
	void LocalMessage(const TDesC8& aText);
	// from CCoeControl
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void HandlePointerEventL(const TPointerEvent& aPointerEvent);
private:
	TBuf<32> iAlpine;                  // (the last Alpine version the runner said)
	struct TLook { TUint iCh; TInt iWidth; TInt iFg; TInt iBg; TUint iFlags; };
	void Draw(const TRect& aRect) const;
	void DrawAll(CWindowGc& aGc) const;
	void DrawCells(CWindowGc& aGc, TInt aRow0, TInt aCol0, TInt aRow1, TInt aCol1) const;
	void DrawOneCell(CWindowGc& aGc, TInt aRow, TInt aCol) const;
	void DrawOneCell(CWindowGc& aGc, TInt aRow, TInt aCol, const TLook& aLook) const;
	void DrawCursor(CWindowGc& aGc) const;
	void DrawStatus(CWindowGc& aGc) const;
	void DrawScrollIndicator(CWindowGc& aGc) const;
	void StatusText(TDes& aText, TInt& aSplit) const;
	TBool GetLook(TInt aRow, TInt aCol, TLook& aLook) const;
	TInt TextByte(const TLook& aLook) const;
	void CellColours(const VTermScreenCell& aCell, TInt& aFg, TInt& aBg) const;
	TInt Theme(TInt aGrey) const;
	void ThemePair(TInt aFg, TInt aBg, TInt& aThemedFg, TInt& aThemedBg) const;
	TRgb Grey(TInt aGrey) const { return TRgb::Gray16(Theme(aGrey)); }
	TInt ViewLine(TInt aRow) const { return iLinesPushed - iScrollOffset + aRow; }
	TInt RowY(TInt aRow) const { return iOriginY + aRow * iCellH; }
	void Layout();
	void ReleaseFont();
	void BeginPaint();
	void EndPaint();
	void AddDamage(TInt aRow0, TInt aCol0, TInt aRow1, TInt aCol1);
	void FeedTerminal(const TUint8* aBytes, TInt aLen);
	void RepaintRows(TInt aRow0, TInt aRow1);
	void ClearSelection();
	void Tick();
	void StartTick();
	static TInt TickCallback(TAny* aSelf);
	static int CbDamage(VTermRect aRect, void* aUser);
	static int CbMoveRect(VTermRect aDest, VTermRect aSrc, void* aUser);
	static int CbPushLine(int aCols, const VTermScreenCell* aCells, void* aUser);
	static int CbClearScrollback(void* aUser);
	static int CbMoveCursor(VTermPos aPos, VTermPos aOldPos, int aVisible, void* aUser);
	static int CbSetTermProp(VTermProp aProp, VTermValue* aVal, void* aUser);
	static int CbBell(void* aUser);
	static void CbOutput(const char* aBytes, size_t aLen, void* aUser);
	static int SelLineFn(void* aCtx, int aLine, unsigned int* aChars, int aMaxCols);
	static int SelMapFn(unsigned int aCh);
private:
	TLxSettings* iSettings;
	CLxSession* iSession;
	VTerm* iVt;
	VTermScreen* iScreen;
	VTermScreenCallbacks iCallbacks;
	CFont* iFont;
	TInt iCellW, iCellH, iAscent;
	TInt iCols, iRows, iOriginX, iOriginY;
	TBool iMono;
	TInt iFontFileId;
	TBool iFontFileLoaded;
	CWindowGc* iPaintGc;
	TTermSb iSb;
	TSbCell* iSbCells;
	short* iSbCols;
	TInt iLinesPushed;
	TInt iScrollOffset;
	TBool iNeedFull;
	TBool iSelActive;
	TInt iSelLine0, iSelCol0, iSelLine1, iSelCol1;
	enum { EPenNone, EPenSelect, EPenScroll, EPenMouse } iPenMode;
	TInt iMouseMode;
	TInt iPenRow0, iPenCol0, iPenY0, iPenOffset0;
	TBool iPenWheeled;
	VTermColor iCacheFg, iCacheBg;
	TInt iCacheRev, iCacheF, iCacheB;
	TBool iCacheValid;
	TBool iDamaged;
	TInt iDmgRow0, iDmgCol0, iDmgRow1, iDmgCol1;
	TInt iCurRow, iCurCol;
	TBool iCurVisible;
	TInt iDrawnCurRow, iDrawnCurCol;
	TBool iDrawnCurVisible;
	TBool iBlinkHidden;
	TInt iStatusH;
	CPeriodic* iTick;
	TBool iForeground;
	TBuf<120> iStatusDrawn;
	TUint iLastBell;
	TBool iEnded;              // the session ended: Enter starts it again
	};

// glyphs.cpp (PsiTerm's): Unicode to what the Psion's fonts can draw
TInt PsiMapToCodePage(TUint aCodePoint);
TUint PsiCodePageToUnicode(TUint aByte);
TInt PsiBoxSegments(TUint aCodePoint);
const TInt KBoxLeft  = 1;
const TInt KBoxRight = 2;
const TInt KBoxUp    = 4;
const TInt KBoxDown  = 8;
const TInt KBoxHeavy = 16;
const TInt KBoxDouble = 32;

#endif
