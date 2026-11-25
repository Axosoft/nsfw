const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { test } = require('node:test');
const { releaseNotes } = require('./release-notes');

const changelog = `# Changelog

## Unreleased

### Added
- Something new.

## 3.0.0 - 2026-11-25

### Added
- Prebuilt binaries.

### Changed
- The loader.

## [2.3.10]

- A later patch.

## 2.3.1

- A patch.
`;

test('returns the section of a version, without its heading', () => {
  assert.strictEqual(releaseNotes(changelog, '3.0.0'), '### Added\n- Prebuilt binaries.\n\n### Changed\n- The loader.');
});

test('matches bracketed headings, and the last section up to the end', () => {
  assert.strictEqual(releaseNotes(changelog, '2.3.10'), '- A later patch.');
  assert.strictEqual(releaseNotes(changelog, '2.3.1'), '- A patch.');
});

test('returns nothing for a missing version, or one that only prefixes another', () => {
  assert.strictEqual(releaseNotes(changelog, '2.3.0'), '');
  assert.strictEqual(releaseNotes(changelog, '2.3'), '');
  assert.strictEqual(releaseNotes(changelog, '3.0.0-beta.1'), '');
});

test('matches the dots of a version literally', () => {
  assert.strictEqual(releaseNotes('## 3x0x0\n\n- Not this.\n', '3.0.0'), '');
});

test('CLI fails on an empty section and prints the notes of a filled one', t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'nsfw-release-notes-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const file = path.join(directory, 'CHANGELOG.md');
  fs.writeFileSync(file, '## 3.0.1\n\n## 3.0.0\n\n- Notes.\n');
  const run = version => cp.spawnSync(process.execPath, [path.join(__dirname, 'release-notes.js'), version, file], {
    encoding: 'utf8'
  });
  const empty = run('3.0.1');
  assert.strictEqual(empty.status, 1);
  assert.match(empty.stderr, /## 3\.0\.1/);
  const filled = run('3.0.0');
  assert.strictEqual(filled.status, 0);
  assert.strictEqual(filled.stdout, '- Notes.\n');
});
