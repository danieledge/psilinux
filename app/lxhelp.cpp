// lxhelp.cpp - Tools > Help on PsiLinux (Shift+Ctrl+H): the topics, in a
// dialog with a topic list and a read-only text viewer, as PsiTerm's and
// PsiMail's help (app/pthelp.cpp). British English, the user's words, key
// names as the keyboard prints them, "program" not "application".
// (0.43) "the Psion" in the text is the machine it runs on: ShowTopicL puts
// "the netBook" or "the Series 5mx" in its place, and @FREE@ the memory that
// machine has free, so each reads about its own.
#include <eikenv.h>
#include <eikchlst.h>
#include <eikrted.h>
#include <txtrich.h>
#include <eiksbfrm.h>
#include <badesca.h>
#include "lxhelp.h"
#include "psilinux.hrh"

struct TLxHelpTopic { const char* iTitle; const char* iText; };

static const TLxHelpTopic KHelpTopics[] =
	{
	{ "Getting started",
	  "PsiLinux runs real Linux programs on the Psion: Alpine Linux's shell and programs, and its "
	  "package manager, apk.\n"
	  "\n"
	  "Linux starts when PsiLinux opens, at Alpine's shell (bash). Type a command and press Enter: "
	  "ls, cat, grep, nano, less, htop and the other everyday commands are there, and apk adds more "
	  "(see Alpine packages). Tools > Preferences > Start Linux as chooses another start (see "
	  "Alpine Linux).\n"
	  "\n"
	  "\"exit\" (or Ctrl+D on an empty line) at the first shell ends Linux and closes PsiLinux, after "
	  "ending the programs left and closing their files. With jobs still running it warns first: "
	  "exit again to close. File > End Linux (Shift+Ctrl+D) ends every Linux program at once, and File > Close "
	  "(Shift+Ctrl+E) closes PsiLinux.\n"
	  "\n"
	  "The status line at the bottom shows the program in front, how many Linux programs are running, "
	  "the memory they use and the memory the Psion has free.\n"
	  "\n"
	  "Linux can sleep behind another program: Preferences > General > In the background, Sleep "
	  "after 1, 5 or 15 minutes (Never sleep, as it comes). With Linux idle at the shell's prompt, its "
	  "memory then goes to the card (\\System\\Apps\\PsiLinux\\Sleep\\) and the Psion has the RAM back. "
	  "It wakes when PsiLinux comes to the front. Linux stays awake while a program runs, an editor or "
	  "a pager is open, or a network connection is open. With too little room on the disk it stays awake "
	  "and tries again later. Asleep, the disk must stay in: with the card out, PsiLinux says "
	  "\"Put the card back to wake Linux\" and waits." },

	{ "Alpine Linux",
	  "PsiLinux runs Alpine Linux 3.24: Alpine's own programs, unchanged, and its package "
	  "manager with Alpine's whole catalogue. The Psion's processor cannot run them directly, so "
	  "PsiLinux translates each piece of a program the first time it runs: the first run of a program "
	  "is slow, later runs much quicker.\n"
	  "\n"
	  "Tools > Preferences > Start Linux as chooses what Start Linux does:\n"
	  "Alpine Linux, shell only (as it comes) - straight to Alpine's shell: quick, and more memory "
	  "free.\n"
	  "Alpine Linux - boots Alpine (BusyBox init, then OpenRC), and logs in as root. It takes about two "
	  "minutes.\n"
	  "PsiLinux's own programs - at full speed, with BusyBox's shell (hush): \"help\" lists the "
	  "shell's own commands and \"busybox\" the rest of BusyBox's.\n"
	  "\n"
	  "The shell is bash, with a short prompt (psion:~#), Tab to complete names and Up for earlier "
	  "commands. ll lists in detail. If bash is removed, BusyBox's sh takes its place. Your own "
	  "settings go in ~/.bashrc.\n"
	  "\n"
	  "At login a welcome banner shows Alpine's logo and the system's figures (memory and disk "
	  "as they are now). Tools > Show welcome banner turns it off (a short fortune instead); an "
	  "empty file ~/.hushlogin (touch ~/.hushlogin) stops both.\n"
	  "\n"
	  "Ready to use: nano (the editor), less, man, htop, ncdu, tree, jq, bc, ssh and scp. For fun: "
	  "fortune, figlet, sl, cmatrix, tetris, worm, robots, snake, adventure, cbonsai, nyancat and "
	  "fastfetch (tetris and robots need Tools > Preferences > Screen > Text size > Smallest). apk adds "
	  "more. A program's first run is slow while it is translated.\n"
	  "\n"
	  "ssh: a login takes about half a minute, most of it before the first question (or the "
	  "password). ed25519 keys are the quickest (ssh-keygen -t ed25519); an RSA key adds about 15 s. "
	  "PsiLinux's settings for it are in /etc/ssh/ssh_config.d/psilinux.conf; yours go in "
	  "~/.ssh/config.\n"
	  "\n"
	  "Alpine is in \\Linux\\Alpine\\ on the Linux disk, and your changes in \\Linux\\Alpine\\root\\. "
	  "On Coda the Linux folder comes on the Coda card: if a part is missing, copy the Linux folder "
	  "from the card again. Elsewhere it comes as PsiLinux-alpine-root.sis and its parts -2 to -5, "
	  "installed on the Linux disk, the first one first. Without it, PsiLinux says so and starts "
	  "its own programs.\n"
	  "\n"
	  "\"poweroff\" or File > End Linux ends Alpine. About shows Alpine's version." },

	{ "Alpine packages",
	  "apk installs Alpine's packages, as on Alpine: apk search NAME finds one, apk info NAME describes "
	  "it, apk add NAME installs it, apk del NAME removes it, apk upgrade brings everything up to date "
	  "and apk list -I lists what is installed.\n"
	  "\n"
	  "Alpine's catalogue is too big for the Psion's memory, so searching is done by the package "
	  "server (PsiProxy). The packages are Alpine's own, signed by Alpine: the Psion downloads them and "
	  "Alpine's apk checks and installs them.\n"
	  "\n"
	  "Before downloading, apk add says how much it will download and how much disk it needs, and "
	  "warns when the program is unlikely to fit in memory. apk add -s only says what it would do. "
	  "Lua, SQLite and nethack fit; vim, links, curl, git's https and Python may need more memory "
	  "than the Psion has.\n"
	  "\n"
	  "The package server is the proxy in Connection settings (PsiTerm, PsiMail or PsiWeb), when it "
	  "is on and is a PsiProxy. With the Atom modem's own proxy (192.168.7.1) it is the PsiProxy set "
	  "in the modem's Web proxy settings; without one apk says Needs PsiProxy on a computer. To use "
	  "another, set it in /etc/psilinux/apk.conf, for example "
	  "APK_SERVER=http://192.168.1.10:8081 (nano /etc/psilinux/apk.conf). apk source shows where "
	  "packages come from, and changes it." },

	{ "The terminal",
	  "Everything you type goes to Linux, including every Ctrl+letter (Ctrl+C, Ctrl+D, Ctrl+Z, Ctrl+R) "
	  "but Ctrl+M (Zoom in: Enter sends what it would) and Ctrl+Q (Switch view): "
	  "PsiLinux's other shortcuts are Shift+Ctrl+letter. Fn+Up and Fn+Down are Pg Up and Pg Dn, Fn+Left and "
	  "Fn+Right are Home and End.\n"
	  "\n"
	  "Shift+Ctrl+I types |, which the keyboard may not have. Keys > Symbols has |, ` and ~, and "
	  "the toolbar's Keys button pops up Esc, Tab, the Ctrl keys and the symbols for the pen "
	  "(its Stop button is Ctrl+C).\n"
	  "\n"
	  "Shift+Fn+Up and Shift+Fn+Down look back through earlier text; typing returns to the live screen. "
	  "Drag the pen on the right-hand edge to scroll.\n"
	  "\n"
	  "Drag the pen over text to select it; Edit > Copy (Shift+Ctrl+C) puts it on the clipboard, and "
	  "Edit > Paste (Shift+Ctrl+V) types the clipboard. Text copied in other Psion programs pastes "
	  "here too.\n"
	  "\n"
	  "When a program asks for the mouse (vim, less, htop), a tap is a click and a drag scrolls; "
	  "Shift with the pen still selects text.\n"
	  "\n"
	  "The terminal is PsiTerm's, with its typefaces and themes. The Zoom in and Zoom out Command "
	  "icons, or View > Zoom in and Zoom out (Ctrl+M and Shift+Ctrl+M), go round the typeface's sizes and the "
	  "smallest size of all. "
	  "Tools > Preferences (Shift+Ctrl+K), Screen, sets the typeface (Terminus, Spleen, Gohufont, "
	  "Cozette), the text size, the colours, the cursor and the status line. "
	  "On the netBook and Series 7, Linux's colours are shown in colour, with PsiTerm's colour "
	  "schemes (Clean, Dracula, Nord, Solarized Dark\x85); on the Series 5mx, as greys, by "
	  "brightness." },

	{ "Files",
	  "Linux sees the Psion's disks as /psion/c, /psion/d and /psion/z (the ROM), and as /c and /d: "
	  "ls /psion/d lists the card, and Linux programs can read and change Psion files there. "
	  "Psion files keep their own names, and as on EPOC Letter and LETTER are one file. The ROM, and "
	  "every disk's System folder, are read-only to Linux (epoc syswrite on --force allows it).\n"
	  "\n"
	  "The epoc command works your Psion from the shell: epoc open FILE opens a file in its program, "
	  "epoc run Word starts a program, epoc tasks and epoc ps list them, epoc clip get and set use the "
	  "clipboard, epoc note shows a message. epoc help lists them all.\n"
	  "\n"
	  "The rest of Linux's files - /bin, /etc, /usr, your home folder /root and /tmp - are in the Linux "
	  "folder, \\Linux\\ on the disk chosen with Tools > Linux disk (the card if there is one). "
	  "ROOT.IMG there holds the programs; your own files and changes go in \\Linux\\root\\.\n"
	  "\n"
	  "Symbolic links and file modes work in Linux's own folders. The Psion's disks have neither: there "
	  "every file can be run, and chmod changes only the read-only attribute (chmod a-w, u+w).\n"
	  "\n"
	  "/proc and /dev work as on Linux: ps, top, free and df read them." },

	{ "Running programs",
	  "A command followed by & runs in the background; jobs lists them, fg brings one back to the front "
	  "and bg lets a stopped one carry on.\n"
	  "\n"
	  "Ctrl+C stops the program in front; Ctrl+Z pauses it (fg or bg then carries it on); Ctrl+\\ quits "
	  "it. A program busy computing hears Ctrl+C only when it next reads or writes; press Ctrl+C a second "
	  "time within 5 seconds and it is stopped at once.\n"
	  "\n"
	  "Scripts work as on Linux: a file starting #!/bin/sh, made runnable with chmod +x. Lua and "
	  "MicroPython scripts work the same way.\n"
	  "\n"
	  "Programs built for PsiLinux (ARMv4T, static, with the PsiLinux toolchain's lx-cc) run at full "
	  "speed. Programs built for another Linux run only in Alpine mode, which translates Alpine's "
	  "own programs as they run (see Alpine Linux)." },

	{ "The Internet",
	  "Linux programs use the Psion's own Internet connection (Control panel > Internet), the same as "
	  "the Psion's Web and Email programs: wget, curl and nc connect, and the Psion dials when the first "
	  "one does. The connection stays up while any Linux program has one open.\n"
	  "\n"
	  "Names are looked up by the Psion (the address in /etc/resolv.conf, 127.0.0.53, is PsiLinux "
	  "itself). curl and wget fetch https:// pages; the first one takes a few seconds, while the Psion "
	  "works out the keys." },

	{ "What will not work",
	  "The Psion has no memory management unit, so a program cannot copy itself and run on as two: "
	  "when it forks, the copy runs first, and the original waits until the copy has started another "
	  "program or ended. That is how most programs use fork (git does), so they work; a server that "
	  "forks to serve each visitor does not, nor do tmux and rsync's own copying.\n"
	  "\n"
	  "Programs must fit the Psion's memory (@FREE@) and its processor, which has no floating "
	  "point hardware: arithmetic with fractions is slow.\n"
	  "\n"
	  "Linux can take nearly all the free memory. To keep some for the Psion's own programs, tick "
	  "Tools > Preferences > Keep memory for other programs, and choose how much: Linux programs then "
	  "get \"out of memory\" first, and the Psion's programs can still open.\n"
	  "\n"
	  "Graphical programs need the Linux screen: X is a separate installer (PsiLinux-X), and there is "
	  "no Wayland. Without it, only programs that run in a terminal.\n"
	  "\n"
	  "Linux's own drivers, kernel modules, mount and the like have no place here: the Psion's own "
	  "system does that.\n"
	  "\n"
	  "PsiLinux keeps no log unless Tools > Troubleshooting > Keep a log is ticked. Then the runner "
	  "writes what it does (the programs it starts, memory, faults) to PsiLinux.log in the Linux "
	  "folder, to send with a report of a problem; untick it to stop (the log kept so far stays). It "
	  "takes effect the next time Linux starts." },

	{ "Credits",
	  "PsiLinux is by Dan Edge. It runs Alpine Linux's own programs, and its own are built from "
	  "BusyBox, musl, Lua, MicroPython, SQLite, jq, nano, curl, git and Mbed TLS.\n"
	  "\n"
	  "The terminal is PsiTerm's, built on libvterm, with the Terminus, Spleen, Gohufont and "
	  "Cozette typefaces. The translator's floating point is Berkeley SoftFloat, by John R. Hauser "
	  "(BSD licence).\n"
	  "\n"
	  "Each program's licence is in Linux, in /usr/share/licenses; the typefaces' and SoftFloat's "
	  "are beside PsiLinux, in its folder. The source is at github.com/danieledge/psilinux." },
	};
const TInt KHelpCount = sizeof(KHelpTopics) / sizeof(KHelpTopics[0]);

void CLxHelpDialog::SetSizeAndPositionL(const TSize& aSize)
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TSize size(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	SetCornerAndSizeL(EHCenterVCenter, size);
	}

void CLxHelpDialog::PreLayoutDynInitL()
	{
	CDesCArrayFlat* titles = new(ELeave) CDesCArrayFlat(KHelpCount);
	CleanupStack::PushL(titles);
	TBuf<40> t;
	for (TInt i = 0; i < KHelpCount; i++)
		{
		t.Copy(TPtrC8((const TUint8*)KHelpTopics[i].iTitle));
		titles->AppendL(t);
		}
	CleanupStack::Pop();
	CEikChoiceList* cl = (CEikChoiceList*)Control(ELxHelpTopic);
	cl->SetArrayL(titles);
	if (iTopic < 0 || iTopic >= KHelpCount)
		iTopic = 0;
	cl->SetCurrentItem(iTopic);
	// (0.43) a scroll bar, so it shows there is more than the box holds
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(ELxHelpText);
	ed->CreateScrollBarFrameL();
	ed->ScrollBarFrame()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EOn);
	ShowTopicL(iTopic);
	}

// every aOld in aText becomes aNew (there is room: ShowTopicL's buffer)
static void LxReplace(TDes& aText, const TDesC& aOld, const TDesC& aNew)
	{
	TInt from = 0;
	for (;;)
		{
		TInt i = aText.Mid(from).Find(aOld);
		if (i < 0)
			return;
		i += from;
		if (aText.Length() - aOld.Length() + aNew.Length() > aText.MaxLength())
			return;
		aText.Replace(i, aOld.Length(), aNew);
		from = i + aNew.Length();
		}
	}

void CLxHelpDialog::ShowTopicL(TInt aTopic)
	{
	const TLxHelpTopic& topic = KHelpTopics[aTopic];
	TPtrC8 title((const TUint8*)topic.iTitle);
	TPtrC8 text((const TUint8*)topic.iText);
	HBufC* b = HBufC::NewLC(title.Length() + text.Length() * 2 + 64);   // (room for the names put in)
	TPtr p = b->Des();
	p.Copy(title);
	p.Append(CEditableText::EParagraphDelimiter);
	for (TInt i = 0; i < text.Length(); i++)
		{
		TUint8 c = text[i];
		if (c == '\n')
			p.Append(CEditableText::EParagraphDelimiter);
		else
			p.Append((TText)c);
		}
	// (0.43) the machine's own name and figures
	TBool tall = iEikonEnv->ScreenDevice()->SizeInPixels().iHeight >= 400;
	LxReplace(p, _L("the Psion"), tall ? _L("the netBook") : _L("the Series 5mx"));
	LxReplace(p, _L("The Psion"), tall ? _L("The netBook") : _L("The Series 5mx"));
	LxReplace(p, _L("@FREE@"), tall ? _L("about 18 MB free on the netBook") : _L("about 9 MB free on the Series 5mx"));
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(ELxHelpText);
	ed->SetTextL(b);
	TCharFormat cf;
	TCharFormatMask cm;
	cf.iFontSpec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	cm.SetAttrib(EAttFontStrokeWeight);
	ed->RichText()->ApplyCharFormatL(cf, cm, 0, title.Length());
	ed->HandleTextChangedL();
	ed->SetCursorPosL(0, EFalse);
	CleanupStack::PopAndDestroy();
	}

void CLxHelpDialog::PostLayoutDynInitL()
	{
	TryChangeFocusToL(ELxHelpTopic);
	}

void CLxHelpDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId != ELxHelpTopic)
		return;
	TInt t = ((CEikChoiceList*)Control(ELxHelpTopic))->CurrentItem();
	if (t != iTopic && t >= 0 && t < KHelpCount)
		{
		iTopic = t;
		ShowTopicL(t);
		}
	}

TBool CLxHelpDialog::OkToExitL(TInt)
	{
	return ETrue;
	}
