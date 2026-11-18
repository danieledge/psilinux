// lxswitch.cpp - for the netBook emulator only (0.39): PsiLinux's sleep
// test. Started by the test launcher (lxlaunch.cpp) when the image has it
// (nbimage.mts, NB_SLEEPTEST=1). Twice, it sends PsiLinux behind (at 60 s,
// then at 170 s), notes the free RAM and the sleep files' size (in the
// emulator they are on C:, in RAM) 90 s on, then brings PsiLinux back to
// the front; the test root's t7 shows its notes (C:\Linux\switch.txt).
#include <e32std.h>
#include <e32hal.h>
#include <f32file.h>
#include <w32std.h>
#include <apgtask.h>

static void Note(RFs& aFs, const TDesC8& aText)
	{
	RFile f;
	if (f.Open(aFs, _L("C:\\Linux\\switch.txt"), EFileWrite) != KErrNone && f.Create(aFs, _L("C:\\Linux\\switch.txt"), EFileWrite) != KErrNone)
		return;
	TInt end = 0;
	f.Seek(ESeekEnd, end);
	f.Write(aText);
	f.Close();
	}

static TInt SleepFiles(RFs& aFs)
	{
	CDir* d = 0;
	TInt n = 0;
	if (aFs.GetDir(_L("C:\\System\\Apps\\PsiLinux\\Sleep\\*.mem"), KEntryAttNormal, ESortNone, d) == KErrNone && d)
		for (TInt i = 0; i < d->Count(); i++)
			n += (*d)[i].iSize;
	delete d;
	return n;
	}

static TInt FreeKb()
	{
	TMemoryInfoV1Buf mi;
	UserHal::MemoryInfo(mi);
	return mi().iFreeRamInBytes / 1024;
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* tc = CTrapCleanup::New();       // (TApaTaskList needs one)
	RFs fs;
	RWsSession ws;
	if (fs.Connect() != KErrNone || ws.Connect() != KErrNone)
		return 0;
	const TUid psilinux = { 0x01000A87 };
	TBuf8<80> l;
	// twice: the second sleep writes only what changed since the first
	for (TInt round = 0; round < 2; round++)
		{
		User::After(round ? 10000000 : 60000000);
		{
			TApaTaskList tasks(ws);
			TApaTask t = tasks.FindApp(psilinux);
			if (t.Exists())
				t.SendToBackground();
			l.Format(_L8("behind (%d), %d KB free; "), t.Exists(), FreeKb());
			Note(fs, l);
		}
		User::After(90000000);
		TInt files = SleepFiles(fs);
		l.Format(_L8("90 s on: %d KB free, sleep files %d KB; "), FreeKb(), files / 1024);
		Note(fs, l);
		User::After(10000000);
		{
			TApaTaskList tasks(ws);
			TApaTask t = tasks.FindApp(psilinux);
			if (t.Exists())
				t.BringToForeground();
			Note(fs, _L8("in front. "));
		}
		}
	ws.Close();
	fs.Close();
	delete tc;
	return 0;
	}
