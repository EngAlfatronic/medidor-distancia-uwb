// Builds www/ for the Android app from the web app in ../data (the same files the
// boards serve over Wi-Fi), adding the Capacitor runtime so the page can reach the
// native UsbSerial plugin. data/ stays build-free; this is the only "build" step.
import { cpSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const src = join(root, '..', 'data');
const out = join(root, 'www');

rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
cpSync(src, out, { recursive: true });

// IIFE build of @capacitor/core: defines window.capacitorExports (registerPlugin, ...).
cpSync(join(root, 'node_modules', '@capacitor', 'core', 'dist', 'capacitor.js'), join(out, 'capacitor.js'));

const indexPath = join(out, 'index.html');
const html = readFileSync(indexPath, 'utf8');
const marker = '<script src="medidor.js"></script>';
if (!html.includes(marker)) {
  throw new Error(`copy-web: "${marker}" not found in data/index.html`);
}
writeFileSync(indexPath, html.replace(marker, `<script src="capacitor.js"></script>\n  ${marker}`));

console.log(`copy-web: ${src} -> ${out}`);
