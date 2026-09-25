#!/usr/bin/env node
// Combines the per-OS `latest-<os>.json` manifests (each produced by ONE
// matrix job's `tauri build` via createUpdaterArtifacts:true, describing
// only that job's own platform(s)) into the single multi-platform
// `latest.json` the updater plugin actually expects at its configured
// endpoint (see tauri.conf.json's plugins.updater.endpoints — a single URL,
// not one per OS).
//
// Why this exists at all: the release workflow runs `tauri-action` in
// build-only mode (no tagName/releaseId) across three independent OS matrix
// jobs, specifically so a separate job owns creating the actual GitHub
// Release (see tauri-release.yml's own comment on that). That's the right
// call for keeping the matrix jobs independent, but it means tauri-cli
// never sees a real release to attach a combined manifest to — its normal
// "release mode" merging (fetch the current release's latest.json, add this
// platform, re-upload) never runs. Left alone, each OS's own single-platform
// latest.json would get uploaded to the SAME release under the SAME
// filename (latest.json) and silently overwrite each other — whichever OS's
// job happens to upload last would be the only platform anyone could ever
// update on. This script does that merge explicitly instead, as its own
// step, after all three are safely downloaded side by side under distinct
// names.
//
// Also rewrites each platform entry's `url` to the final GitHub Release
// download URL. tauri-cli fills in *some* url when it builds (often a local
// path or a guess, since the actual release/asset doesn't exist yet at
// build time in this workflow's design) — GitHub Release asset URLs are
// fully deterministic from (owner/repo, tag, filename) even before upload,
// so this recomputes the correct one from whatever filename tauri-cli
// originally referenced, rather than trusting the placeholder host/path.
import { readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';

const dir = process.argv[2] || 'release';
const outFile = process.argv[3] || join(dir, 'latest.json');
const repo = process.argv[4]; // "owner/repo"
const tag = process.argv[5];

if (!repo || !tag) {
  console.error('usage: merge-latest-json.mjs <dir> <outFile> <owner/repo> <tag>');
  process.exit(1);
}

const manifestFiles = readdirSync(dir).filter((f) => /^latest-.*\.json$/.test(f));
if (manifestFiles.length === 0) {
  console.error(`No latest-*.json manifests found in ${dir} — nothing to merge. ` +
    'Did every OS matrix job actually produce updater artifacts? (createUpdaterArtifacts:true ' +
    'needs a configured signing key to emit a usable signature — see tauri.conf.json\'s ' +
    'updater.pubkey and the TAURI_SIGNING_PRIVATE_KEY secret.)');
  process.exit(1);
}

let merged = null;
for (const file of manifestFiles) {
  const manifest = JSON.parse(readFileSync(join(dir, file), 'utf8'));
  if (!merged) {
    merged = { version: manifest.version, notes: manifest.notes, pub_date: manifest.pub_date, platforms: {} };
  } else if (merged.version !== manifest.version) {
    // Every OS job builds from the same tag/commit, so this should never
    // happen — surfacing it loudly beats silently shipping a manifest that
    // claims two different versions depending on which platform reads it.
    console.error(`Version mismatch: ${file} says ${manifest.version}, expected ${merged.version}`);
    process.exit(1);
  }
  for (const [platformKey, entry] of Object.entries(manifest.platforms || {})) {
    // GitHub replaces spaces with periods in the actual served
    // browser_download_url for an uploaded release asset — this project's
    // installer filenames do contain spaces (e.g. "Net Pulse Setup
    // 0.9.5.exe"), so the raw basename alone would produce a URL that
    // doesn't match what GitHub actually serves.
    const filename = basename(entry.url).replace(/ /g, '.');
    merged.platforms[platformKey] = {
      signature: entry.signature,
      url: `https://github.com/${repo}/releases/download/${tag}/${filename}`,
    };
  }
}

writeFileSync(outFile, JSON.stringify(merged, null, 2) + '\n');
console.log(`Merged ${manifestFiles.length} platform manifest(s) -> ${outFile}`);
console.log(JSON.stringify(merged, null, 2));
