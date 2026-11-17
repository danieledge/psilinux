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
// (tests/emu/netbook/build.sh) and build/nbtest/root/ROOT.IMG (mkroot.sh).
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
const R = `${b}/nbtest/root/`;
for (const n of readdirSync(R).filter(n => /^ROOT\.(IMG|I\d\d)$/.test(n)))
  addFile(rom, `Z:\\Test\\Linux\\${n}`, readFileSync(R + n));
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
