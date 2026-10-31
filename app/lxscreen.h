// lxscreen.h - the Linux screen: what Linux's programs draw on /dev/fb0
// (X, games), shown on the whole of the Psion's screen, with the pen and the
// keys going back to them as /dev/input.
//
// The pixels are in the runner's global chunk (lxfb.h). The runner says
// which rectangles changed; only those are turned into the Psion's 16 greys
// (lxfbconv.h) and drawn.
#ifndef LXSCREEN_H
#define LXSCREEN_H

#include <coecntrl.h>
#include <fbs.h>
#include <w32std.h>
extern "C" {
#include "lxfb.h"
#include "lxfbconv.h"
}

// (a bitmap the screen is written into directly: its heap held still meanwhile,
// LockHeap being protected on ER5)
class CLxBitmap : public CWsBitmap
	{
public:
	CLxBitmap(RWsSession& aWs) : CWsBitmap(aWs) {}
	void Lock() const { LockHeap(); }
	void Unlock() const { UnlockHeap(); }
	};

class CLxScreenView : public CCoeControl
	{
public:
	~CLxScreenView();
	void ConstructL(const TRect& aRect);
	TInt Open();                       // the runner's screen chunk (when LxShared.fb_on says so)
	void Close();
	TBool IsOpen() const { return iFb != 0; }
	void Pump();                       // the runner rang: draw what changed, soon (at most 20 times a second)
	void SetWake(const TCallBack& aWake) { iWake = aWake; }   // rings the runner: input is waiting
	// from CCoeControl
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void HandlePointerEventL(const TPointerEvent& aPointerEvent);
private:
	void Draw(const TRect& aRect) const;
	void Convert(const TRect& aRect);  // the 8-bit pixels of aRect into iBitmap
	void Show(const TRect& aRect);     // iBitmap's aRect onto the screen, now
	void Post(TInt aDev, TInt aType, TInt aCode, TInt aValue);
	void Sync(TInt aDev) { Post(aDev, 0, 0, 0); iWake.CallBack(); }   // (an event is complete: the runner takes it now)
	TBool KeyDirect(const TKeyEvent& aKeyEvent, TInt& aLinux) const;
	void KeyChar(TUint aChar);
	void DrawChanges();
	static TInt PollCallback(TAny* aSelf);
	static TInt FrameCallback(TAny* aSelf);
	RChunk iChunk;
	LxFbHeader* iFb;
	const TUint8* iPixels;
	enum { KStrip = 48, KStrips = LX_FB_H / KStrip };
	CLxBitmap* iBitmap[KStrips];       // the screen in 16 greys (EGray16), in strips: ER5's window server
	                                   // stopped on one 640x240 bitmap (a large one)
	LxFbConv iConv;
	TUint iGreySeq;
	TUint iAll;
	CPeriodic* iPoll;                  // for programs that draw without saying where
	CPeriodic* iFrame;                 // the next frame: the app (in front, above Linux's
	                                   // priority) draws only so often, so Linux keeps the CPU
	TInt iPollMs;
	TUint8 iSent[256];                 // by EPOC scan code: the Linux key it went down as (0: none)
	TCallBack iWake;
	TBool iPenDown;
	TInt iPenButton;                   // BTN_LEFT, or BTN_RIGHT (Ctrl+tap)
	};

#endif
