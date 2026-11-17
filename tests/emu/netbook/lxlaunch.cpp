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

const TUid KUidLauncher = { 0x10000CEA };

class CLlAppUi : public CEikAppUi
	{
public:
	void ConstructL()
		{
		BaseConstructL();
		RFs& fs = iEikonEnv->FsSession();
		CFileMan* fm = CFileMan::NewL(fs);
		const TText* const KFrom[] = { _S("Z:\\Test\\PsiLinux\\"), _S("Z:\\Test\\Libs\\"), _S("Z:\\Test\\Linux\\"), _S("Z:\\Test\\Documents\\") };
		const TText* const KTo[] = { _S("C:\\System\\Apps\\PsiLinux\\"), _S("C:\\System\\Libs\\"), _S("C:\\Linux\\"), _S("C:\\Documents\\") };
		for (TInt i = 0; i < 4; i++)
			{
			fs.MkDirAll(TPtrC(KTo[i]));
			fm->Copy(TPtrC(KFrom[i]), TPtrC(KTo[i]), CFileMan::EOverWrite | CFileMan::ERecurse);
			}
		delete fm;
		// (copied files keep the ROM's read-only attribute: Linux's root must be writable)
		fs.SetAtt(_L("C:\\Linux\\ROOT.IMG"), 0, KEntryAttReadOnly);
		fs.SetAtt(_L("C:\\Linux\\Alpine\\ROOT.IMG"), 0, KEntryAttReadOnly);    // (a small Alpine root, if there: mkalpine.sh)
		CApaCommandLine* cl = CApaCommandLine::NewLC();
		cl->SetLibraryNameL(_L("C:\\System\\Apps\\PsiLinux\\PsiLinux.app"));
		cl->SetCommandL(EApaCommandRun);
		TRAPD(err, EikDll::StartAppL(*cl));
		CleanupStack::PopAndDestroy();
		iIdle = CIdle::NewL(CActive::EPriorityIdle);
		iIdle->Start(TCallBack(Close, this));
		}
	~CLlAppUi() { delete iIdle; }
	static TInt Close(TAny* aSelf)
		{
		((CLlAppUi*)aSelf)->Exit();
		return 0;
		}
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
