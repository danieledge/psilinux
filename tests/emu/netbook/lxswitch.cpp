// lxswitch.cpp - for the netBook emulator only (0.39): PsiLinux's sleep
// test. Started by the test launcher (lxlaunch.cpp) when the image has it
// (nbimage.mts, NB_SLEEPTEST=1). Twice, it sends PsiLinux behind (at 60 s,
// then at 170 s), notes the free RAM and the sleep files' size (in the
// emulator they are on C:, in RAM) 90 s on, then brings PsiLinux back to
// the front; the test root's t7 shows its notes (C:\Linux\switch.txt).
//
// (0.40) C:\System\Apps\PsiLinux\swmode.txt (NB_SWMODE in nbimage.mts)
// picks another test for the first round:
//   fill  the disk too full for the sleep (a file leaves 1.5 MB free): the
//         sleep fails once, its files are deleted, and it is not tried
//         again every second; the file goes at 150 s, and the second round
//         (PsiLinux in front in between) sleeps
//   card  the card taken out while Linux sleeps: at 150 s the sleep files'
//         folder is renamed (as if the card were out), PsiLinux comes to
//         the front at 160 s and must wait ("Put the card back"), and at
//         190 s the folder is back and Linux wakes; then, asleep a third
//         time, one file's header is another session's (another card):
//         Linux waits again, and wakes when the header is put back
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

static TInt SleepFiles(RFs& aFs, TInt* aCount = 0)
	{
	CDir* d = 0;
	TInt n = 0;
	if (aFs.GetDir(_L("C:\\System\\Apps\\PsiLinux\\Sleep\\*.mem"), KEntryAttNormal, ESortNone, d) == KErrNone && d)
		{
		for (TInt i = 0; i < d->Count(); i++)
			n += (*d)[i].iSize;
		if (aCount)
			*aCount = d->Count();
		}
	else if (aCount)
		*aCount = 0;
	delete d;
	return n;
	}

static TInt FreeKb()
	{
	TMemoryInfoV1Buf mi;
	UserHal::MemoryInfo(mi);
	return mi().iFreeRamInBytes / 1024;
	}

static void Show(RWsSession& aWs, TBool aFront)
	{
	const TUid psilinux = { 0x01000A87 };
	TApaTaskList tasks(aWs);
	TApaTask t = tasks.FindApp(psilinux);
	if (t.Exists())
		{
		if (aFront)
			t.BringToForeground();
		else
			t.SendToBackground();
		}
	}

_LIT(KFill, "C:\\Linux\\fill.bin");

// a sleep file's stamp changed (as another session's): twice puts it back
static TInt Flip(RFs& aFs, const TDesC& aName)
	{
	RFile f;
	TInt r = f.Open(aFs, aName, EFileWrite);
	if (r != KErrNone)
		return r;
	TBuf8<4> stamp;
	r = f.Read(8, stamp, 4);
	if (r == KErrNone && stamp.Length() == 4)
		{
		stamp[0] ^= 0x5A;
		r = f.Write(8, stamp);
		}
	f.Close();
	return r;
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* tc = CTrapCleanup::New();       // (TApaTaskList needs one)
	RFs fs;
	RWsSession ws;
	if (fs.Connect() != KErrNone || ws.Connect() != KErrNone)
		return 0;
	TBuf8<8> mode;
	{
		RFile mf;
		if (mf.Open(fs, _L("C:\\System\\Apps\\PsiLinux\\swmode.txt"), EFileRead) == KErrNone)
			{
			mf.Read(mode);
			mf.Close();
			}
	}
	TBool fill = mode.Find(_L8("fill")) == 0, card = mode.Find(_L8("card")) == 0;
	TBuf8<100> l;
	// twice: the second sleep writes only what changed since the first
	for (TInt round = 0; round < 2; round++)
		{
		User::After(round ? 10000000 : 60000000);
		if (fill && round == 0)
			{
			// the disk (C:, RAM, in the emulator) left with 1.5 MB free
			RFile f;
			TInt size = FreeKb() * 1024 - 1536 * 1024;
			if (size > 0 && f.Replace(fs, KFill, EFileWrite) == KErrNone)
				{
				TInt r = f.SetSize(size);
				f.Close();
				l.Format(_L8("filled %d KB (%d), %d KB free; "), size / 1024, r, FreeKb());
				Note(fs, l);
				}
			}
		Show(ws, EFalse);
		l.Format(_L8("behind, %d KB free; "), FreeKb());
		Note(fs, l);
		User::After(90000000);
		TInt count = 0;
		TInt files = SleepFiles(fs, &count);
		l.Format(_L8("90 s on: %d KB free, sleep files %d KB (%d); "), FreeKb(), files / 1024, count);
		Note(fs, l);
		if (fill && round == 0)
			{
			fs.Delete(KFill);
			l.Format(_L8("fill deleted, %d KB free; "), FreeKb());
			Note(fs, l);
			}
		if (card && round == 0)
			{
			// the card out: the sleep files gone with it
			TInt r = fs.Rename(_L("C:\\System\\Apps\\PsiLinux\\Sleep\\"), _L("C:\\System\\Apps\\PsiLinux\\SleepOut\\"));
			l.Format(_L8("card out (%d); "), r);
			Note(fs, l);
			}
		User::After(10000000);
		Show(ws, ETrue);
		Note(fs, _L8("in front. "));
		if (card && round == 0)
			{
			User::After(30000000);
			TInt freeOut = FreeKb();
			TInt r = fs.Rename(_L("C:\\System\\Apps\\PsiLinux\\SleepOut\\"), _L("C:\\System\\Apps\\PsiLinux\\Sleep\\"));
			l.Format(_L8("30 s in front, card out: %d KB free; card back (%d); "), freeOut, r);
			Note(fs, l);
			User::After(10000000);
			l.Format(_L8("10 s on: %d KB free; "), FreeKb());
			Note(fs, l);
			}
		}
	if (card)
		{
		// another card: Linux asleep a third time, then one of its files
		// replaced by a header from another session
		User::After(10000000);
		Show(ws, EFalse);
		User::After(90000000);
		CDir* d = 0;
		TInt r = KErrNotFound;
		TFileName n(_L("C:\\System\\Apps\\PsiLinux\\Sleep\\"));
		if (fs.GetDir(_L("C:\\System\\Apps\\PsiLinux\\Sleep\\*.mem"), KEntryAttNormal, ESortNone, d) == KErrNone && d && d->Count())
			{
			n.Append((*d)[0].iName);
			r = Flip(fs, n);
			}
		delete d;
		l.Format(_L8("asleep again, another card's file (%d); "), r);
		Note(fs, l);
		User::After(5000000);
		Show(ws, ETrue);
		User::After(20000000);
		l.Format(_L8("20 s in front: %d KB free; "), FreeKb());
		Note(fs, l);
		// Linux's own card back: it wakes
		r = r == KErrNone ? Flip(fs, n) : r;
		User::After(15000000);
		l.Format(_L8("its card back (%d), 15 s on: %d KB free. "), r, FreeKb());
		Note(fs, l);
		}
	ws.Close();
	fs.Close();
	delete tc;
	return 0;
	}
