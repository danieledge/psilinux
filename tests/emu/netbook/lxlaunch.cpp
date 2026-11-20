// lxlaunch.cpp - for the netBook emulator only, never a real image. The
// emulator does not mount the card, where PsiLinux and its Linux root live.
// In a scratch test image (nbimage.mts) they are in the ROM under Z:\Test\,
// and this program, built under Jotter's UID and put in Jotter's place so
// that the System screen starts it at boot, copies them where they are
// installed (C:\System\Apps\PsiLinux\, C:\System\Libs\, C:\Linux\, and the
// test documents to C:\Documents\), starts PsiLinux and closes. (The runner
// has writable data, so it cannot run from the ROM in place.)
#include <eikapp.h>
#include <eikdoc.h>
#include <eikappui.h>
#include <eikenv.h>
#include <eikdll.h>
#include <apacmdln.h>
#include <f32file.h>
#include <eikcmds.hrh>
#include <e32hal.h>
#include <apgtask.h>

const TUid KUidLauncher = { 0x10000CEA };

class CLlAppUi : public CEikAppUi
	{
public:
	void ConstructL()
		{
		BaseConstructL();
		RFs& fs = iEikonEnv->FsSession();
		CFileMan* fm = CFileMan::NewL(fs);
		const TText* const KFrom[] = { _S("Z:\\Test\\PsiLinux\\"), _S("Z:\\Test\\Libs\\"), _S("Z:\\Test\\Linux\\"), _S("Z:\\Test\\Documents\\"),
			_S("Z:\\Test\\Data\\") };
		const TText* const KTo[] = { _S("C:\\System\\Apps\\PsiLinux\\"), _S("C:\\System\\Libs\\"), _S("C:\\Linux\\"), _S("C:\\Documents\\"),
			_S("C:\\System\\Data\\") };     // (0.41) NB_PSILINK: Connection settings, PsiLink.ini
		for (TInt i = 0; i < 5; i++)
			{
			fs.MkDirAll(TPtrC(KTo[i]));
			fm->Copy(TPtrC(KFrom[i]), TPtrC(KTo[i]), CFileMan::EOverWrite | CFileMan::ERecurse);
			}
		delete fm;
		// (copied files keep the ROM's read-only attribute: Linux's root must be writable)
		fs.SetAtt(_L("C:\\Linux\\ROOT.IMG"), 0, KEntryAttReadOnly);
		fs.SetAtt(_L("C:\\Linux\\Alpine\\ROOT.IMG"), 0, KEntryAttReadOnly);    // (a small Alpine root, if there: mkalpine.sh)
		// (0.38) the free RAM just before PsiLinux starts, for the tests'
		// measure of what PsiLinux takes (t/t8 reads it as
		// /psion/c/Linux/prefree.txt)
		TMemoryInfoV1Buf mi;
		UserHal::MemoryInfo(mi);
		RFile pf;
		if (pf.Replace(fs, _L("C:\\Linux\\prefree.txt"), EFileWrite) == KErrNone)
			{
			TBuf8<40> t;
			t.Format(_L8("%d KB free\n"), mi().iFreeRamInBytes / 1024);
			pf.Write(t);
			pf.Close();
			}
		// (0.39) the sleep test's helper (lxswitch.cpp), if the image has it
		RProcess sw;
		if (sw.Create(_L("C:\\System\\Apps\\PsiLinux\\lxswitch.exe"), KNullDesC) == KErrNone)
			{
			sw.Resume();
			sw.Close();
			}
		CApaCommandLine* cl = CApaCommandLine::NewLC();
		cl->SetLibraryNameL(_L("C:\\System\\Apps\\PsiLinux\\PsiLinux.app"));
		cl->SetCommandL(EApaCommandRun);
		TRAPD(err, EikDll::StartAppL(*cl));
		CleanupStack::PopAndDestroy();
		// (0.38) with Z:\Test\SENDTRIM in the image (nbimage.mts,
		// NB_SENDTRIM=1): stay, and 20 s on send PsiLinux "sleep", as
		// PsiHub does short of memory (psion-core ssh/psimem.h); then close
		if (fs.Entry(_L("Z:\\Test\\SENDTRIM"), iEntry) == KErrNone)
			{
			iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
			iTimer->Start(10000000, 10000000, TCallBack(SendSleep, this));
			return;
			}
		iIdle = CIdle::NewL(CActive::EPriorityIdle);
		iIdle->Start(TCallBack(Close, this));
		}
	~CLlAppUi() { delete iIdle; delete iTimer; }
	static TInt Close(TAny* aSelf)
		{
		((CLlAppUi*)aSelf)->Exit();
		return 0;
		}
	// (what happened, for the test to show: /psion/c/Linux/sent.txt)
	void Note(const TDesC8& aWhat)
		{
		RFile f;
		RFs& fs = iCoeEnv->FsSession();
		if (f.Open(fs, _L("C:\\Linux\\sent.txt"), EFileWrite) != KErrNone && f.Create(fs, _L("C:\\Linux\\sent.txt"), EFileWrite) != KErrNone)
			return;
		TInt end = 0;
		f.Seek(ESeekEnd, end);
		f.Write(aWhat);
		f.Close();
		}
	static TInt SendSleep(TAny* aSelf)
		{
		CLlAppUi* self = (CLlAppUi*)aSelf;
		if (++self->iTicks < 2)
			{
			TBuf8<20> a;
			a.Format(_L8("%d0 s; "), self->iTicks);
			self->Note(a);
			return 0;
			}
		const TUid psilinux = { 0x01000A87 }, msg = { 0x01000BA0 };
		TApaTaskList tasks(self->iCoeEnv->WsSession());
		TApaTask t = tasks.FindApp(psilinux);
		self->Note(t.Exists() ? _L8("found; ") : _L8("not found; "));
		TInt err = t.Exists() ? t.SendMessage(msg, _L8("sleep")) : KErrNotFound;
		TBuf8<40> l;
		l.Format(_L8("sleep sent: %d"), err);
		self->Note(l);
		self->iTimer->Cancel();
		self->Exit();
		return 0;
		}
	TInt iTicks;
	CPeriodic* iTimer;
	TEntry iEntry;
	void HandleCommandL(TInt aCommand)
		{
		if (aCommand == EEikCmdExit)
			Exit();
		}
	CIdle* iIdle;
	};

class CLlDocument : public CEikDocument
	{
public:
	CLlDocument(CEikApplication& aApp) : CEikDocument(aApp) {}
private:
	CEikAppUi* CreateAppUiL() { return new(ELeave) CLlAppUi; }
	};

class CLlApplication : public CEikApplication
	{
private:
	CApaDocument* CreateDocumentL() { return new(ELeave) CLlDocument(*this); }
	TUid AppDllUid() const { return KUidLauncher; }
	};

EXPORT_C CApaApplication* NewApplication()
	{
	return new CLlApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
