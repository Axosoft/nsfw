/* eslint-disable no-console */
// Checks that a packed nsfw tarball carries a prebuilt binary for every supported platform, along with what the
// install script needs to build one from source on any other, and the docs: README.md, LICENSE and CHANGELOG.md, which
// npm no longer adds on its own. Fails over a size budget. Prints a Markdown report of the sizes.
// Usage: node js/scripts/check-package.js nsfw-<version>.tgz
const fs = require('fs');
const zlib = require('zlib');

const prebuilds = [
  'darwin-arm64/nsfw.node',
  'darwin-x64/nsfw.node',
  'linux-arm64/nsfw.glibc.node',
  'linux-arm64/nsfw.musl.node',
  'linux-x64/nsfw.glibc.node',
  'linux-x64/nsfw.musl.node',
  'win32-arm64/nsfw.node',
  'win32-x64/nsfw.node'
].map(file => `prebuilds/${file}`);
const required = [...prebuilds, 'package.json', 'js/dist/index.js', 'js/dist/index.d.ts', 'binding.gyp', 'src/NSFW.cpp',
  'README.md', 'LICENSE', 'CHANGELOG.md'];
// About twice the size with all eight binaries (1.6 MB), so a binary that grows by a lot, or a stray file, fails.
const maxPackedBytes = 3 * 1024 * 1024;

const tarball = process.argv[2];
if (!tarball) {
  throw new Error('Usage: node js/scripts/check-package.js <tarball>');
}
const packed = fs.readFileSync(tarball);
const files = listTar(zlib.gunzipSync(packed));

const missing = required.filter(file => !files.has(file));
const unexpected = [...files.keys()].filter(file => file.startsWith('prebuilds/') && !prebuilds.includes(file));
const unpackedBytes = [...files.values()].reduce((sum, size) => sum + size, 0);

const kB = bytes => `${(bytes / 1024).toFixed(0)} kB`;
console.log(`### ${tarball}\n`);
console.log(`Packed ${kB(packed.length)}, unpacked ${kB(unpackedBytes)} in ${files.size} files.\n`);
console.log('| Prebuilt binary | Size |\n| --- | ---: |');
for (const file of prebuilds.filter(file => files.has(file))) {
  console.log(`| ${file} | ${kB(files.get(file))} |`);
}

const errors = [
  ...missing.map(file => `missing ${file}`),
  ...unexpected.map(file => `unexpected ${file}`),
  ...packed.length > maxPackedBytes ? [`packed size ${kB(packed.length)} exceeds ${kB(maxPackedBytes)}`] : []
];
if (errors.length) {
  console.error(errors.join('\n'));
  process.exit(1);
}

/**
 * Lists the regular files in an uncompressed tar archive, as npm pack writes it: ustar headers, no long names.
 * @param {Buffer} tar ...
 * @returns {Map<string, number>} The size of each file, by its path below the package/ directory.
 */
function listTar(tar) {
  const entries = new Map();
  for (let offset = 0; offset + 512 <= tar.length && tar[offset] !== 0;) {
    const field = (start, length) => tar.toString('utf8', offset + start, offset + start + length).replace(/\0.*$/s, '');
    const name = [field(345, 155), field(0, 100)].filter(Boolean).join('/');
    const size = parseInt(field(124, 12), 8);
    const type = field(156, 1);
    if (type === '0' || type === '') {
      entries.set(name.replace(/^package\//, ''), size);
    }
    offset += 512 + Math.ceil(size / 512) * 512;
  }
  return entries;
}
