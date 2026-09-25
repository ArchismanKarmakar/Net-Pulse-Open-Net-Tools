#!/usr/bin/env node
// Reads /VERSION (the one canonical source of truth for this project's
// version) and writes it into every file that needs its own copy:
// tauri-app/src-tauri/tauri.conf.json, tauri-app/src-tauri/Cargo.toml,
// tauri-app/package.json.
//
// WHY a version needs to exist in three separate places at all, rather than
// just reading /VERSION at build time: none of the three ecosystems
// involved (Cargo, npm, Tauri's own bundler) support pulling their
// "version" field from an arbitrary external file — each expects it
// present, literally, in its own manifest. tauri-version-release.yml's own
// "Check version consistency" step exists specifically because these three
// used to drift out of sync when bumped by hand (documented there: "0.9.1
// vs 0.9.2" was a real, shipped mismatch). This script is what makes "one
// edit changes everywhere" literally true: edit /VERSION, run this, done —
// rather than three manual edits a human has to remember to keep in lockstep.
//
// tauri-app/src-tauri/tauri.canary.conf.json deliberately has NO version
// field of its own and needs none — Tauri merges it over the base
// tauri.conf.json via JSON Merge Patch, so it inherits the version from
// there automatically. Adding one here would just be a second copy that
// could ALSO drift, defeating the entire point of this script.
//
// Usage: node scripts/sync-version.mjs [--check]
//   (no args)  rewrites all three files to match /VERSION
//   --check    verifies all three already match /VERSION; exits 1 and
//              prints exactly what's wrong if not, without changing
//              anything — this is what CI should call, so a human bumping
//              one file by hand and forgetting the others is caught the
//              same way it already was, just against a real single source
//              of truth instead of an arbitrary "pick one file to trust".

import { readFileSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..')
const checkOnly = process.argv.includes('--check')

const versionPath = path.join(root, 'VERSION')
const version = readFileSync(versionPath, 'utf8').trim()
if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(version)) {
  console.error(`::error::VERSION file contains "${version}", which is not a valid semver version`)
  process.exit(1)
}

const targets = [
  {
    path: path.join(root, 'tauri-app/src-tauri/tauri.conf.json'),
    read: (text) => JSON.parse(text).version,
    write: (text) => text.replace(/("version"\s*:\s*")[^"]*(")/, `$1${version}$2`),
  },
  {
    path: path.join(root, 'tauri-app/src-tauri/Cargo.toml'),
    // Cargo.toml has multiple `version = "..."` lines (the package's own,
    // and several dependency version REQUIREMENTS like `tauri = "2.11"`).
    // Only the first one, under [package], is this project's own version —
    // matching the exact line tauri-version-release.yml's own consistency
    // check already greps for (`grep -m1 '^version'`), so this stays
    // consistent with logic that already existed rather than introducing a
    // second, potentially-different way of finding the same line.
    read: (text) => {
      const m = text.match(/^version\s*=\s*"([^"]*)"/m)
      return m ? m[1] : null
    },
    write: (text) => text.replace(/^(version\s*=\s*")[^"]*(")/m, `$1${version}$2`),
  },
  {
    path: path.join(root, 'tauri-app/package.json'),
    read: (text) => JSON.parse(text).version,
    write: (text) => text.replace(/("version"\s*:\s*")[^"]*(")/, `$1${version}$2`),
  },
]

let mismatches = []
for (const t of targets) {
  const text = readFileSync(t.path, 'utf8')
  const current = t.read(text)
  if (current !== version) mismatches.push({ path: t.path, current })
}

if (checkOnly) {
  if (mismatches.length === 0) {
    console.log(`OK: all files match VERSION (${version})`)
    process.exit(0)
  }
  for (const m of mismatches) {
    console.error(`::error::${path.relative(root, m.path)} has version "${m.current}", VERSION file says "${version}"`)
  }
  process.exit(1)
}

for (const t of targets) {
  const text = readFileSync(t.path, 'utf8')
  const updated = t.write(text)
  writeFileSync(t.path, updated)
  console.log(`wrote ${path.relative(root, t.path)} -> ${version}`)
}
console.log(`\nAll files now at ${version}.`)
