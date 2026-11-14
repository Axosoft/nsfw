/* eslint-disable no-console */
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');

function runChild(args, outputFile, timeoutMS = 60000) {
  return new Promise((resolve, reject) => {
    const fd = fs.openSync(outputFile, 'w');
    let child;
    try {
      child = cp.spawn(process.execPath, args, { cwd: repoRoot, stdio: ['ignore', fd, fd] });
    } finally {
      fs.closeSync(fd);
    }
    const startedAt = new Date().toISOString();
    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill('SIGKILL');
    }, timeoutMS);
    child.once('error', error => {
      clearTimeout(timer);
      reject(error);
    });
    child.once('close', (code, signal) => {
      clearTimeout(timer);
      resolve({ pid: child.pid, startedAt, code, signal, timedOut });
    });
  });
}

function collectReports(since, outputDir, reportDirs) {
  const reports = [];
  for (const [index, directory] of reportDirs.entries()) {
    let names;
    try {
      names = fs.readdirSync(directory);
    } catch (error) {
      if (error.code === 'ENOENT' || error.code === 'EACCES') continue;
      throw error;
    }
    for (const name of names) {
      if (!/^node.*\.(ips|crash)$/.test(name)) continue;
      const source = path.join(directory, name);
      const destination = path.join(outputDir, `${index}-${name}`);
      try {
        if (fs.statSync(source).mtimeMs < since) continue;
        fs.copyFileSync(source, destination);
      } catch (error) {
        if (error.code === 'ENOENT' || error.code === 'EACCES') continue;
        throw error;
      }
      reports.push({ source, destination });
    }
  }
  return reports;
}

async function main() {
  const repetitions = Number(process.argv[2] || 30);
  if (!Number.isInteger(repetitions) || repetitions < 1 || repetitions > 200) {
    throw new Error('repetitions must be an integer between 1 and 200');
  }
  const outputDir = path.resolve(process.argv[3] || 'teardown-diagnostics');
  fs.mkdirSync(outputDir);
  const started = Date.now();
  const revision = cp.execFileSync('git', ['rev-parse', 'HEAD'], { cwd: repoRoot, encoding: 'utf8' }).trim();
  const metadata = {
    revision, node: process.version, arch: process.arch, platform: process.platform,
    osRelease: os.release(), repetitions, startedAt: new Date(started).toISOString(),
    nodeAddonApi: require('node-addon-api/package.json').version
  };
  if (process.platform === 'darwin') {
    metadata.macOS = cp.execFileSync('sw_vers', { encoding: 'utf8' }).trim();
  }
  fs.writeFileSync(path.join(outputDir, 'metadata.json'), JSON.stringify(metadata, null, 2));
  console.log(metadata);
  const watchDir = fs.mkdtempSync(path.join(os.tmpdir(), 'nsfw-teardown-'));
  try {
    for (let iteration = 1; iteration <= repetitions; iteration++) {
      const result = await runChild([
        path.join(repoRoot, 'js/spec/lifecycle-child.js'), 'worker-terminate-busy', watchDir
      ], path.join(outputDir, `child-${iteration}.log`));
      const record = { iteration, ...result };
      fs.appendFileSync(path.join(outputDir, 'results.jsonl'), `${JSON.stringify(record)}\n`);
      console.log(record);
      if (result.code === 0 && result.signal === null && !result.timedOut) continue;
      process.exitCode = 1;
      // ReportCrash writes asynchronously after the child exits.
      if (process.platform === 'darwin') await new Promise(resolve => setTimeout(resolve, 10000));
      const reports = collectReports(started, outputDir, [
        path.join(os.homedir(), 'Library/Logs/DiagnosticReports'), '/Library/Logs/DiagnosticReports'
      ]);
      fs.writeFileSync(path.join(outputDir, 'reports.json'), JSON.stringify(reports, null, 2));
      console.error(`Child ${result.pid} failed; collected ${reports.length} crash reports in ${outputDir}`);
      break;
    }
  } finally {
    fs.rmSync(watchDir, { recursive: true, force: true });
  }
}

if (require.main === module) {
  main().catch(error => {
    console.error(error);
    process.exitCode = 1;
  });
}

module.exports = { runChild, collectReports };
