/* eslint-disable no-console */
// Prints the CHANGELOG.md section of a version, the notes of its GitHub release. Fails when the section is missing or
// empty, so a release is never published without its changelog.
// Usage: node js/scripts/release-notes.js <version> [changelog]
const fs = require('fs');
const path = require('path');

/**
 * @param {string} changelog The contents of CHANGELOG.md.
 * @param {string} version A version, without the v of its tag.
 * @returns {string} The body of the `## <version>` section, without its heading, or an empty string.
 */
function releaseNotes(changelog, version) {
  const lines = changelog.split(/\r?\n/);
  // Also matches `## [1.2.3]` and `## 1.2.3 - 2026-11-25`.
  const heading = new RegExp(`^## \\[?${version.replace(/[.+]/g, '\\$&')}\\]?(\\s|$)`);
  const start = lines.findIndex(line => heading.test(line));
  if (start === -1) {
    return '';
  }
  const end = lines.findIndex((line, index) => index > start && /^## /.test(line));
  return lines.slice(start + 1, end === -1 ? undefined : end).join('\n').trim();
}

if (require.main === module) {
  const [version, changelogPath = path.join(__dirname, '..', '..', 'CHANGELOG.md')] = process.argv.slice(2);
  if (!version) {
    console.error('Usage: node js/scripts/release-notes.js <version> [changelog]');
    process.exit(1);
  }
  const notes = releaseNotes(fs.readFileSync(changelogPath, 'utf8'), version);
  if (!notes) {
    console.error(`${changelogPath} has no section, or an empty one, for ${version}: add a "## ${version}" heading`);
    process.exit(1);
  }
  console.log(notes);
}

module.exports = { releaseNotes };
