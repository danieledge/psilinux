// lxterm.h - PsiLinux's terminal view and its Linux session.
//
// (0.29) The terminal is PsiTerm's own: the class CTermView of PsiTerm's
// psiterm.h, its drawing, fonts (Terminus, Spleen, Gohufont, Cozette and
// their sizes), themes and colour schemes, history, pen selection and
// clipboard compiled from PsiTerm's psiterm.cpp as they are (lxptview.cpp,
// made by tools/ptview.py at every build). CLxTermView is that terminal
// with PsiLinux's session in place of PsiTerm's SSH: lxterm.cpp gives the
// parts of CTermView that are PsiLinux's (the bytes typed go to the runner,
// the status line, the keys, the tick), and nothing for tmux's tab strip.
// docs/PTVIEW.md says how PsiTerm could make that seam a file of its own.
//
// CLxSession is the link to psilxrun.exe, the runner: the global chunk
// (lxshared.h), the doorbells (psibell.h) and the runner process.
#ifndef LXTERM_H
#define LXTERM_H

// (the system headers psiterm.h takes, first: see below)
#include <coecntrl.h>
#include <coemain.h>
#include <eikappui.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikenv.h>
#include <eikmenup.h>
#include <eikcmds.hrh>
#include <eikmenu.hrh>
#include <c32comm.h>
#include <f32file.h>
#include <eikdialg.h>
#include <eikdialg.hrh>
#include <eikbutb.h>
#include <badesca.h>
#include <e32hal.h>
// PsiTerm's terminal. Its data are private to CTermView; PsiLinux's view
// reads a few of them (the selection, the history's length), so the class
// is seen here with everything public. Only access changes, never layout.
#define private public
#include "psiterm.h"
#undef private
extern "C" {
#include "lxshared.h"
}

// the settings PsiLinux keeps (PsiLinux.ini). Saved whole: never change its
// size; new options take the spare fields.
struct TLxSettings
	{
	TInt iVersion;           // 1
	TInt iZoom;              // font size: PsiTerm's zoom levels (0 Courier small, then the typeface's sizes)
	TInt iTheme;             // PsiTerm's themes: 0 classic, 1 inverted, 2 high contrast, 3 soft; colour screens 4.. (0.29)
	TInt iCursor;            // 0 block, 1 underline, 2 bar
	TInt iBlink;             // the cursor blinks
	TInt iStatus;            // the status line is shown
	TInt iBold;              // all text bold
	TInt iToolbar;           // the toolbar is shown
	TInt iDisk;              // the disk the Linux files are on: 'C', 'D'... (0 = the card if there is one)
	TInt iStart;             // what starts: 0 Alpine Linux (BusyBox init, OpenRC), 1 Alpine's shell only, 2 PsiLinux
	TInt iKeep;              // 0.21: keep memory for other programs (the reserve)
	TInt iKeepKb;            // ...how much: 512, 1024 or 2048 (0, from an older file: 1024)
	TInt iSpare[12];         // KLxSpare*: spare fields given a meaning since
	};

const TInt KLxSpareLog = 0;      // 0.24: Tools > Keep a log (0: off, as in older files)
const TInt KLxSpareFace = 1;     // 0.29: the typeface of the zoom levels (PsiTerm's iFace: 0 Terminus...)
const TInt KLxSpareNoWelcome = 2; // 0.37: 1: Tools > Show welcome banner not ticked (0: shown, as in older files)
const TInt KLxSpareSleep = 3;    // 0.39: In the background: 0 sleep after 1 minute (older files), 1 5 minutes, 2 15 minutes, 3 never
const TInt KLxSpareSleep2 = 4;   // 0.40: In the background, the choice + 1 (0: not set, in older files: Never sleep,
                                 // or 0.39's 5 or 15 minutes if that was chosen); KLxSpareSleep is kept the same
// (0.40) Preferences > In the background: 0 sleep after 1 minute, 1 5 minutes, 2 15 minutes, 3 never (the standard)
inline TInt LxSleepChoice(const TLxSettings& aS)
	{
	TInt v = aS.iSpare[KLxSpareSleep2];
	if (v >= 1 && v <= 4)
		return v - 1;
	TInt old = aS.iSpare[KLxSpareSleep];
	return old == 1 || old == 2 ? old : 3;
	}
inline void LxSetSleepChoice(TLxSettings& aS, TInt aChoice)
	{
	aS.iSpare[KLxSpareSleep2] = aChoice + 1;
	aS.iSpare[KLxSpareSleep] = aChoice;
	}

// (0.29) PsiTerm's theme for a colour screen with no saved settings (its
// KPtThemeClean, 1.04)
const TInt KLxThemeColourDefault = 5;

class CLxTermView;
class CLxEpoc;

// told when Linux's screen (/dev/fb0) opens or closes, and when the runner rings
class MLxScreenHook
	{
public:
	virtual void ScreenOnOff(TBool aOn) = 0;
	virtual void ScreenPump() = 0;
	};

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
	void SetWelcome(TBool aOn) { iWelcomeOff = !aOn; if (iShared) iShared->welcome_off = iWelcomeOff; }   // (0.37) the next shell reads it
	void Write(const TDesC8& aBytes);  // typed bytes to the terminal
	void Resize(TInt aRows, TInt aCols);
	void Pump();                       // output to the view, the runner's state
	void Beat();
	// (0.38) memory: the runner gives back its caches now; PsiLinux in front or behind
	void Trim() { User::Heap().Compress(); if (iShared) { iShared->trim_seq++; Ring(); } }
	void SetStartFree(TUint aBytes) { iStartFree = aBytes; }
	// (0.39) sleep: after this many seconds behind (0: never); the launcher's "sleep"
	void SetSleepAfter(TInt aSecs) { iSleepAfter = aSecs; if (iShared) iShared->sleep_after = aSecs; }
	void SleepNow(TBool aAsked) { iSleepAsked = aAsked; if (iShared) { iShared->sleep_req++; Ring(); } }
	void InFront() { if (iShared && iShared->app_behind) { iShared->app_behind = 0; Ring(); } TickRate(); }
	void SleepUi();                    // "Waking Linux..." and the like (the tick)
	void Ring();                       // the runner's doorbell (keys or the pen waiting for it)
	void SetScreenHook(MLxScreenHook* aHook) { iScreenHook = aHook; }
private:
	static TInt BellCallback(TAny* aSelf);
	static TInt TickCallback(TAny* aSelf);
	void TickRate();             // (0.40) the tick: every 5 s while Linux sleeps behind
	static TInt EpocAnswered(TAny* aSelf);   // (0.36) a message's answer is in the chunk
	CLxEpoc* iEpoc;              // (0.36) EPOC requests from Linux (lxepocsv.cpp)
	void Arm();
	CLxTermView& iView;
	RChunk iChunk;
	TUint iReserve;              // bytes to keep for other programs (Preferences)
	TBool iLogOn;                // Tools > Keep a log (0.24)
	TBool iWelcomeOff;           // Tools > Show welcome banner not ticked (0.37)
	TUint iStartFree;            // (0.38) the Psion's free RAM as the app started
	TInt iBeats;                 // (0.38) ticks, for the heap's compressing
	TInt iSleepAfter;            // (0.39) Preferences: In the background (seconds; 0 never)
	TBool iSleepAsked;           // (0.39) the launcher asked: an infoprint says what sleep gave back
	TUint iSleepSeq;
	TBool iWaking;               // "Waking Linux..." is up
	TBool iWakingCard;           // (0.40) ...as "Put the card back to wake Linux"
	TInt iTickNowUs;             // (0.40) the tick's period now
	TBool iNoRoomTold;
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
	MLxScreenHook* iScreenHook;
	TUint iFbSeq;
	TBool iFbOn;
	};

// PsiTerm's terminal with PsiLinux's session
class CLxTermView : public CTermView
	{
public:
	CLxTermView();
	~CLxTermView();
	void ConstructL(const TRect& aRect, TLxSettings& aSettings);
	// the session
	void StartLinuxL();
	// (0.26) the next start only, in place of Preferences > Start (0 Alpine,
	// 1 Alpine's shell only, 2 PsiLinux): PsiHub's Terminal asks for 1
	void SetStartOnce(TInt aStart) { iStartOnce = aStart; }
	void SessionOutput(const TUint8* aBytes, TInt aLen);
	void SessionStateChanged();
	void SessionInfoChanged();
	CLxSession* Session() { return iSession; }
	void AlpineRelease(TDes& aOut);    // the Alpine version the runner found, or nothing
	void SendBytes(const TDesC8& aBytes);   // to the runner (CTermView::WriteToHost)
	void ApplySettingsL();             // appearance changed (TLxSettings -> PsiTerm's)
	void ZoomBy(TInt aStep);           // PsiTerm's, and the size kept in TLxSettings
	TInt SleepAfter() const;           // (0.39) Preferences: In the background, in seconds (0: never)
	void SetTermRectL(const TRect& aRect);   // PsiTerm's, and the runner told the new size
	TUint ReserveBytes() const;        // memory to keep for other programs (0: none)
	TBool HasSelection() const { return iSelActive; }
	TBool Ended() const { return iEnded; }
	void LxStatusText(TDes& aText, TInt& aSplit) const;   // CTermView::StatusText
	TKeyResponse LxKeyL(const TKeyEvent& aKeyEvent, TEventCode aType);   // CTermView::OfferKeyEventL
	// (0.29) the echo time, on the status line with Keep a log on: from a key
	// going to the runner to its echo drawn, in ms
	void EchoDrawn();
	TInt EchoLast() const { return iEchoLastMs; }
	TInt EchoAvg() const { return iEchoAvgMs; }
	TLxSettings& LxSettings() { return *iLxSettings; }
private:
	void ToPsiTerm();                  // TLxSettings -> CTermView's TPsiSettings
	void SizeToRunner();               // the terminal's rows and columns, if they changed
	TLxSettings* iLxSettings;
	CLxSession* iSession;
	TBuf<32> iAlpine;                  // (the last Alpine version the runner said)
	TBool iEnded;                      // the session ended: Enter starts it again
	TInt iStartOnce;                   // (0.26) SetStartOnce(), or -1
	TUint iKeyAt;                      // (0.29) tick a key went to the runner (0: none waiting)
	TInt iEchoLastMs, iEchoAvgMs;
	static TInt CloseApp(TAny* aSelf);  // (0.36) "exit" at the top shell closes PsiLinux
	CIdle* iCloser;
	};

#endif
