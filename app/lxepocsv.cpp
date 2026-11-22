// lxepocsv.cpp - (0.36) EPOC requests, done by PsiLinux.app (lxepocsv.h):
// opening a file in its program, starting or switching to a program, the
// task list and the processes, the clipboard, infoprints, messages, a
// screenshot, the machine's state. Each answers in the shared chunk.
//
// What it will not do: end EPOC's own processes (the kernel, the file and
// window servers, the shell; nor PsiLinux), or end a system process without
// --force. A message (alert, confirm) is RNotifier's, so PsiLinux's window
// goes on drawing while it is up.
#include <e32hal.h>
#include <eikenv.h>
#include <eikappui.h>
#include <eikapp.h>
#include <eikdll.h>
#include <apgcli.h>
#include <apgtask.h>
#include <apgwgnam.h>
#include <apacmdln.h>
#include <apaid.h>
#include <baclipb.h>
#include <txtetext.h>
#include <s32file.h>
#include <fbs.h>
#include <coemain.h>
#include "lxepocsv.h"

// (ER5's TThreadId and TProcessId keep their numbers to themselves)
static TUint TidNum(TThreadId aId) { TUint v; Mem::Copy(&v, &aId, sizeof v); return v; }
static TUint PidNum(TProcessId aId) { TUint v; Mem::Copy(&v, &aId, sizeof v); return v; }
static TThreadId TidOf(TUint aNum) { TThreadId t; Mem::Copy(&t, &aNum, sizeof aNum); return t; }
static TProcessId PidOf(TUint aNum) { TProcessId p; Mem::Copy(&p, &aNum, sizeof aNum); return p; }

// EPOC's 8-bit text is Windows-1252: 0x80-0x9f are these (0: none)
static const TUint16 KCp1252[32] = {
	0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
	0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178 };

void LxUtf8ToEpoc(const TDesC8& aIn, TDes& aOut)
	{
	aOut.SetLength(0);
	TInt i = 0, n = aIn.Length();
	while (i < n && aOut.Length() < aOut.MaxLength())
		{
		TUint c = aIn[i++];
		if (c >= 0x80)
			{
			TInt more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
			c &= more == 3 ? 0x07 : more == 2 ? 0x0f : 0x1f;
			while (more-- > 0 && i < n && (aIn[i] & 0xc0) == 0x80)
				c = (c << 6) | (aIn[i++] & 0x3f);
			}
		if (c == '\n')
			c = CEditableText::EParagraphDelimiter;   // (callers that want '\n' change it back)
		TUint e = '?';
		if (c < 0x80 || (c >= 0xa0 && c <= 0xff))
			e = c;
		else
			for (TInt k = 0; k < 32; k++)
				if (KCp1252[k] == c)
					e = 0x80 + k;
		aOut.Append(TChar(e));
		}
	}

void LxEpocToUtf8(const TDesC& aIn, TDes8& aOut)
	{
	for (TInt i = 0; i < aIn.Length(); i++)
		{
		TUint c = aIn[i] & 0xff;
		if (c == CEditableText::EParagraphDelimiter || c == CEditableText::ELineBreak)
			c = '\n';
		else if (c >= 0x80 && c < 0xa0)
			c = KCp1252[c - 0x80] ? KCp1252[c - 0x80] : '?';
		TInt need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
		if (aOut.Length() + need > aOut.MaxLength())
			return;
		if (need == 1)
			aOut.Append(TChar(c));
		else if (need == 2)
			{
			aOut.Append(TChar(0xc0 | (c >> 6)));
			aOut.Append(TChar(0x80 | (c & 0x3f)));
			}
		else
			{
			aOut.Append(TChar(0xe0 | (c >> 12)));
			aOut.Append(TChar(0x80 | ((c >> 6) & 0x3f)));
			aOut.Append(TChar(0x80 | (c & 0x3f)));
			}
		}
	}

// ---------------------------------------------------------------------------

CLxEpoc::CLxEpoc() : CActive(EPriorityStandard), iOut(0, 0)
	{
	}

CLxEpoc* CLxEpoc::NewL()
	{
	CLxEpoc* self = new(ELeave) CLxEpoc;
	CleanupStack::PushL(self);
	self->ConstructL();
	CleanupStack::Pop();
	return self;
	}

void CLxEpoc::ConstructL()
	{
	CActiveScheduler::Add(this);
	User::LeaveIfError(iWs.Connect());
	iWsOpen = ETrue;
	}

CLxEpoc::~CLxEpoc()
	{
	Cancel();
	if (iNotifierOpen)
		iNotifier.Close();
	if (iWsOpen)
		iWs.Close();
	}

TBool CLxEpoc::Serve(LxShared& aShared)
	{
	if (iWaiting || aShared.epoc_req_seq == aShared.epoc_ans_seq)
		return EFalse;
	iShared = &aShared;
	iSeq = aShared.epoc_req_seq;
	TInt len = aShared.epoc_req_len;
	iOut.Set(aShared.epoc_ans + sizeof(LxeHeader), 0, LXE_MAX);
	if (len < (TInt)sizeof(LxeHeader) || len > LX_EPOC_BUF)
		{
		Mem::FillZ(&iReq, sizeof iReq);
		Answer(LXE_FAILED);
		return ETrue;
		}
	Mem::Copy(&iReq, aShared.epoc_req, sizeof iReq);
	iArgs = aShared.epoc_req + sizeof(LxeHeader);
	iArgLen = Min((TInt)iReq.len, len - (TInt)sizeof(LxeHeader));
	TRAPD(err, DoL(iReq, iArgs, iArgLen));
	if (iWaiting)
		return EFalse;                 // (a message is up: RunL answers)
	if (err != KErrNone)
		{
		iOut.SetLength(0);
		Answer(err == KErrNotFound || err == KErrPathNotFound ? LXE_NOTFOUND : err == KErrNotSupported ? LXE_UNSUPPORTED : LXE_FAILED, err);
		}
	return ETrue;
	}

void CLxEpoc::Answer(TInt aStatus, TInt aErr, TUint aArg)
	{
	if (!iShared)
		return;
	LxeHeader h;
	Mem::FillZ(&h, sizeof h);
	h.magic = LXE_MAGIC;
	h.version = LXE_VERSION;
	h.op = iReq.op;
	h.status = aStatus;
	h.err = aErr;
	h.arg = aArg;
	h.len = iOut.Length();
	Mem::Copy(iShared->epoc_ans, &h, sizeof h);
	iShared->epoc_ans_len = sizeof h + iOut.Length();
	iShared->epoc_ans_seq = iSeq;
	iShared = NULL;
	}

void CLxEpoc::Text(const TDesC8& aUtf8)
	{
	TInt n = Min(aUtf8.Length(), iOut.MaxLength() - iOut.Length());
	iOut.Append(aUtf8.Left(n));
	}

void CLxEpoc::TextEpoc(const TDesC& aEpoc)
	{
	TPtr8 rest((TUint8*)iOut.Ptr() + iOut.Length(), 0, iOut.MaxLength() - iOut.Length());
	// (tabs and new lines inside a field would break the lines: spaces)
	TBuf<256> clean;
	for (TInt i = 0; i < aEpoc.Length() && clean.Length() < clean.MaxLength(); i++)
		{
		TChar c = aEpoc[i];
		clean.Append(c == '\t' || c == '\n' || c == '\r' || (TUint)c == CEditableText::EParagraphDelimiter ? TChar(' ') : c);
		}
	LxEpocToUtf8(clean, rest);
	iOut.SetLength(iOut.Length() + rest.Length());
	}

void CLxEpoc::Field(const TDesC& aEpoc, TBool aLast)
	{
	TextEpoc(aEpoc);
	Text(aLast ? _L8("\n") : _L8("\t"));
	}

void CLxEpoc::FieldNum(TInt aNum, TBool aLast)
	{
	TBuf8<16> b;
	b.Num(aNum);
	Text(b);
	Text(aLast ? _L8("\n") : _L8("\t"));
	}

void CLxEpoc::FieldUid(TUid aUid, TBool aLast)
	{
	TBuf8<16> b;
	b.Format(_L8("0x%08X"), aUid.iUid);
	Text(b);
	Text(aLast ? _L8("\n") : _L8("\t"));
	}

void CLxEpoc::Key(const TDesC8& aKey, const TDesC& aValue)
	{
	Text(aKey);
	Text(_L8("\t"));
	TextEpoc(aValue);
	Text(_L8("\n"));
	}

void CLxEpoc::KeyNum(const TDesC8& aKey, TInt aValue)
	{
	TBuf<16> b;
	b.Num(aValue);
	Key(aKey, b);
	}

TPtrC8 CLxEpoc::Arg(TInt aIndex) const
	{
	TInt o = 0, k = 0;
	while (o < iArgLen)
		{
		TInt s = o;
		while (o < iArgLen && iArgs[o])
			o++;
		if (k == aIndex)
			return TPtrC8(iArgs + s, o - s);
		o++;
		k++;
		}
	return TPtrC8();
	}

void CLxEpoc::ArgEpoc(TInt aIndex, TDes& aOut) const
	{
	LxUtf8ToEpoc(Arg(aIndex), aOut);
	}

void CLxEpoc::DoL(const LxeHeader& aReq, const TUint8*, TInt)
	{
	switch (aReq.op)
		{
	case LXE_OP_OPEN: OpenL(); break;
	case LXE_OP_RUN: RunAppL(); break;
	case LXE_OP_TASKS: TasksL(); break;
	case LXE_OP_PS: PsL(); break;
	case LXE_OP_CLOSE: CloseL(); break;
	case LXE_OP_KILL: KillL(); break;
	case LXE_OP_INFO: InfoL(); break;
	case LXE_OP_CLIPGET: ClipGetL(); break;
	case LXE_OP_CLIPSET: ClipSetL(); break;
	case LXE_OP_NOTE: NoteL(); break;
	case LXE_OP_BEEP: BeepL(); break;
	case LXE_OP_ALERT: AlertL(EFalse); break;
	case LXE_OP_CONFIRM: AlertL(ETrue); break;
	case LXE_OP_SHOT: ShotL(); break;
	case LXE_OP_TIME: TimeL(); break;
	case LXE_OP_LOCALE: LocaleL(); break;
	case LXE_OP_PRINTERS: PrintersL(); break;
	case LXE_OP_ALIVE: AliveL(); break;
	case LXE_OP_APPS: AppsL(); break;
	case LXE_OP_PRINT:
		Text(_L8("EPOC has no \"print this file\": each program prints its own documents. Open it (epoc open) and print from its File menu, to a printer or to a file"));
		Answer(LXE_UNSUPPORTED);
		break;
	default:
		Answer(LXE_BADVERSION);
		break;
		}
	}

// ---- programs ---------------------------------------------------------------

// A program by caption ("Word"), its file's name ("Word", "Word.app"), its
// UID ("0x1000007f") or the .app's full name
TInt CLxEpoc::FindApp(const TDesC& aName, TUid& aUid, TFileName& aDll, TDes& aCaption)
	{
	RApaLsSession ls;
	TInt r = ls.Connect();
	if (r != KErrNone)
		return r;
	TUint uidv = 0;
	TBool byUid = EFalse;
	if (aName.Length() > 2 && aName[0] == '0' && (aName[1] == 'x' || aName[1] == 'X'))
		{
		TLex lex(aName.Mid(2));
		byUid = lex.Val(uidv, EHex) == KErrNone && lex.Eos();
		}
	r = ls.GetAllApps();
	TApaAppInfo info;
	TInt found = KErrNotFound;
	while (r == KErrNone && found != KErrNone && (r = ls.GetNextApp(info)) == KErrNone)
		{
		TParsePtrC p(info.iFullName);
		if ((byUid && (TUint)info.iUid.iUid == uidv) || info.iCaption.CompareF(aName) == 0 || p.Name().CompareF(aName) == 0
			|| p.NameAndExt().CompareF(aName) == 0 || info.iFullName.CompareF(aName) == 0)
			{
			aUid = info.iUid;
			aDll = info.iFullName;
			aCaption.Copy(info.iCaption.Left(Min(info.iCaption.Length(), aCaption.MaxLength())));
			found = KErrNone;
			}
		}
	ls.Close();
	return found;
	}

// a task (a window group on the task list) by caption, 0xUID or thread id
TInt CLxEpoc::FindTask(const TDesC& aName, TInt& aWgId)
	{
	CArrayFixFlat<TInt>* wgs = new(ELeave) CArrayFixFlat<TInt>(8);
	CleanupStack::PushL(wgs);
	User::LeaveIfError(iWs.WindowGroupList(wgs));
	TUint num = 0;
	TBool isNum = EFalse, isUid = EFalse;
	if (aName.Length() > 2 && aName[0] == '0' && (aName[1] == 'x' || aName[1] == 'X'))
		{
		TLex lex(aName.Mid(2));
		isUid = lex.Val(num, EHex) == KErrNone && lex.Eos();
		}
	else
		{
		TLex lex(aName);
		isNum = lex.Val(num) == KErrNone && lex.Eos();
		}
	TInt found = KErrNotFound;
	for (TInt i = 0; i < wgs->Count() && found != KErrNone; i++)
		{
		CApaWindowGroupName* wn = CApaWindowGroupName::NewLC(iWs, (*wgs)[i]);
		TThreadId tid;
		iWs.GetWindowGroupClientThreadId((*wgs)[i], tid);
		if (wn->Caption().Length() && ((isUid && (TUint)wn->AppUid().iUid == num) || (isNum && TidNum(tid) == num)
			|| wn->Caption().CompareF(aName) == 0))
			{
			aWgId = (*wgs)[i];
			found = KErrNone;
			}
		CleanupStack::PopAndDestroy();       // wn
		}
	CleanupStack::PopAndDestroy();           // wgs
	return found;
	}

void CLxEpoc::OpenL()
	{
	TFileName doc;
	ArgEpoc(0, doc);
	TEntry e;
	if (CEikonEnv::Static()->FsSession().Entry(doc, e) != KErrNone)
		{
		Text(_L8("no such file"));
		Answer(LXE_NOTFOUND, KErrNotFound);
		return;
		}
	// (as the System screen does: a document that is open already comes to the front)
	TApaTaskList tl(iWs);
	TApaTask t = tl.FindDoc(doc);
	TThreadId tid;
	TUid uid = KNullUid;
	if (t.Exists())
		{
		if (!(iReq.flags & LXE_F_BG))
			t.BringToForeground();
		tid = t.ThreadId();
		}
	else
		{
		RApaLsSession ls;
		User::LeaveIfError(ls.Connect());
		CleanupClosePushL(ls);
		TDataType dt;
		ls.AppForDocument(doc, uid, dt);
		if (uid == KNullUid)
			{
			CleanupStack::PopAndDestroy();   // ls
			Text(_L8("no program opens that file"));
			Answer(LXE_NOTFOUND, KErrNotFound);
			return;
			}
		User::LeaveIfError(ls.StartDocument(doc, tid));
		CleanupStack::PopAndDestroy();       // ls
		}
	TApaAppInfo info;
	RApaLsSession ls2;
	if (uid != KNullUid && ls2.Connect() == KErrNone)
		{
		if (ls2.GetAppInfo(info, uid) == KErrNone)
			{
			FieldUid(uid);
			Field(info.iCaption, ETrue);
			}
		ls2.Close();
		}
	Answer(LXE_OK, 0, TidNum(tid));
	}

void CLxEpoc::RunAppL()
	{
	TFileName name, doc;
	ArgEpoc(0, name);
	ArgEpoc(1, doc);
	TUid uid;
	TFileName dll;
	TBuf<64> caption;
	// a program file (an .exe, or an .app named by its path). (TParse, not
	// TParsePtrC, which panics on a name EPOC cannot parse: the name is
	// whatever was typed)
	TParse pp;
	TBool parsed = pp.Set(name, NULL, NULL) == KErrNone;
	if (FindApp(name, uid, dll, caption) != KErrNone)
		{
		if (parsed && pp.Ext().CompareF(_L(".exe")) == 0)
			{
			TThreadId tid = EikDll::StartExeL(name);
			Field(name, ETrue);
			Answer(LXE_OK, 0, TidNum(tid));
			return;
			}
		Text(_L8("no program of that name (epoc apps lists them)"));
		Answer(LXE_NOTFOUND, KErrNotFound);
		return;
		}
	TApaTaskList tl(iWs);
	TApaTask t = tl.FindApp(uid);
	TBool switched = EFalse;
	TThreadId tid;
	if (t.Exists() && !(iReq.flags & LXE_F_NEW))
		{
		if (doc.Length())
			User::LeaveIfError(t.SwitchOpenFile(doc));
		if (!(iReq.flags & LXE_F_BG))
			t.BringToForeground();
		tid = t.ThreadId();
		switched = ETrue;
		}
	else
		{
		CApaCommandLine* cl = CApaCommandLine::NewLC();
		cl->SetLibraryNameL(dll);
		if (doc.Length())
			{
			cl->SetDocumentNameL(doc);
			cl->SetCommandL(EApaCommandOpen);
			}
		else
			cl->SetCommandL((iReq.flags & LXE_F_BG) ? EApaCommandBackground : EApaCommandRun);
		tid = EikDll::StartAppL(*cl);
		CleanupStack::PopAndDestroy();       // cl
		}
	FieldUid(uid);
	Field(caption);
	Text(switched ? _L8("switched\n") : _L8("started\n"));
	Answer(LXE_OK, 0, TidNum(tid));
	}

void CLxEpoc::AppsL()
	{
	RApaLsSession ls;
	User::LeaveIfError(ls.Connect());
	CleanupClosePushL(ls);
	TInt r = ls.GetAllApps();
	TApaAppInfo info;
	while (r == KErrNone && ls.GetNextApp(info) == KErrNone)
		{
		FieldUid(info.iUid);
		Field(info.iCaption);
		Field(info.iFullName, ETrue);
		}
	CleanupStack::PopAndDestroy();           // ls
	Answer(LXE_OK);
	}

void CLxEpoc::TasksL()
	{
	CArrayFixFlat<TInt>* wgs = new(ELeave) CArrayFixFlat<TInt>(8);
	CleanupStack::PushL(wgs);
	User::LeaveIfError(iWs.WindowGroupList(wgs));
	TInt focus = iWs.GetFocusWindowGroup();
	for (TInt i = 0; i < wgs->Count(); i++)
		{
		CApaWindowGroupName* wn = CApaWindowGroupName::NewLC(iWs, (*wgs)[i]);
		if (wn->Caption().Length())
			{
			TThreadId tid;
			iWs.GetWindowGroupClientThreadId((*wgs)[i], tid);
			FieldNum(TidNum(tid));
			FieldUid(wn->AppUid());
			Field(wn->Caption());
			Field(wn->DocName());
			Text((*wgs)[i] == focus ? _L8("fg\n") : _L8("\n"));
			}
		CleanupStack::PopAndDestroy();       // wn
		}
	CleanupStack::PopAndDestroy();           // wgs
	Answer(LXE_OK);
	}

// EPOC itself: never ended, even with --force
TBool CLxEpoc::IsCoreProcess(const TDesC& aName)
	{
	static const TText* const KCore[] = { _S("EKern"), _S("EFile"), _S("EWSrv"), _S("FbServ"), _S("Shell"), _S("Sys$Shell"),
		_S("C32Exe"), _S("AppArc"), _S("EikSrv"), _S("Ekern.exe"), 0 };
	TInt b = aName.Locate('[');
	TPtrC n(b > 0 ? aName.Left(b) : TPtrC(aName));
	for (TInt i = 0; KCore[i]; i++)
		if (n.CompareF(TPtrC(KCore[i])) == 0)
			return ETrue;
	return EFalse;
	}

void CLxEpoc::PsL()
	{
	TFindProcess fp;
	TFullName full;
	while (fp.Next(full) == KErrNone)
		{
		RProcess p;
		if (p.Open(fp) != KErrNone)
			continue;
		TInt b = full.Locate('[');
		TPtrC name(b > 0 ? full.Left(b) : TPtrC(full));
		// memory: the chunks whose names start with the process's
		TInt mem = 0;
		TFullName cm(full);
		cm.Append(_L("*"));
		TFindChunk fc(cm);
		TFullName chunk;
		while (fc.Next(chunk) == KErrNone)
			{
			RChunk c;
			if (c.Open(fc) == KErrNone)
				{
				mem += c.Size();
				c.Close();
				}
			}
		FieldNum(PidNum(p.Id()));
		Field(name);
		FieldUid(p.Type()[2]);
		FieldNum(mem / 1024);
		FieldNum(p.Priority());
		Field(p.FileName());
		Text(p.System() || p.Protected() || IsCoreProcess(full) ? _L8("sys\n") : _L8("\n"));
		p.Close();
		}
	Answer(LXE_OK);
	}

void CLxEpoc::CloseL()
	{
	TBuf<64> name;
	ArgEpoc(0, name);
	TInt wg;
	if (FindTask(name, wg) != KErrNone)
		{
		Text(_L8("no such program on the task list (epoc tasks)"));
		Answer(LXE_NOTFOUND, KErrNotFound);
		return;
		}
	CApaWindowGroupName* wn = CApaWindowGroupName::NewLC(iWs, wg);
	TBool us = wn->AppUid() == CEikonEnv::Static()->EikAppUi()->Application()->AppDllUid();
	CleanupStack::PopAndDestroy();
	if (us)
		{
		Text(_L8("that is PsiLinux: type exit to close it"));
		Answer(LXE_REFUSED);
		return;
		}
	TApaTask t(iWs);
	t.SetWgId(wg);
	t.EndTask();
	Answer(LXE_OK);
	}

void CLxEpoc::KillL()
	{
	TBuf<64> name;
	ArgEpoc(0, name);
	TUint id = 0;
	TLex lex(name);
	TBool isNum = lex.Val(id) == KErrNone && lex.Eos();
	TFindProcess fp;
	TFullName full;
	RProcess me;
	TInt found = 0;
	while (fp.Next(full) == KErrNone)
		{
		RProcess p;
		if (p.Open(fp) != KErrNone)
			continue;
		TInt b = full.Locate('[');
		TPtrC n(b > 0 ? full.Left(b) : TPtrC(full));
		if ((isNum && PidNum(p.Id()) == id) || (!isNum && n.CompareF(name) == 0))
			{
			found++;
			TParsePtrC fn(p.FileName());
			if (p.Id() == me.Id() || IsCoreProcess(full) || fn.Name().CompareF(_L("psilxrun")) == 0)
				{
				Text(_L8("that is part of EPOC (or PsiLinux): never ended"));
				p.Close();
				Answer(LXE_REFUSED);
				return;
				}
			if ((p.System() || p.Protected()) && !(iReq.flags & LXE_F_FORCE))
				{
				Text(_L8("a system process: --force to end it anyway"));
				p.Close();
				Answer(LXE_REFUSED);
				return;
				}
			p.Kill(KErrNone);
			}
		p.Close();
		}
	if (!found)
		{
		Text(_L8("no such process (epoc ps)"));
		Answer(LXE_NOTFOUND, KErrNotFound);
		return;
		}
	Answer(LXE_OK);
	}

void CLxEpoc::AliveL()
	{
	RThread t;
	TBool alive = EFalse;
	if (t.Open(TidOf(iReq.arg)) == KErrNone)
		{
		alive = t.ExitType() == EExitPending;
		t.Close();
		}
	else
		{
		RProcess p;
		if (p.Open(PidOf(iReq.arg)) == KErrNone)
			{
			alive = p.ExitType() == EExitPending;
			p.Close();
			}
		}
	Answer(alive ? LXE_OK : LXE_NOTFOUND);
	}

// ---- the machine -------------------------------------------------------------

// Coda's battery gauge (PsiBatt), if it is there: C:\System\Data\PsiBatt.dat
static TInt CodaBattery(RFs& aFs, TInt& aTenths, TInt& aMode, TInt& aRate)
	{
	RFile f;
	if (f.Open(aFs, _L("C:\\System\\Data\\PsiBatt.dat"), EFileRead | EFileShareAny) != KErrNone)
		return KErrNotFound;
	TBuf8<128> b;
	TInt r = f.Read(b);
	f.Close();
	if (r != KErrNone || b.Length() < 44)
		return KErrNotFound;
	const TInt* w = (const TInt*)b.Ptr();
	if (w[0] != 0x31427350 || w[3] < 0 || w[3] > 1000)     // "PsB1"; iShown
		return KErrNotFound;
	aTenths = w[3];
	aMode = w[4];
	aRate = w[10];
	return KErrNone;
	}

void CLxEpoc::InfoL()
	{
	RFs& fs = CEikonEnv::Static()->FsSession();
	TMachineInfoV1Buf mb;
	UserHal::MachineInfo(mb);
	TMachineInfoV1& m = mb();
	Key(_L8("machine"), m.iMachineName);
	TBuf<32> v;
	v.Format(_L("%d.%02d(%d)"), m.iRomVersion.iMajor, m.iRomVersion.iMinor, m.iRomVersion.iBuild);
	Key(_L8("rom"), v);
	// Coda: Z:\System\Data\PsiEdition.txt (its first two lines)
	{
		RFile f;
		TBuf<160> ed;
		if (f.Open(fs, _L("Z:\\System\\Data\\PsiEdition.txt"), EFileRead | EFileShareReadersOnly) == KErrNone)
			{
			TBuf8<160> b;
			f.Read(b);
			f.Close();
			// (its lines joined with "; ")
			for (TInt i = 0; i < b.Length() && ed.Length() < ed.MaxLength() - 2; i++)
				{
				if (b[i] == '\r')
					continue;
				if (b[i] == '\n')
					{
					if (i + 1 < b.Length())
						ed.Append(_L("; "));
					}
				else
					ed.Append(TChar(b[i]));
				}
			}
		Key(_L8("edition"), ed);
	}
	Key(_L8("cpu"), m.iProcessorName);
	KeyNum(_L8("cpu_khz"), m.iProcessorClockInKHz);
	v.Format(_L("%dx%d"), m.iDisplaySizeInPixels.iWidth, m.iDisplaySizeInPixels.iHeight);
	Key(_L8("screen"), v);
	KeyNum(_L8("colours"), m.iMaximumDisplayColors);
	TMemoryInfoV1Buf memb;
	UserHal::MemoryInfo(memb);
	KeyNum(_L8("ram_kb"), memb().iTotalRamInBytes / 1024);
	KeyNum(_L8("free_kb"), memb().iFreeRamInBytes / 1024);
	TSupplyInfoV1Buf sb;
	UserHal::SupplyInfo(sb);
	TSupplyInfoV1& s = sb();
	TInt tenths, mode, rate;
	if (CodaBattery(fs, tenths, mode, rate) == KErrNone)
		{
		KeyNum(_L8("battery_percent"), (tenths + 5) / 10);    // (0.48) rounded, as the toolbar (PsiBattShownPercent)
		static const TText* const KModes[] = { _S("battery"), _S("charging"), _S("topping up"), _S("full") };
		Key(_L8("battery_mode"), TPtrC(KModes[mode >= 0 && mode <= 3 ? mode : 0]));
		if (rate > 0)
			KeyNum(_L8("battery_drain_tenths_per_hour"), rate);
		}
	static const TText* const KStatus[] = { _S("empty"), _S("very low"), _S("low"), _S("good") };
	Key(_L8("battery_status"), TPtrC(KStatus[s.iMainBatteryStatus >= EZero && s.iMainBatteryStatus <= EGood ? s.iMainBatteryStatus : 0]));
	KeyNum(_L8("battery_mv"), s.iMainBatteryMilliVolts);
	KeyNum(_L8("battery_max_mv"), s.iMainBatteryMaxMilliVolts);
	Key(_L8("mains"), s.iExternalPowerPresent ? _L("yes") : _L("no"));
	KeyNum(_L8("backup_mv"), s.iBackupBatteryMilliVolts);
	// the disks: letter, name, size and free in KB, kind
	TDriveList dl;
	if (fs.DriveList(dl) == KErrNone)
		for (TInt d = 0; d < KMaxDrives; d++)
			{
			if (!dl[d])
				continue;
			TVolumeInfo vi;
			if (fs.Volume(vi, d) != KErrNone)
				continue;
			TChar c;
			RFs::DriveToChar(d, c);
			Text(_L8("disk\t"));
			TBuf<2> l;
			l.Append(c);
			Field(l);
			Field(vi.iName);
			FieldNum((vi.iSize / TInt64(1024)).Low());
			FieldNum((vi.iFree / TInt64(1024)).Low());
			Field(vi.iDrive.iType == EMediaRom ? _L("rom") : vi.iDrive.iType == EMediaRam ? _L("internal") :
				vi.iDrive.iType == EMediaFlash ? _L("card") : vi.iDrive.iType == EMediaHardDisk ? _L("card") : _L("other"), ETrue);
			}
	Answer(LXE_OK);
	}

static void DateTime(TDes& aOut, const TTime& aTime)
	{
	TDateTime d = aTime.DateTime();
	aOut.Format(_L("%04d-%02d-%02d %02d:%02d:%02d"), d.Year(), d.Month() + 1, d.Day() + 1, d.Hour(), d.Minute(), d.Second());
	}

void CLxEpoc::TimeL()
	{
	TTime home, utc;
	home.HomeTime();
	TLocale loc;
	TInt off = loc.UniversalTimeOffset().Int();
	utc = home - TTimeIntervalSeconds(off);
	TBuf<40> b;
	DateTime(b, home);
	Key(_L8("local"), b);
	DateTime(b, utc);
	Key(_L8("utc"), b);
	TInt a = off < 0 ? -off : off;
	b.Format(_L("%c%02d%02d"), off < 0 ? '-' : '+', a / 3600, (a / 60) % 60);
	Key(_L8("offset"), b);
	Key(_L8("dst"), loc.QueryHomeHasDaylightSavingOn() ? _L("yes") : _L("no"));
	TTime epoch(TDateTime(1970, EJanuary, 0, 0, 0, 0, 0));
	TTimeIntervalSeconds secs;
	utc.SecondsFrom(epoch, secs);
	KeyNum(_L8("epoch"), secs.Int());
	Answer(LXE_OK);
	}

void CLxEpoc::LocaleL()
	{
	TLocale loc;
	KeyNum(_L8("country"), loc.CountryCode());
	KeyNum(_L8("language"), User::Language());
	static const TText* const KDate[] = { _S("mm/dd/yyyy"), _S("dd/mm/yyyy"), _S("yyyy/mm/dd") };
	Key(_L8("date_format"), TPtrC(KDate[loc.DateFormat() <= EDateJapanese ? loc.DateFormat() : 1]));
	TBuf<4> c;
	c.Append(loc.DateSeparator(1));
	Key(_L8("date_separator"), c);
	c.Zero();
	c.Append(loc.TimeSeparator(1));
	Key(_L8("time_separator"), c);
	Key(_L8("clock"), loc.TimeFormat() == ETime24 ? _L("24") : _L("12"));
	c.Zero();
	c.Append(loc.DecimalSeparator());
	Key(_L8("decimal"), c);
	c.Zero();
	c.Append(loc.ThousandsSeparator());
	Key(_L8("thousands"), c);
	TCurrencySymbol cur;
	cur.Set();
	Key(_L8("currency"), cur);
	Key(_L8("units"), loc.UnitsGeneral() == EUnitsImperial ? _L("imperial") : _L("metric"));
	static const TText* const KDays[] = { _S("monday"), _S("tuesday"), _S("wednesday"), _S("thursday"), _S("friday"), _S("saturday"), _S("sunday") };
	Key(_L8("week_starts"), TPtrC(KDays[loc.StartOfWeek() <= ESunday ? loc.StartOfWeek() : 0]));
	Answer(LXE_OK);
	}

void CLxEpoc::PrintersL()
	{
	RFs& fs = CEikonEnv::Static()->FsSession();
	TDriveList dl;
	User::LeaveIfError(fs.DriveList(dl));
	for (TInt d = 0; d < KMaxDrives; d++)
		{
		if (!dl[d])
			continue;
		TChar c;
		RFs::DriveToChar(d, c);
		TFileName pat;
		pat.Append(c);
		pat.Append(_L(":\\System\\Printers\\*.pdr"));
		CDir* dir = NULL;
		if (fs.GetDir(pat, KEntryAttNormal, ESortByName, dir) != KErrNone || !dir)
			continue;
		for (TInt i = 0; i < dir->Count(); i++)
			{
			TParsePtrC p((*dir)[i].iName);
			Field(p.Name());
			TFileName full(pat.Left(pat.Length() - 5));
			full.Append((*dir)[i].iName);
			Field(full, ETrue);
			}
		delete dir;
		}
	Answer(LXE_OK);
	}

// ---- the clipboard ------------------------------------------------------------

void CLxEpoc::ClipGetL()
	{
	RFs& fs = CEikonEnv::Static()->FsSession();
	CClipboard* cb = NULL;
	TRAPD(r, cb = CClipboard::NewForReadingL(fs));
	if (r != KErrNone || !cb)
		{
		Answer(LXE_OK);                      // (nothing on it)
		return;
		}
	CleanupStack::PushL(cb);
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	TRAP(r, text->PasteFromStoreL(cb->Store(), cb->StreamDictionary(), 0));
	TInt len = Min(text->DocumentLength(), LXE_MAX / 3);
	HBufC* buf = HBufC::NewLC(len);
	TPtr p = buf->Des();
	text->Extract(p, 0, len);
	TPtr8 rest((TUint8*)iOut.Ptr() + iOut.Length(), 0, iOut.MaxLength() - iOut.Length());
	LxEpocToUtf8(p, rest);
	iOut.SetLength(iOut.Length() + rest.Length());
	CleanupStack::PopAndDestroy(3);          // buf, text, cb
	Answer(LXE_OK);
	}

void CLxEpoc::ClipSetL()
	{
	RFs& fs = CEikonEnv::Static()->FsSession();
	TPtrC8 a = Arg(0);
	HBufC* buf = HBufC::NewLC(a.Length() + 1);
	TPtr p = buf->Des();
	LxUtf8ToEpoc(a, p);
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	// (a trailing new line is the shell's, not the text's)
	if (p.Length() && (TUint)p[p.Length() - 1] == CEditableText::EParagraphDelimiter)
		p.SetLength(p.Length() - 1);
	text->InsertL(0, p);
	CClipboard* cb = CClipboard::NewForWritingLC(fs);
	text->CopyToStoreL(cb->Store(), cb->StreamDictionary(), 0, text->DocumentLength());
	cb->CommitL();
	CleanupStack::PopAndDestroy(3);          // cb, text, buf
	Answer(LXE_OK);
	}

// ---- talking to the user -------------------------------------------------------

void CLxEpoc::NoteL()
	{
	TBuf<120> t;
	ArgEpoc(0, t);
	for (TInt i = 0; i < t.Length(); i++)
		if ((TUint)t[i] == CEditableText::EParagraphDelimiter)
			t[i] = ' ';
	CEikonEnv::Static()->InfoMsg(t);
	Answer(LXE_OK);
	}

void CLxEpoc::BeepL()
	{
	TPtrC8 ms = Arg(0);
	TInt msv = 200;
	TLex8 lex(ms);
	lex.Val(msv);
	if (msv < 10 || msv > 5000)
		msv = 200;
	if (iReq.arg >= 50 && iReq.arg <= 10000)
		User::Beep(iReq.arg, TTimeIntervalMicroSeconds32(msv * 1000));
	else
		CEikonEnv::Beep();
	Answer(LXE_OK);
	}

void CLxEpoc::AlertL(TBool aConfirm)
	{
	if (!iNotifierOpen)
		{
		User::LeaveIfError(iNotifier.Connect());
		iNotifierOpen = ETrue;
		}
	ArgEpoc(0, iLine1);
	ArgEpoc(1, iLine2);
	ArgEpoc(2, iBut2);                       // (lxepoc.h: yes, then no; Yes is the second button)
	ArgEpoc(3, iBut1);
	for (TInt i = 0; i < iLine2.Length(); i++)
		if ((TUint)iLine2[i] == CEditableText::EParagraphDelimiter)
			iLine2[i] = ' ';
	if (aConfirm)
		{
		if (!iBut1.Length())
			iBut1 = _L("No");
		if (!iBut2.Length())
			iBut2 = _L("Yes");
		}
	else
		{
		iBut1 = _L("Continue");
		iBut2.Zero();
		}
	iButton = 0;
	iNotifier.Notify(iLine1, iLine2, iBut1, iBut2, iButton, iStatus);
	SetActive();
	iWaiting = ETrue;
	}

void CLxEpoc::RunL()
	{
	iWaiting = EFalse;
	if (!iShared)
		return;
	if (iStatus.Int() != KErrNone)
		Answer(LXE_FAILED, iStatus.Int());
	else
		Answer(LXE_OK, 0, iReq.op == LXE_OP_CONFIRM && iButton == 1 ? 1 : 0);   // (the second button: Yes)
	iAnswered.CallBack();
	}

void CLxEpoc::DoCancel()
	{
	// (RNotifier has no cancel in ER5: the message stays until answered)
	TRequestStatus* s = &iStatus;
	User::RequestComplete(s, KErrCancel);
	}

// ---- a screenshot -----------------------------------------------------------------

typedef TInt (*TPsiShotSave)();

void CLxEpoc::ShotL()
	{
	RFs& fs = CEikonEnv::Static()->FsSession();
	TFileName want;
	ArgEpoc(0, want);
	// Coda's Screenshot (Screenshot.ctl, ordinal 3: saves a PNG in \Screenshots)
	RLibrary lib;
	TInt r = lib.Load(_L("Z:\\System\\Controls\\Screenshot.ctl"));
	if (r != KErrNone)
		{
		TFindFile ff(fs);
		if (ff.FindByDir(_L("\\System\\Controls\\Screenshot.ctl"), KNullDesC) == KErrNone)
			r = lib.Load(ff.File());
		}
	if (r == KErrNone)
		{
		TPsiShotSave save = (TPsiShotSave)lib.Lookup(3);
		TTime before;
		before.UniversalTime();
		r = save ? save() : KErrNotSupported;
		lib.Close();
		if (r == KErrNone)
			{
			// the newest file in D:\Screenshots (or C:'s)
			TFileName newest;
			TTime when(0);
			const TText* const KDirs[] = { _S("D:\\Screenshots\\"), _S("C:\\Screenshots\\") };
			for (TInt k = 0; k < 2; k++)
				{
				CDir* dir = NULL;
				TFileName pat;
				pat = TPtrC(KDirs[k]);
				pat.Append(_L("*.png"));
				if (fs.GetDir(pat, KEntryAttNormal, ESortByDate | EDescending, dir) == KErrNone && dir && dir->Count()
					&& (*dir)[0].iModified > when)
					{
					when = (*dir)[0].iModified;
					newest = TPtrC(KDirs[k]);
					newest.Append((*dir)[0].iName);
					}
				delete dir;
				}
			if (want.Length() && newest.Length())
				{
				CFileMan* fm = CFileMan::NewL(fs);
				CleanupStack::PushL(fm);
				r = fm->Copy(newest, want, CFileMan::EOverWrite);
				CleanupStack::PopAndDestroy();
				if (r == KErrNone)
					newest = want;
				}
			Field(newest, ETrue);
			Answer(r == KErrNone ? LXE_OK : LXE_FAILED, r);
			return;
			}
		}
	// no Coda: the screen as an EPOC picture (.mbm), which Paint and Sketch open
	CWsScreenDevice* sd = CCoeEnv::Static()->ScreenDevice();
	CFbsBitmap* bm = new(ELeave) CFbsBitmap;
	CleanupStack::PushL(bm);
	User::LeaveIfError(bm->Create(sd->SizeInPixels(), sd->DisplayMode()));
	User::LeaveIfError(sd->CopyScreenToBitmap(bm));
	if (!want.Length())
		{
		TTime now;
		now.HomeTime();
		TDateTime d = now.DateTime();
		TDriveInfo di;
		TBool card = fs.Drive(di, EDriveD) == KErrNone && di.iType != EMediaNotPresent;
		want.Format(_L("%c:\\Screenshots\\Screen %04d-%02d-%02d %02d-%02d-%02d.mbm"), card ? 'D' : 'C',
			d.Year(), d.Month() + 1, d.Day() + 1, d.Hour(), d.Minute(), d.Second());
		}
	TParse wp;                               // (not TParsePtrC: it panics on a bad name)
	if (wp.Set(want, NULL, NULL) != KErrNone)
		{
		CleanupStack::PopAndDestroy();       // bm
		Text(_L8("not a file name EPOC can use"));
		Answer(LXE_FAILED, KErrBadName);
		return;
		}
	if (wp.Ext().CompareF(_L(".png")) == 0)
		{
		CleanupStack::PopAndDestroy();       // bm
		Text(_L8("a PNG needs Coda's Screenshot; give a .mbm name (an EPOC picture)"));
		Answer(LXE_UNSUPPORTED);
		return;
		}
	fs.MkDirAll(want);
	r = bm->Save(want);
	CleanupStack::PopAndDestroy();           // bm
	if (r == KErrNone)
		Field(want, ETrue);
	Answer(r == KErrNone ? LXE_OK : LXE_FAILED, r);
	}
