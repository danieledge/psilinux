// nbimage.mts - a scratch netBook image for the emulator (never a real one)
// with PsiLinux in it, for testing PsiLinux on Coda. The emulator does not
// mount the card, where PsiLinux and its Linux root are installed, so they
// go in the ROM under Z:\Test\ with a small Linux root (BusyBox only: the
// whole root does not fit in the ROM), two test documents, and the test
// launcher (lxlaunch.cpp) in Jotter's place, which copies them to C: at boot
// and starts PsiLinux.
//
//   node --experimental-strip-types tests/emu/netbook/nbimage.mts BASE.IMG OUT.IMG
//
// Needs Coda's image tools (romlib.mts: $PSION_NETBOOK, standard
// ../psion-netbook), build/psilinux-pkg (build.sh), build/nbtest/lxlaunch.*
// (tests/emu/netbook/build.sh) and build/nbtest/root/ROOT.IMG (mkroot.sh);
// build/nbtest/alpine/ROOT.IMG (mkalpine.sh) too, if it is there.
import { readFileSync, writeFileSync, existsSync, readdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const top = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const nb = process.env.PSION_NETBOOK ?? resolve(top, '../psion-netbook');
const { load, find, addFile, removeChecked, write, setRomSize } = await import(resolve(nb, 'tools/romlib.mts'));
const [base, out] = process.argv.slice(2);
if (!base || !out) { console.error('usage: nbimage.mts BASE.IMG OUT.IMG'); process.exit(2); }
const rom = load(base);
setRomSize(rom, 0x1000000);     // (16 MB, the most: 2 MB less RAM in this scratch image)
const b = resolve(top, 'build');
const P = `${b}/psilinux-pkg/`;
// room: the spare wallpapers, Help and the samples out of this scratch image
for (const n of removeChecked(rom, (['Z:\\System\\Wallpaper\\Coda psi', 'Z:\\System\\Wallpaper\\Coda hills',
  'Z:\\System\\Wallpaper\\Coda sweep', 'Z:\\System\\Wallpaper\\Coda shapes', 'Z:\\System\\Data\\Help',
  'Z:\\System\\Samples\\PsiAmp\\Coda.mp3', 'Z:\\System\\Data\\Iens9522.dat', 'Z:\\System\\Data\\Rektc400.dat',
  'Z:\\System\\Fonts\\TfNotoSerif.gdr', 'Z:\\System\\Libs\\GSMBSC.TSY'] as string[]).filter(p => find(rom, p).length))) console.log(n);
addFile(rom, 'Z:\\System\\Apps\\Jotter\\Jotter.app', readFileSync(`${b}/nbtest/lxlaunch.app`), 'xip');
addFile(rom, 'Z:\\System\\Apps\\Jotter\\Jotter.RSC', readFileSync(`${b}/nbtest/lxlaunch.rsc`));
for (const [n, to] of [['psilinux.app', 'PsiLinux.app'], ['psilinux.rsc', 'PsiLinux.rsc'], ['psilinux.aif', 'PsiLinux.aif'],
  ['psilinux.mbm', 'PsiLinux.mbm'], ['psilxrun.exe', 'psilxrun.exe'], ['psiterm.gdr', 'PsiTerm.gdr'], ['epoc', 'bin\\epoc']])
  addFile(rom, `Z:\\Test\\PsiLinux\\${to}`, readFileSync(P + n));
addFile(rom, 'Z:\\Test\\Libs\\LxSync1.ldd', readFileSync(P + 'lxsync1.ldd'));
if (!find(rom, 'Z:\\System\\Libs\\EStLib.dll').length)
  addFile(rom, 'Z:\\Test\\Libs\\ESTLIB.DLL', readFileSync(resolve(top, 'core/tools/emu/ESTLIB.DLL')));
// (0.37) a small Alpine root (mkalpine.sh), if there is one, in place of
// PsiLinux's own: PsiLinux then starts Alpine's shell, as it does on a real
// netBook. For the room, programs the test does not use go too.
const AR = `${b}/nbtest/alpine/`;
const alpine = existsSync(AR + 'ROOT.IMG');
if (alpine)
  for (const p of ['Agenda', 'Sheet', 'PsiSketch', 'Paint', 'PsiAmp', 'TextEd', 'OPL', 'PsiView', 'Data', 'Calc', 'Word']
    .map(a => `Z:\\System\\Apps\\${a}`).concat(['Z:\\System\\OPX', 'Z:\\System\\Libs\\OplR.dll', 'Z:\\System\\Convert\\PsiConv.cnv',
    'Z:\\System\\Libs\\PsiSpell.bin', 'Z:\\System\\Fonts\\TfInter.gdr', 'Z:\\System\\Fonts\\TfDejaVuSansMono.gdr',
    'Z:\\System\\Libs\\ELexs.exe', 'Z:\\System\\Printers']))
    if (find(rom, p).length) try { removeChecked(rom, [p]); } catch (e) { console.log(`kept ${p}: ${(e as Error).message.split('\n')[1]}`); }
// (0.38) NB_SENDTRIM=1: the launcher stays and sends PsiLinux "sleep" at 20 s
if (process.env.NB_SENDTRIM === '1') addFile(rom, 'Z:\\Test\\SENDTRIM', Buffer.from('sleep\n'));
// (0.39) NB_SLEEPTEST=1: the launcher starts lxswitch.exe (PsiLinux behind at
// 60 s, in front at 160 s), and the Alpine root may sleep to C: (SLEEPONC:
// the emulator has no memory disk)
if (process.env.NB_SLEEPTEST === '1') {
  addFile(rom, 'Z:\\Test\\PsiLinux\\lxswitch.exe', readFileSync(`${b}/nbtest/lxswitch.exe`));
  addFile(rom, 'Z:\\Test\\Linux\\Alpine\\SLEEPONC', Buffer.from('test\n'));
}
const R = alpine ? AR : `${b}/nbtest/root/`;
for (const n of readdirSync(R).filter(n => /^ROOT\.(IMG|I\d\d)$/.test(n)))
  addFile(rom, `Z:\\Test\\Linux\\${alpine ? 'Alpine\\' : ''}${n}`, readFileSync(R + n));
// the test documents: a Word file, and a .docx (Coda's Word opens it)
const docs: [string, string][] = [['Readme', resolve(top, '../psionEmulators/applib/epocutil/sndonoff/Readme.wrd')],
  ['Garden notes.docx', resolve(nb, 'tools/examples/files/Garden notes.docx')]];
for (const [n, f] of docs) if (existsSync(f)) addFile(rom, `Z:\\Test\\Documents\\${n}`, readFileSync(f));
for (const p of ['Z:\\System\\Controls\\Screenshot.ctl', 'Z:\\System\\Recogs\\PsiConv.mdl', 'Z:\\System\\Data\\PsiEdition.txt'])
  console.log(`${p}: ${find(rom, p).length ? 'there' : 'not there'}`);
const { img, notes } = write(rom);
writeFileSync(out, img);
for (const n of notes) console.log(n);
console.log(`PsiLinux in ${out} (Z:\\Test\\, the launcher in Jotter's place)`);
