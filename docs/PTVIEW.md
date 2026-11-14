# PsiTerm's terminal in PsiLinux (0.29), and the seam PsiTerm could make

PsiLinux's terminal is PsiTerm's: the same drawing, the same fonts
(Terminus 12 to 24, Spleen, Gohufont, Cozette and Courier small, with
PsiTerm's zoom levels), the same themes and colour schemes (Classic to
Soft on a grey screen; Light, Dark, High contrast, Soft and Muxedo's
eleven on a colour one), the same history, pen selection, copying and
pasting, and PsiTerm's drawing benchmark. PsiLinux keeps its own program:
its window, toolbar, menus, Preferences, settings file and its session with
the runner.

## How it is done now

PsiTerm keeps its terminal in one class, `CTermView` (`app/psiterm.h`,
`app/psiterm.cpp`), together with its SSH session, the serial line, tmux,
the start screen and recent commands. So at every build:

1. `tools/ptview.py` takes PsiTerm's terminal functions out of
   `psiterm.cpp` (and `RowY` out of `pttabs.cpp`) word for word, by name,
   with the file-scope constants and helpers they use (the typeface table,
   the xterm palette, Muxedo's themes...), into `app/lxptview.cpp`; and the
   theme and cursor names out of `psiterm.rss` into `app/lxptview.rssi`.
   Both are generated and not kept in the repository. If PsiTerm renames or
   removes one of them, the build stops there and names it.
2. `app/lxterm.h` includes PsiTerm's own `psiterm.h`, so the class and its
   data are PsiTerm's, compiled against PsiTerm's checkout of psion-core
   (`psiterm/core`: its ssh headers and its fonts, `pkg/psiterm.gdr`).
3. `app/lxterm.cpp` defines the members of `CTermView` that are PsiLinux's:

   | PsiTerm's member | In PsiLinux |
   |---|---|
   | `ConstructL`, the constructor and destructor | the terminal without the serial line, SSH, recent commands, the start screen |
   | `WriteToHost` | the bytes go to the runner (`CLxSession::Write`) |
   | `StatusText` | Linux's status: the program in front, how many, their memory, free memory, the time (and the echo time with Keep a log) |
   | `OfferKeyEventL` | PsiTerm's key mapping without SSH, tmux, snippets and recent commands; Enter starts Linux when it is not running |
   | `Tick`, `StartTick`, `SetForeground`, `TickBusy` | the cursor's blink and the status line only |
   | `LocalMessage`, `ApplyAppearanceL`, `SshLoggedIn` | into the terminal; lays out again; "Linux runs" |
   | `SerialDataL`, `SerialError`, `TabStripHeight`, `DrawTabs`, `TabPenDownL`, `TmuxQueryKick`, `ParseTabList`, `RingSsh`, `RunToolDialogL`, `CSerialPort::Close`, `CFontDialog::FaceHeights` | nothing (PsiLinux has no serial line, no tmux tab strip, no Other font...) |

   `CLxTermView` derives from `CTermView` and adds PsiLinux's session.
   PsiLinux's settings (`TLxSettings`, unchanged in size; the typeface in a
   spare field) are copied into PsiTerm's `TPsiSettings` when they change.
4. `CTermView`'s data are private, and PsiLinux's view reads a few (the
   selection, the history's length), so `lxterm.h` includes `psiterm.h`
   with `private` defined as `public`. That changes access only, never the
   layout of the class.

It works, but it is held together by names: PsiTerm can break PsiLinux's
build by renaming a helper, moving a function into another file, or
calling something new from a drawing function.

## The seam: what PsiTerm would need (not made; for Dan)

The smallest change that makes PsiTerm's terminal a component both programs
compile as it is:

1. **Split the class.** Move the terminal into `app/ptview.h` and
   `app/ptview.cpp` as a base class, say `CPtTermView : public CCoeControl`,
   with the data and functions PsiLinux takes today (the list is METHODS
   and STATICS in `tools/ptview.py`: fonts and zoom, libvterm and its
   callbacks, drawing, colours and themes, the history, the pen, the
   clipboard, the benchmark). `CTermView : public CPtTermView` keeps SSH,
   the serial line, tmux, the start screen, recent commands and the rest.
   `psiterm.mmp` gains `ptview.cpp`; nothing else in PsiTerm changes
   behaviour.
2. **Make the places where the terminal reaches the session virtual** (with
   PsiTerm's present code as `CTermView`'s overrides):
   - `virtual void WriteToHost(const TDesC8&)` (keys and libvterm's answers);
   - `virtual void StatusText(TDes&, TInt&) const`;
   - `virtual TBool HostLive() const` in place of the `iSshActive` test in
     `CbBell` and `SshLoggedIn()` in `HandlePointerEventL` (mouse reporting);
   - `virtual void SizeChangedTo(TInt aRows, TInt aCols)` in place of the
     write to `iShared` (rows, cols, resized, `RingSsh`) at the end of
     `Layout`;
   - `virtual void TitleChanged()` in place of `TmuxQueryKick` and the tab
     parsing in `CbSetTermProp`;
   - the tab strip as virtuals with empty defaults: `TabStripHeight`,
     `DrawTabs`, `TabPenDownL`, `RowHidden` (`iTabsTop` and its friends move
     to `CTermView`, or stay in the base as "a strip of this height at the
     top");
   - `ZoomBy`'s Other font branch calls `CFontDialog::FaceHeights`: move
     that function (it only asks the font server) into `ptview.cpp` too.
3. **Settings.** The terminal reads `iZoom`, `iFace`, `iFontName`,
   `iFontPixels`, `iTheme`, `iCursor`, `iBlink`, `iStatus`, `iBold`,
   `iBell` and `iDisplay` of `TPsiSettings`. Put those in a struct of their
   own, `TPtLook`, that `TPsiSettings` keeps as it is in its file (a copy
   at load and save, so `PsiTerm.ini` does not change) and the base class
   takes.
4. **Accessors** for what another program's window needs: `HasSelection()`,
   `HistoryLines()`, `Settings()` (the `TPtLook`).
5. **Names.** Theme and cursor names in `app/ptview.rssi`, included by
   `psiterm.rss`, so another program's resource file can include it too.
6. **The inline functions in `psiterm.h`** that call into SSH and the
   serial line (`SerialClose`, `ShowDebugL`, `LinkSwitchedOn`) stay on
   `CTermView`, out of the base class, so a program compiling only the
   terminal does not need them.

Then PsiLinux's `app/lxptview.cpp` becomes `#include "ptview.cpp"` (as
`lxtermsb.cpp` does for `termsb.cpp`), `CLxTermView` derives from
`CPtTermView` and overrides the virtuals above, and `tools/ptview.py`, the
stubs in `lxterm.cpp` and the `private`/`public` trick go.
