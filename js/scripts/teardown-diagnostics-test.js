const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { test } = require('node:test');
const { runChild, collectReports } = require('./teardown-diagnostics');

function temporaryDirectory(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'nsfw-diagnostic-test-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}

test('rejects an existing output directory without changing earlier evidence', t => {
  const directory = temporaryDirectory(t);
  const evidence = ['metadata.json', 'results.jsonl', 'child-1.log', 'reports.json', '0-node-crash.ips'];
  for (const name of evidence) fs.writeFileSync(path.join(directory, name), `earlier ${name}`);
  for (const output of [directory, temporaryDirectory(t)]) {
    const result = cp.spawnSync(process.execPath, [
      path.join(__dirname, 'teardown-diagnostics.js'), '1', output
    ], { encoding: 'utf8', timeout: 65000 });
    assert.ifError(result.error);
    assert.strictEqual(result.status, 1);
    assert.match(result.stderr, /EEXIST/);
  }
  assert.deepStrictEqual(fs.readdirSync(directory).sort(), evidence.sort());
  for (const name of evidence) {
    assert.strictEqual(fs.readFileSync(path.join(directory, name), 'utf8'), `earlier ${name}`);
  }
});

test('captures child output and successful exit', async t => {
  const log = path.join(temporaryDirectory(t), 'child.log');
  const result = await runChild(['-e', 'console.log("stdout"); console.error("stderr")'], log);
  assert.strictEqual(result.code, 0);
  assert.strictEqual(result.signal, null);
  assert.strictEqual(result.timedOut, false);
  assert.ok(result.pid > 0);
  assert.ok(Number.isFinite(Date.parse(result.startedAt)));
  assert.match(fs.readFileSync(log, 'utf8'), /stdout\nstderr/);
});

test('CLI stops after a failed child and preserves failure artifacts', { skip: process.platform === 'win32' }, t => {
  const directory = temporaryDirectory(t);
  const script = path.join(directory, 'js/scripts/teardown-diagnostics.js');
  const child = path.join(directory, 'js/spec/lifecycle-child.js');
  const output = path.join(directory, 'evidence');
  fs.mkdirSync(path.dirname(script), { recursive: true });
  fs.mkdirSync(path.dirname(child), { recursive: true });
  fs.copyFileSync(path.join(__dirname, 'teardown-diagnostics.js'), script);
  fs.writeFileSync(child, 'console.log("failed child"); process.kill(process.pid, "SIGTERM");\n');
  const repoRoot = path.resolve(__dirname, '../..');
  const gitDir = cp.execFileSync('git', ['rev-parse', '--absolute-git-dir'], {
    cwd: repoRoot, encoding: 'utf8'
  }).trim();
  const result = cp.spawnSync(process.execPath, [script, '2', output], {
    encoding: 'utf8', timeout: 65000,
    env: { ...process.env, GIT_DIR: gitDir, NODE_PATH: path.join(repoRoot, 'node_modules') }
  });
  assert.ifError(result.error);
  assert.strictEqual(result.status, 1);
  const records = fs.readFileSync(path.join(output, 'results.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
  assert.strictEqual(records.length, 1);
  assert.strictEqual(records[0].signal, 'SIGTERM');
  assert.strictEqual(records[0].code, null);
  assert.strictEqual(records[0].timedOut, false);
  assert.match(fs.readFileSync(path.join(output, 'child-1.log'), 'utf8'), /failed child/);
  assert.strictEqual(fs.existsSync(path.join(output, 'child-2.log')), false);
  assert.ok(Array.isArray(JSON.parse(fs.readFileSync(path.join(output, 'reports.json'), 'utf8'))));
});

test('preserves a synthetic child signal', { skip: process.platform === 'win32' }, async t => {
  const log = path.join(temporaryDirectory(t), 'child.log');
  const result = await runChild(['-e', 'process.kill(process.pid, "SIGTERM")'], log);
  assert.strictEqual(result.code, null);
  assert.strictEqual(result.signal, 'SIGTERM');
  assert.strictEqual(result.timedOut, false);
});

test('kills a hung child and records the timeout', async t => {
  const log = path.join(temporaryDirectory(t), 'child.log');
  const result = await runChild(['-e', 'setInterval(() => {}, 1000)'], log, 100);
  assert.strictEqual(result.timedOut, true);
  assert.notStrictEqual(result.code, 0);
});

test('collects only new Node crash reports from both report locations', t => {
  const directory = temporaryDirectory(t);
  const reportDirs = ['user', 'system'].map(name => path.join(directory, name));
  const output = path.join(directory, 'output');
  fs.mkdirSync(output);
  const since = Date.now() - 1000;
  for (const source of reportDirs) {
    fs.mkdirSync(source);
    fs.writeFileSync(path.join(source, 'node-current.ips'), 'current');
    fs.writeFileSync(path.join(source, 'node-old.crash'), 'old');
    fs.utimesSync(path.join(source, 'node-old.crash'), new Date(0), new Date(0));
    fs.writeFileSync(path.join(source, 'unrelated.ips'), 'unrelated');
  }
  const reports = collectReports(since, output, [...reportDirs, path.join(directory, 'missing')]);
  assert.strictEqual(reports.length, 2);
  assert.deepStrictEqual(fs.readdirSync(output).sort(), ['0-node-current.ips', '1-node-current.ips']);
  for (const report of reports) assert.strictEqual(fs.readFileSync(report.destination, 'utf8'), 'current');
});

for (const operation of ['statSync', 'copyFileSync']) {
  for (const code of ['ENOENT', 'EACCES', 'EIO']) {
    test(`handles ${code} during report ${operation}`, t => {
      const directory = temporaryDirectory(t);
      const sources = ['first', 'second'].map(name => path.join(directory, name));
      const output = path.join(directory, 'output');
      fs.mkdirSync(output);
      for (const source of sources) {
        fs.mkdirSync(source);
        fs.writeFileSync(path.join(source, 'node-current.ips'), 'report');
      }
      fs.writeFileSync(path.join(sources[0], 'node-unavailable.ips'), 'unavailable');
      const unavailable = path.join(sources[0], 'node-unavailable.ips');
      const original = fs[operation];
      t.mock.method(fs, operation, (...args) => {
        if (args[0] === unavailable) throw Object.assign(new Error('report unavailable'), { code });
        return original(...args);
      });
      if (code === 'EIO') {
        assert.throws(() => collectReports(0, output, sources), { code });
      } else {
        const reports = collectReports(0, output, sources);
        assert.strictEqual(reports.length, 2);
        assert.deepStrictEqual(fs.readdirSync(output).sort(), ['0-node-current.ips', '1-node-current.ips']);
        for (const report of reports) assert.strictEqual(fs.readFileSync(report.destination, 'utf8'), 'report');
      }
    });
  }
}
