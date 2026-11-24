// Compiles js/src to js/dist from scratch, so no output of a removed source file is left to be tested or packed.
const cp = require('child_process');
const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');
// TypeScript 7 does not export bin/tsc, so it is found from the package root.
const tsc = path.join(path.dirname(require.resolve('typescript/package.json')), 'bin', 'tsc');

fs.rmSync(path.join(repoRoot, 'js', 'dist'), { recursive: true, force: true });
cp.execFileSync(process.execPath, [tsc, '-p', repoRoot], { stdio: 'inherit' });
