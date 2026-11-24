// Compiles js/src to js/dist from scratch, so no output of a removed source file is left to be tested or packed.
const cp = require('child_process');
const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');
const tsc = require.resolve('typescript/bin/tsc');

fs.rmSync(path.join(repoRoot, 'js', 'dist'), { recursive: true, force: true });
cp.execFileSync(process.execPath, [tsc, '-p', repoRoot], { stdio: 'inherit' });
