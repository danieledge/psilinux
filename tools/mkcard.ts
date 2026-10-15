// linux/tools/mkcard.ts - a CF card image for the emulator with PsiLinux on it.
//   node --experimental-strip-types linux/tools/mkcard.ts probe|psilinux [out.img] [--file CARDPATH=HOSTFILE]...
// probe:    build/lxprobe-pkg/ (the M0 probe) as \System\Apps\LxProbe\
// psilinux: build/psilinux-pkg/ (the app and the runner) as \System\Apps\PsiLinux\
//           and, if built, the root image build/psilinux-root/ROOT.IMG as \Linux\ROOT.IMG
// The card writer makes 8.3 names only and holds about 16 entries a folder
// (see tools/emu/README.md), which is one reason the Linux files travel in
// one image file. Default output: build/emu/lxcard.img.
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = dirname(fileURLToPath(import.meta.url)) + '/';
const REPO = resolve(HERE, '../..') + '/';
const EMU = process.env.PSION_EMU || resolve(REPO, '../psionEmulators');
const { createBlankImage, writeFileAtPath } = await import(EMU + '/frontend/src/lib/fat16.ts');
const args = process.argv.slice(2);
const what = args.shift();
const extra: [string, string][] = [];
let out = REPO + 'build/emu/lxcard.img';
for (let i = 0; i < args.length; i++) {
	if (args[i] === '--file') { const [d, ...r] = args[++i].split('='); extra.push([d, r.join('=')]); }
	else out = resolve(args[i]);
}
const size = Number(process.env.EMU_CARD_MB || 32) * 1024 * 1024;
const img = createBlankImage(size);
const w = (p: string, d: Uint8Array) => { const r = writeFileAtPath(img, p, d); if (!r.ok) throw new Error(p + ': ' + r.reason); };
const f = (p: string) => new Uint8Array(readFileSync(p));
w('PAD.BIN', new Uint8Array(Number(process.env.EMU_PAD || 9000)));
w('SYSTEM/LIBS/ESTLIB.DLL', f(REPO + 'tools/emu/ESTLIB.DLL'));   // (libvterm needs the EPOC C library)
if (what === 'probe') {
	const P = REPO + 'build/lxprobe-pkg/';
	for (const n of ['lxprobe.app', 'lxprobe.rsc', 'lxprobe.exe'])
		w('SYSTEM/APPS/LXPROBE/' + n.toUpperCase(), f(P + n));
} else if (what === 'psilinux') {
	const P = process.env.EMU_PKG ? resolve(process.env.EMU_PKG) + '/' : REPO + 'build/psilinux-pkg/';
	// (8.3 names on the card: PSILINUX is exactly 8)
	for (const n of ['psilinux.app', 'psilinux.rsc', 'psilinux.aif', 'psilinux.mbm', 'psilxrun.exe', 'psiterm.gdr'])
		if (existsSync(P + n)) w('SYSTEM/APPS/PSILINUX/' + n.toUpperCase(), f(P + n));
	const R = process.env.EMU_ROOT || REPO + 'build/psilinux-root/ROOT.IMG';
	if (existsSync(R)) w('LINUX/ROOT.IMG', f(R));
} else {
	console.error('usage: mkcard.ts probe|psilinux [out.img] [--file CARDPATH=HOSTFILE]'); process.exit(1);
}
for (const [d, src] of extra) w(d.toUpperCase(), f(src));
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, img); console.log(out);
