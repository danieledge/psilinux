// lxepocsv.h - (0.36) PsiLinux.app's side of EPOC requests: the epoc command
// in Linux asks (common/lxepoc.h), the kernel checks the request and the
// runner leaves it in the shared chunk; CLxEpoc does it here, in the app,
// which has the window server, apparc and the clipboard, and answers.
#ifndef LXEPOCSV_H
#define LXEPOCSV_H

#include <e32base.h>
#include <f32file.h>
#include <w32std.h>
extern "C" {
#include "lxshared.h"
#include "lxepoc.h"
}

class CLxEpoc : public CActive
	{
public:
	static CLxEpoc* NewL();
	~CLxEpoc();
	// a request is waiting in aShared (epoc_req_seq != epoc_ans_seq): do it,
	// and answer (now, or when a message's button is pressed). ETrue if it
	// was answered, and the runner should be rung.
	TBool Serve(LxShared& aShared);
	void SetAnswered(TCallBack aCb) { iAnswered = aCb; }   // (a message's answer: ring the runner)
	void Forget() { iShared = NULL; }        // the session has ended
private:
	CLxEpoc();
	void ConstructL();
	// CActive: the message (RNotifier) was answered
	void RunL();
	void DoCancel();
	void DoL(const LxeHeader& aReq, const TUint8* aArgs, TInt aLen);
	void Answer(TInt aStatus, TInt aErr = 0, TUint aArg = 0);
	void Text(const TDesC8& aUtf8);         // append to the answer
	void TextEpoc(const TDesC& aEpoc);      // ...an EPOC string, as UTF-8
	void Field(const TDesC& aEpoc, TBool aLast = EFalse);   // ...a field, then a tab or a new line
	void FieldNum(TInt aNum, TBool aLast = EFalse);
	void FieldUid(TUid aUid, TBool aLast = EFalse);
	void Key(const TDesC8& aKey, const TDesC& aValue);
	void KeyNum(const TDesC8& aKey, TInt aValue);
	TPtrC8 Arg(TInt aIndex) const;          // the request's i'th string (UTF-8)
	void ArgEpoc(TInt aIndex, TDes& aOut) const;   // ...as EPOC text
	// the requests
	void OpenL();
	void RunAppL();
	void TasksL();
	void PsL();
	void CloseL();
	void KillL();
	void InfoL();
	void ClipGetL();
	void ClipSetL();
	void NoteL();
	void BeepL();
	void AlertL(TBool aConfirm);
	void ShotL();
	void TimeL();
	void LocaleL();
	void PrintersL();
	void AliveL();
	void AppsL();
	TInt FindApp(const TDesC& aName, TUid& aUid, TFileName& aDll, TDes& aCaption);
	TInt FindTask(const TDesC& aName, TInt& aWgId);
	static TBool IsCoreProcess(const TDesC& aName);
	LxShared* iShared;                      // (while a request is being done)
	TUint iSeq;                             // its epoc_req_seq
	LxeHeader iReq;
	const TUint8* iArgs;
	TInt iArgLen;
	TPtr8 iOut;                             // the answer's text, in the chunk
	TBool iWaiting;                         // a message is up (RunL answers)
	RNotifier iNotifier;
	TBool iNotifierOpen;
	TInt iButton;
	TBuf<80> iLine1, iLine2, iBut1, iBut2;
	RWsSession iWs;
	TBool iWsOpen;
	TCallBack iAnswered;
	};

// UTF-8 <-> EPOC's own 8-bit text (Windows-1252, as ER5's fonts have it)
void LxUtf8ToEpoc(const TDesC8& aIn, TDes& aOut);
void LxEpocToUtf8(const TDesC& aIn, TDes8& aOut);

#endif
