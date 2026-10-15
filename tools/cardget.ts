// linux/tools/cardget.ts - prints a file from an emulator card image, by path,
// or lists a folder:  node --experimental-strip-types linux/tools/cardget.ts CARD.img LINUX/PSILINUX.LOG
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '../..') + '/';
const EMU = process.env.PSION_EMU || resolve(REPO, '../psionEmulators');
const { findByPath, readFileBytes, listPath } = await import(EMU + '/frontend/src/lib/fat16.ts');
const [card, path] = process.argv.slice(2);
const img = new Uint8Array(readFileSync(card));
const e = findByPath(img, path.toUpperCase());
if (e && !e.isDirectory) process.stdout.write(Buffer.from(readFileBytes(img, e)));
else {
	const l = listPath(img, path.toUpperCase());
	if (!l) { console.error('not found: ' + path); process.exit(1); }
	for (const x of l) console.log((x.isDirectory ? 'd ' : 'f ') + x.name + ' ' + (x.size ?? ''));
}
