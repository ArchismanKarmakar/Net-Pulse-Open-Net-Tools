#!/usr/bin/env node
// Cross-platform SHA-256 manifest generator — avoids relying on `sha256sum`
// (Linux) vs `shasum -a 256` (macOS) vs neither (plain Windows cmd) being
// present/consistent across GitHub Actions runners, and avoids shell-quoting
// issues with filenames electron-builder produces that contain spaces/em
// dashes (e.g. "Net Pulse — Open Net Tools Setup 0.8.1.exe").
import { createHash } from 'node:crypto';
import { readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const dir = process.argv[2] || 'release';
const outFile = process.argv[3] || join(dir, 'SHA256SUMS.txt');

const lines = [];
for (const name of readdirSync(dir).sort()) {
  const full = join(dir, name);
  if (statSync(full).isDirectory()) continue;
  if (name.startsWith('SHA256SUMS')) continue; // never hash a checksum file into itself
  const hash = createHash('sha256').update(readFileSync(full)).digest('hex');
  lines.push(`${hash}  ${name}`);
}

const content = lines.join('\n') + (lines.length ? '\n' : '');
writeFileSync(outFile, content);
console.log(content);
