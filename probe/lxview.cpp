// lxview.cpp - the PsiLinux probe's window: starts lxprobe.exe (from the
// app's own folder) and shows the end of its log, D:\LxProbe.log (or C:),
// twice a second. (An app is a DLL and may not have writable static data,
// so the probes themselves are in the exe.)
#include <eikenv.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikappui.h>
#include <coecntrl.h>
#include <f32file.h>
#include <eikcmds.hrh>

const TUid KUidLxProbe={0x0100FA12};

class CLxView : public CCoeControl
	{
public:
	void ConstructL(const TRect& aRect)
		{
		CreateWindowL();
		SetRectL(aRect);
		ActivateL();
		}
	void SetText(const TDesC8& aText)
		{
		iText.Copy(aText.Right(Min(aText.Length(),iText.MaxLength())));
		DrawNow();
		}
private:
	void Draw(const TRect&) const
		{
		CWindowGc& gc=SystemGc();
		gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		gc.SetBrushColor(KRgbWhite);
		gc.Clear(Rect());
		const CFont* f=iCoeEnv->NormalFont();
		gc.UseFont(f);
		gc.SetPenColor(KRgbBlack);
		TInt h=f->HeightInPixels()+1;
		TInt rows=Rect().Height()/h;
		// the last rows lines
		TInt n=0, i=iText.Length();
		while (i>0 && n<=rows)
			{
			i--;
			if (iText[i]=='\n')
				n++;
			}
		TPtrC rest(iText.Mid(i));
		TInt y=0;
		while (rest.Length())
			{
			TInt nl=rest.Locate('\n');
			TPtrC line(nl<0 ? rest : rest.Left(nl));
			rest.Set(nl<0 ? TPtrC() : rest.Mid(nl+1));
			if (line.Length() && line[line.Length()-1]=='\r')
				line.Set(line.Left(line.Length()-1));
			if (line.Length()==0 && y==0)
				continue;
			y+=h;
			gc.DrawText(line,TPoint(2,y-2));
			}
		gc.DiscardFont();
		}
	TBuf<2400> iText;
	};

class CLxAppUi : public CEikAppUi
	{
public:
	void ConstructL()
		{
		BaseConstructL();
		iView=new(ELeave) CLxView;
		iView->ConstructL(ClientRect());
		// lxprobe.exe beside this app
		TParse p;
		p.Set(_L("lxprobe.exe"),&Application()->AppFullName(),NULL);
		iLogName=_L("C:\\LxProbe.log");
		TDriveInfo d;
		if (iCoeEnv->FsSession().Drive(d,EDriveD)==KErrNone && d.iType!=EMediaNotPresent)
			iLogName[0]='D';
		iCoeEnv->FsSession().Delete(iLogName);
		TInt r=iProc.Create(p.FullName(),KNullDesC);
		if (r==KErrNone)
			{
			iProc.Resume();
			iStarted=ETrue;
			}
		else
			{
			TBuf8<100> m;
			m.Format(_L8("Could not start lxprobe.exe: %d\n"),r);
			iView->SetText(m);
			}
		iTimer=CPeriodic::NewL(CActive::EPriorityStandard);
		iTimer->Start(500000,500000,TCallBack(Tick,this));
		}
	~CLxAppUi()
		{
		delete iTimer;
		if (iStarted)
			iProc.Close();
		delete iView;
		}
private:
	static TInt Tick(TAny* aSelf)
		{
		CLxAppUi* s=(CLxAppUi*)aSelf;
		RFile f;
		if (f.Open(s->iCoeEnv->FsSession(),s->iLogName,EFileRead|EFileShareAny)==KErrNone)
			{
			TInt size=0;
			f.Size(size);
			TBuf8<2400> b;
			f.Read(Max(0,size-b.MaxLength()),b);
			f.Close();
			s->iView->SetText(b);
			}
		return ETrue;
		}
	void HandleCommandL(TInt aCommand)
		{
		if (aCommand==EEikCmdExit)
			Exit();
		}
	CLxView* iView;
	CPeriodic* iTimer;
	RProcess iProc;
	TBool iStarted;
	TFileName iLogName;
	};

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
	TUid AppDllUid() const { return KUidLxProbe; }
	};

EXPORT_C CApaApplication* NewApplication()
	{
	return new CLxApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
