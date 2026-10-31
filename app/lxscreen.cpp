// lxscreen.cpp - the Linux screen (lxscreen.h).
#include <eikenv.h>
#include <e32keys.h>
#include "lxscreen.h"

// Linux's key codes (linux/input-event-codes.h)
enum
	{
	KEsc = 1, K1 = 2, K0 = 11, KMinus = 12, KEqual = 13, KBackspace = 14, KTab = 15,
	KLeftBrace = 26, KRightBrace = 27, KEnter = 28, KLeftCtrl = 29, KSemicolon = 39, KApostrophe = 40,
	KGrave = 41, KLeftShift = 42, KBackslash = 43, KComma = 51, KDot = 52, KSlash = 53, KRightShift = 54,
	KSpace = 57, KHome = 102, KUp = 103, KPageUp = 104, KLeft = 105, KRight = 106, KEnd = 107,
	KDown = 108, KPageDown = 109, KDelete = 111, KMenu = 127,   // (KEY_COMPOSE: the PC's Menu key, X's Menu)
	BtnLeft = 0x110, BtnRight = 0x111, BtnTouch = 0x14a
	};
enum { EvSyn = 0, EvKey = 1, EvAbs = 3 };

// the letters' keys, A to Z
static const TUint8 KLetter[26] =
	{ 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44 };

// a character's key on a US keyboard, for the ones the Psion makes another
// way (Shift+2 is " here): the key, and 0x80 if it needs Shift
static const TUint8 KAscii[95] =
	{
	KSpace, 2|0x80, KApostrophe|0x80, 4|0x80, 5|0x80, 6|0x80, 8|0x80, KApostrophe,       // space ! " # $ % & '
	10|0x80, 11|0x80, 9|0x80, KEqual|0x80, KComma, KMinus, KDot, KSlash,                  // ( ) * + , - . /
	11, 2, 3, 4, 5, 6, 7, 8, 9, 10,                                                       // 0-9
	KSemicolon|0x80, KSemicolon, KComma|0x80, KEqual, KDot|0x80, KSlash|0x80, 3|0x80,     // : ; < = > ? @
	30|0x80, 48|0x80, 46|0x80, 32|0x80, 18|0x80, 33|0x80, 34|0x80, 35|0x80, 23|0x80, 36|0x80, 37|0x80, 38|0x80, 50|0x80,
	49|0x80, 24|0x80, 25|0x80, 16|0x80, 19|0x80, 31|0x80, 20|0x80, 22|0x80, 47|0x80, 17|0x80, 45|0x80, 21|0x80, 44|0x80,
	KLeftBrace, KBackslash, KRightBrace, 7|0x80, KMinus|0x80, KGrave,                     // [ \ ] ^ _ `
	30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
	KLeftBrace|0x80, KBackslash|0x80, KRightBrace|0x80, KGrave|0x80                       // { | } ~
	};

CLxScreenView::~CLxScreenView()
	{
	Close();
	delete iFrame;
	delete iPoll;
	for (TInt i = 0; i < KStrips; i++)
		delete iBitmap[i];
	}

void CLxScreenView::ConstructL(const TRect& aRect)
	{
	// (16 greys, as the terminal's: a window in the default mode has every
	// pixel dithered down to 4 greys, seconds a frame; backed up, so it is
	// drawn into straight away and never asked to redraw)
	CreateBackedUpWindowL(iCoeEnv->RootWin(), EGray16);
	SetRectL(aRect);
	EnableDragEvents();
	for (TInt i = 0; i < KStrips; i++)
		{
		iBitmap[i] = new(ELeave) CLxBitmap(iCoeEnv->WsSession());
		User::LeaveIfError(iBitmap[i]->Create(TSize(LX_FB_W, KStrip), EGray16));
		}
	iPoll = CPeriodic::NewL(CActive::EPriorityLow);
	iFrame = CPeriodic::NewL(CActive::EPriorityLow);
	ActivateL();
	}

TInt CLxScreenView::Open()
	{
	if (iFb)
		return KErrNone;
	TInt err = iChunk.OpenGlobal(_L(LX_FB_NAME), EFalse);
	if (err != KErrNone)
		return err;
	LxFbHeader* fb = (LxFbHeader*)iChunk.Base();
	if (fb->magic != LX_FB_MAGIC)
		{
		iChunk.Close();
		return KErrNotSupported;
		}
	iFb = fb;
	iPixels = (const TUint8*)fb + LX_FB_HDR;
	iGreySeq = fb->grey_seq - 1;           // (the tables, and everything drawn, at the first Pump)
	iAll = fb->dmg_all - 1;
	iPollMs = 0;
	DrawChanges();
	return KErrNone;
	}

void CLxScreenView::Close()
	{
	if (!iFb)
		return;
	iPoll->Cancel();
	iFrame->Cancel();
	iFb = 0;
	iPixels = 0;
	iChunk.Close();
	}

const TInt KFrameUs = 50000;          // (20 frames a second at most)

void CLxScreenView::Pump()
	{
	if (iFb && !iFrame->IsActive())
		iFrame->Start(KFrameUs, KFrameUs, TCallBack(FrameCallback, this));
	}

TInt CLxScreenView::FrameCallback(TAny* aSelf)
	{
	CLxScreenView* v = (CLxScreenView*)aSelf;
	v->iFrame->Cancel();                 // (one frame; the next ring starts another)
	v->DrawChanges();
	return 0;
	}

// the rectangles that changed: drawn in at most four pieces, near ones joined
void CLxScreenView::DrawChanges()
	{
	if (!iFb)
		return;
	if (iFb->poll_ms != iPollMs)
		{
		iPollMs = iFb->poll_ms;
		iPoll->Cancel();
		if (iPollMs > 0)
			iPoll->Start(iPollMs * 1000, iPollMs * 1000, TCallBack(PollCallback, this));
		}
	TBool all = EFalse;
	if (iFb->grey_seq != iGreySeq)
		{
		iGreySeq = iFb->grey_seq;
		lx_fbconv_tables(&iConv, iFb->grey);
		all = ETrue;
		}
	if (iFb->dmg_all != iAll)
		{
		iAll = iFb->dmg_all;
		all = ETrue;
		}
	TRect r[4];
	TInt n = 0;
	TUint head = iFb->dmg_head;
	while (iFb->dmg_tail != head)
		{
		const LxFbRect& d = iFb->dmg[iFb->dmg_tail & (LX_FB_DMG - 1)];
		TRect a(d.x, d.y, d.x + d.w, d.y + d.h);
		iFb->dmg_tail++;
		if (all)
			continue;
		TInt i;
		for (i = 0; i < n; i++)
			{
			TRect g = r[i];
			g.Grow(16, 8);                 // (near enough: one blit is cheaper than two)
			if (g.Intersects(a))
				break;
			}
		if (i < n)
			r[i].BoundingRect(a);
		else if (n < 4)
			r[n++] = a;
		else
			r[3].BoundingRect(a);
		}
	if (all)
		{
		r[0] = TRect(0, 0, LX_FB_W, LX_FB_H);
		n = 1;
		}
	for (TInt i = 0; i < n; i++)
		{
		Convert(r[i]);
		Show(r[i]);
		}
	if (n)
		iCoeEnv->WsSession().Flush();      // (on the screen now, not when something else flushes)
	}

TInt CLxScreenView::PollCallback(TAny* aSelf)
	{
	CLxScreenView* v = (CLxScreenView*)aSelf;
	if (!v->iFb || v->iFb->poll_ms <= 0)
		{
		// (the program says where it draws now: no more looking)
		v->iPollMs = 0;
		v->iPoll->Cancel();
		return 0;
		}
	if (v->IsVisible())
		{
		TRect all(0, 0, LX_FB_W, LX_FB_H);
		v->Convert(all);
		v->Show(all);
		v->iCoeEnv->WsSession().Flush();
		}
	return 1;
	}

void CLxScreenView::Convert(const TRect& aRect)
	{
	TInt x0 = aRect.iTl.iX & ~1, x1 = (aRect.iBr.iX + 1) & ~1;
	if (x0 < 0) x0 = 0;
	if (x1 > LX_FB_W) x1 = LX_FB_W;
	TInt y0 = Max(aRect.iTl.iY, 0), y1 = Min(aRect.iBr.iY, LX_FB_H);
	TInt stride = CFbsBitmap::ScanLineLength(LX_FB_W, EGray16);
	for (TInt s = y0 / KStrip; s < KStrips && s * KStrip < y1; s++)
		{
		TInt a = Max(y0, s * KStrip), b = Min(y1, (s + 1) * KStrip);
		iBitmap[s]->Lock();
		TUint8* d = (TUint8*)iBitmap[s]->DataAddress() - s * KStrip * stride;
		if (iFb->depth == 4)
			{
			// (the Psion's own pixels: as they are)
			for (TInt y = a; y < b; y++)
				Mem::Copy(d + y * stride + x0 / 2, iPixels + y * LX_FB_ROW4 + x0 / 2, (x1 - x0) / 2);
			}
		else
			{
			for (TInt y = a; y < b; y++)
				lx_fbconv_row(&iConv, y, iPixels + y * LX_FB_W, d + y * stride, x0, x1);
			}
		iBitmap[s]->Unlock();
		}
	}

void CLxScreenView::Show(const TRect& aRect)
	{
	if (!IsVisible())
		return;
	ActivateGc();
	Draw(aRect);
	DeactivateGc();
	}

void CLxScreenView::Draw(const TRect& aRect) const
	{
	CWindowGc& gc = SystemGc();
	if (iFb)
		{
		for (TInt s = 0; s < KStrips; s++)
			{
			TRect r(0, s * KStrip, LX_FB_W, (s + 1) * KStrip);
			r.Intersection(aRect);
			if (!r.IsEmpty())
				gc.BitBlt(r.iTl, iBitmap[s], TRect(r.iTl - TPoint(0, s * KStrip), r.Size()));
			}
		}
	else
		{
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(KRgbBlack);
		gc.Clear(aRect);
		}
	}

void CLxScreenView::Post(TInt aDev, TInt aType, TInt aCode, TInt aValue)
	{
	if (!iFb)
		return;
	TUint head = iFb->in_head;
	if (head - iFb->in_tail >= LX_FB_IN)
		return;                            // (full: Linux is not reading)
	LxInEvent& e = iFb->in[head & (LX_FB_IN - 1)];
	e.dev = (TUint8)aDev;
	e.type = (TUint8)aType;
	e.code = (TUint16)aCode;
	e.value = aValue;
	iFb->in_head = head + 1;
	}

// ---- the pen: an absolute pointer, in the screen's own pixels ----------------

void CLxScreenView::HandlePointerEventL(const TPointerEvent& aPointerEvent)
	{
	if (!iFb)
		return;
	TPoint p = aPointerEvent.iPosition;
	p.iX = Max(0, Min(p.iX, LX_FB_W - 1));
	p.iY = Max(0, Min(p.iY, LX_FB_H - 1));
	Post(LX_IN_PEN, EvAbs, 0, p.iX);
	Post(LX_IN_PEN, EvAbs, 1, p.iY);
	switch (aPointerEvent.iType)
		{
	case TPointerEvent::EButton1Down:
		// Ctrl+tap: the right button (menus)
		iPenButton = (aPointerEvent.iModifiers & EModifierCtrl) ? BtnRight : BtnLeft;
		Post(LX_IN_PEN, EvKey, BtnTouch, 1);
		Post(LX_IN_PEN, EvKey, iPenButton, 1);
		iPenDown = ETrue;
		break;
	case TPointerEvent::EButton1Up:
		if (iPenDown)
			{
			Post(LX_IN_PEN, EvKey, BtnTouch, 0);
			Post(LX_IN_PEN, EvKey, iPenButton, 0);
			iPenDown = EFalse;
			}
		break;
	default:
		break;
		}
	Sync(LX_IN_PEN);
	}

// ---- the keys ---------------------------------------------------------------------
// A key that is the same key on a PC is passed on as itself, pressed and let
// go (so a game can hold an arrow down): the letters, the arrows, Enter,
// Space, Shift, Ctrl... and the digits without Shift. Fn+arrow is Home, End,
// Page Up and Down. Anything else the Psion's keyboard makes (Shift+2 is ",
// Fn+... the brackets) is passed on as the US key that types the same
// character, with Shift as it needs.

TBool CLxScreenView::KeyDirect(const TKeyEvent& aKeyEvent, TInt& aLinux) const
	{
	TInt s = aKeyEvent.iScanCode;
	TUint mods = aKeyEvent.iModifiers;
	TBool fn = (mods & EModifierFunc) != 0;
	if (s >= 'A' && s <= 'Z' && !fn) { aLinux = KLetter[s - 'A']; return ETrue; }
	if (s >= '0' && s <= '9' && !fn && !(mods & EModifierShift)) { aLinux = s == '0' ? K0 : K1 + s - '1'; return ETrue; }
	switch (s)
		{
	case EStdKeySpace: aLinux = KSpace; return ETrue;
	case EStdKeyEnter: aLinux = KEnter; return ETrue;
	case EStdKeyEscape: aLinux = KEsc; return ETrue;
	case EStdKeyTab: aLinux = KTab; return ETrue;
	case EStdKeyBackspace: aLinux = (mods & EModifierShift) ? KDelete : KBackspace; return ETrue;
	case EStdKeyDelete: aLinux = KDelete; return ETrue;
	case EStdKeyLeftShift: aLinux = KLeftShift; return ETrue;
	case EStdKeyRightShift: aLinux = KRightShift; return ETrue;
	case EStdKeyLeftCtrl: case EStdKeyRightCtrl: aLinux = KLeftCtrl; return ETrue;
	case EStdKeyLeftArrow: aLinux = fn ? KHome : KLeft; return ETrue;
	case EStdKeyRightArrow: aLinux = fn ? KEnd : KRight; return ETrue;
	case EStdKeyUpArrow: aLinux = fn ? KPageUp : KUp; return ETrue;
	case EStdKeyDownArrow: aLinux = fn ? KPageDown : KDown; return ETrue;
	case EStdKeyHome: aLinux = KHome; return ETrue;
	case EStdKeyEnd: aLinux = KEnd; return ETrue;
	case EStdKeyPageUp: aLinux = KPageUp; return ETrue;
	case EStdKeyPageDown: aLinux = KPageDown; return ETrue;
	case EStdKeyMenu: aLinux = KMenu; return ETrue;   // (the desktop's menus: Ctrl+Q still goes back to the terminal)
		}
	return EFalse;
	}

void CLxScreenView::KeyChar(TUint aChar)
	{
	if (aChar < 32 || aChar > 126)
		return;
	TInt k = KAscii[aChar - 32];
	TBool want = (k & 0x80) != 0;
	TBool have = iSent[EStdKeyLeftShift] || iSent[EStdKeyRightShift];
	k &= 0x7f;
	if (want != have)
		Post(LX_IN_KBD, EvKey, KLeftShift, want);
	Post(LX_IN_KBD, EvKey, k, 1);
	Post(LX_IN_KBD, EvKey, k, 0);
	if (want != have)
		Post(LX_IN_KBD, EvKey, KLeftShift, have);
	Sync(LX_IN_KBD);
	}

TKeyResponse CLxScreenView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (!iFb)
		return EKeyWasNotConsumed;
	TInt s = aKeyEvent.iScanCode;
	TInt k;
	// Ctrl+Q: Switch view (EIKON's hotkey), the one key that is not Linux's
	if (s == 'Q' && (aKeyEvent.iModifiers & EModifierCtrl) && !(aKeyEvent.iModifiers & EModifierShift))
		return EKeyWasNotConsumed;
	TBool sent = s >= 0 && s < 256 && iSent[s];
	switch (aType)
		{
	case EEventKeyDown:
		if (s >= 0 && s < 256 && !sent && KeyDirect(aKeyEvent, k))
			{
			iSent[s] = (TUint8)k;
			Post(LX_IN_KBD, EvKey, k, 1);
			Sync(LX_IN_KBD);
			}
		break;
	case EEventKeyUp:
		if (sent)
			{
			// (as the key it went down as, whatever Shift and Fn do now)
			Post(LX_IN_KBD, EvKey, iSent[s], 0);
			iSent[s] = 0;
			Sync(LX_IN_KBD);
			}
		break;
	case EEventKey:
		if (!sent)
			KeyChar(aKeyEvent.iCode);      // (Linux repeats a held key itself)
		break;
	default:
		break;
		}
	return EKeyWasConsumed;
	}
