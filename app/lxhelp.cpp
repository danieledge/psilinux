// lxhelp.cpp - Tools > Help on PsiLinux (Shift+Ctrl+H): the topics, in a
// dialog with a topic list and a read-only text viewer, as PsiTerm's and
// PsiMail's help (app/pthelp.cpp). British English, the user's words, key
// names as the keyboard prints them, "program" not "application".
#include <eikenv.h>
#include <eikchlst.h>
#include <eikrted.h>
#include <txtrich.h>
#include <badesca.h>
#include "lxhelp.h"
#include "psilinux.hrh"

struct TLxHelpTopic { const char* iTitle; const char* iText; };

static const TLxHelpTopic KHelpTopics[] =
	{
	{ "Getting started",
	  "PsiLinux runs real Linux programs on the Psion, natively: a shell, the everyday commands (ls, "
	  "cat, grep, sed, awk, find, tar, vi...), Lua, MicroPython, SQLite, jq, nano, curl and git.\n"
	  "\n"
	  "Linux starts when PsiLinux opens, with a shell (BusyBox's hush). Type a command and press Enter. "
	  "\"help\" lists the shell's own commands and \"busybox\" the rest of BusyBox's.\n"
	  "\n"
	  "\"exit\" (or Ctrl+D on an empty line) ends the shell and Linux with it; Enter starts it again. "
	  "File > End Linux (Shift+Ctrl+D) ends every Linux program at once, and File > Close "
	  "(Shift+Ctrl+E) closes PsiLinux.\n"
	  "\n"
	  "The status line at the bottom shows the program in front, how many Linux programs are running, "
	  "the memory they use and the memory the Psion has free." },

	{ "Alpine Linux",
	  "PsiLinux can also run Alpine Linux 3.24: Alpine's own programs, unchanged, and its package "
	  "manager with Alpine's whole catalogue. The Psion's processor cannot run them directly, so "
	  "PsiLinux translates each piece of a program the first time it runs: the first run of a program "
	  "is slow, later runs much quicker.\n"
	  "\n"
	  "Tools > Preferences > Start chooses what Start Linux does:\n"
	  "Alpine Linux - boots Alpine (BusyBox init, then OpenRC), and logs in as root. It takes about two "
	  "minutes.\n"
	  "Alpine Linux, shell only - straight to Alpine's shell: quicker, and more memory free.\n"
	  "PsiLinux - PsiLinux's own programs, at full speed.\n"
	  "\n"
	  "The shell is bash, with a short prompt (psion:~#), Tab to complete names and Up for earlier "
	  "commands. ll lists in detail. If bash is removed, BusyBox's sh takes its place. Your own "
	  "settings go in ~/.bashrc; an empty file ~/.hushlogin stops the fortune at login.\n"
	  "\n"
	  "Ready to use: nano (the editor), less, man, htop, ncdu, tree, jq, bc, ssh and scp. For fun: "
	  "fortune, figlet, sl, cmatrix, tetris, worm, robots, snake, adventure, cbonsai, nyancat and "
	  "fastfetch (tetris and robots need Preferences > Screen > Text size > Smallest). apk adds "
	  "more. A program's first run is slow while it is translated.\n"
	  "\n"
	  "ssh: a login takes about half a minute, most of it before the first question (or the "
	  "password). ed25519 keys are the quickest (ssh-keygen -t ed25519); an RSA key adds about 15 s. "
	  "PsiLinux's settings for it are in /etc/ssh/ssh_config.d/psilinux.conf; yours go in "
	  "~/.ssh/config.\n"
	  "\n"
	  "Alpine needs PsiLinux-alpine-root.sis and its parts -2 to -5, all installed on the Linux disk, "
	  "the first one first: they go in "
	  "\\Linux\\Alpine\\, and your changes in \\Linux\\Alpine\\root\\. Without it, PsiLinux "
	  "says so and starts its own programs.\n"
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
	  "Lua, SQLite and nethack fit; vim, links, curl, git's https and Python need more memory than a "
	  "5mx has.\n"
	  "\n"
	  "The package server is the proxy in Connection settings (PsiTerm, PsiMail or PsiWeb), when it "
	  "is on and is a PsiProxy. With the Atom modem's own proxy (192.168.7.1) it is the PsiProxy set "
	  "in the modem's Web proxy settings; without one apk says Needs PsiProxy on a computer. To use "
	  "another, set it in /etc/psilinux/apk.conf, for example "
	  "APK_SERVER=http://192.168.1.10:8081 (nano /etc/psilinux/apk.conf)." },

	{ "The terminal",
	  "Everything you type goes to Linux, including every Ctrl+letter (Ctrl+C, Ctrl+D, Ctrl+Z, Ctrl+R): "
	  "PsiLinux's own shortcuts are Shift+Ctrl+letter. Fn+Up and Fn+Down are Pg Up and Pg Dn, Fn+Left and "
	  "Fn+Right are Home and End.\n"
	  "\n"
	  "The Psion's keyboard has no | key: Shift+Ctrl+I types it. Keys > Symbols has |, ` and ~, and "
	  "the toolbar's Keys button pops up Esc, Tab and the Ctrl keys for the pen.\n"
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
	  "The sidebar's zoom icons, or View > Zoom in and Zoom out (Shift+Ctrl+M), go round five text "
	  "sizes. Tools > Preferences (Shift+Ctrl+K) sets the colours, the cursor and the status line. "
	  "Colours are shown as greys, by brightness." },

	{ "Files",
	  "Linux sees the Psion's disks as /c and /d (and the ROM as /z): ls /d lists the memory disk, and "
	  "Linux programs can read and change Psion files there. Psion files keep their own names.\n"
	  "\n"
	  "The rest of Linux's files - /bin, /etc, /usr, your home folder /root and /tmp - are in the Linux "
	  "folder, \\Linux\\ on the disk chosen with Tools > Linux disk (the memory disk if there is one). "
	  "ROOT.IMG there holds the programs; your own files and changes go in \\Linux\\root\\.\n"
	  "\n"
	  "Symbolic links and file modes work in Linux's own folders. The Psion's disks have neither: there "
	  "every file can be run, and chmod changes nothing.\n"
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
	  "Programs must fit the Psion's memory (about 9 MB free on a 5mx) and its 36 MHz ARMv4T processor, "
	  "which has no floating point hardware: arithmetic with fractions is slow.\n"
	  "\n"
	  "Linux can take nearly all the free memory. To keep some for the Psion's own programs, tick "
	  "Tools > Preferences > Keep memory for other programs, and choose how much: Linux programs then "
	  "get \"out of memory\" first, and the Psion's programs can still open.\n"
	  "\n"
	  "There is no graphical display (no X or Wayland): only programs that run in a terminal.\n"
	  "\n"
	  "Linux's own drivers, kernel modules, mount and the like have no place here: the Psion's own "
	  "system does that.\n"
	  "\n"
	  "PsiLinux keeps no log unless Tools > Keep a log is ticked. Then the runner writes what it does "
	  "(the programs it starts, memory, faults) to PsiLinux.log in the Linux folder, for whoever is "
	  "looking into a problem; untick it to stop (the log kept so far stays). It takes effect the "
	  "next time Linux starts." },
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
	ShowTopicL(iTopic);
	}

void CLxHelpDialog::ShowTopicL(TInt aTopic)
	{
	const TLxHelpTopic& topic = KHelpTopics[aTopic];
	TPtrC8 title((const TUint8*)topic.iTitle);
	TPtrC8 text((const TUint8*)topic.iText);
	HBufC* b = HBufC::NewLC(title.Length() + text.Length() + 4);
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
