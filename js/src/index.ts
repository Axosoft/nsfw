import { promises as fs, Stats } from 'fs';
import path = require('path');

type Implementation = nsfw.NSFW;

interface NativeBinding {
  // recursive: internal, false only reports the entries of watchPath itself, and follows no symlink. Defaults to true.
  new (watchPath: string, eventCallback: EventCallback,
    options: Required<Omit<nsfw.Options, 'nativeBinding'>> & { recursive?: boolean }): Implementation;
  getAllocatedInstanceCount?: () => number;
}

type EventCallback = (events: nsfw.FileChangeEvent[]) => void;

// webpack rewrites a require() of a computed path into a lookup among the modules it bundled, which never finds a .node
// file on disk.
declare const __webpack_require__: unknown;
declare const __non_webpack_require__: NodeJS.Require;
const runtimeRequire: NodeJS.Require = typeof __webpack_require__ === 'function' ? __non_webpack_require__ : require;

// require() caches by resolved file name, so every watcher of a file in a thread shares one addon. Distinct files, such
// as copies of the same binary, are separate addons with separate state.
function loadBindingFile(file: string): NativeBinding {
  try {
    return runtimeRequire(file) as NativeBinding;
  } catch (error) {
    throw bindingLoadError(file, (error as Error).message, error);
  }
}

function bindingLoadError(file: string, reason: string, cause?: unknown): Error {
  return new Error(`nsfw could not load its native binding from ${file}: ${reason}`, { cause });
}

// Loaded on first use, so that importing nsfw works where only an explicit nativeBinding can locate the addon.
let defaultBinding: NativeBinding | undefined;
function loadDefaultBinding(): NativeBinding {
  if (defaultBinding === undefined) {
    let file: string;
    try {
      // A local build in build/Release wins over the prebuilt binary for this platform in prebuilds/.
      // eslint-disable-next-line @typescript-eslint/no-require-imports
      file = require('node-gyp-build').path(path.join(__dirname, '..', '..'));
    } catch (error) {
      throw new Error(`nsfw could not find its native binding (${(error as Error).message.trim()}). Set the ` +
        'nativeBinding option or the NSFW_NATIVE_BINDING environment variable to the absolute path of its .node file.',
      { cause: error });
    }
    defaultBinding = loadBindingFile(file);
  }
  return defaultBinding;
}

// require() runs a file without the .node extension as JavaScript, tries other files and directories when the one
// named is missing, and resolves a relative path against a working directory that a packaged app rarely controls.
async function loadBindingPath(file: string, invalid: string): Promise<NativeBinding> {
  if (!path.isAbsolute(file) || path.extname(file) !== '.node') throw new TypeError(invalid);
  const stats = await fs.stat(file).catch((error: NodeJS.ErrnoException) => {
    if (error.code === 'ENOENT') return undefined;
    throw bindingLoadError(file, error.code ?? error.message, error);
  });
  if (!stats?.isFile()) throw bindingLoadError(file, 'no such file');
  return loadBindingFile(file);
}

async function resolveBinding(nativeBinding: unknown): Promise<NativeBinding> {
  if (nativeBinding === undefined) {
    const fromEnvironment = process.env.NSFW_NATIVE_BINDING;
    return fromEnvironment ?
      loadBindingPath(fromEnvironment, 'NSFW_NATIVE_BINDING must be the absolute path to a .node file.') :
      loadDefaultBinding();
  }
  const invalid = 'nativeBinding must be the absolute path to a .node file or the addon loaded from one.';
  if (typeof nativeBinding === 'string') return loadBindingPath(nativeBinding, invalid);
  // The addon exports the watcher class itself.
  if (typeof nativeBinding === 'function' && typeof nativeBinding.prototype?.start === 'function') {
    return nativeBinding as NativeBinding;
  }
  throw new TypeError(invalid);
}

const isWin32 = process.platform === 'win32';

// Throws unless excludedPaths is an array of absolute paths.
function validateExcludedPaths(excludedPaths: unknown): asserts excludedPaths is string[] {
  if (!Array.isArray(excludedPaths)) {
    throw new TypeError('excludedPaths must be an array of strings.');
  }
  for (const excludedPath of excludedPaths) {
    if (typeof excludedPath !== 'string') {
      throw new TypeError('excludedPaths elements must be strings.');
    }
    if (!path.isAbsolute(excludedPath)) {
      throw new Error('Excluded path must be an absolute path.');
    }
  }
}

// Once per process, as Node does for its own deprecations.
let warnedExcludedPaths = false;
function warnExcludedPaths(): void {
  if (warnedExcludedPaths) return;
  warnedExcludedPaths = true;
  process.emitWarning(
    'excludedPaths, updateExcludedPaths() and getExcludedPaths() are deprecated: use excludedPatterns, ' +
    'updateExcludedPatterns() and getExcludedPatterns() instead (\'/build\' excludes build in the watched directory).',
    { type: 'DeprecationWarning', code: 'NSFW_EXCLUDED_PATHS' });
}

function validateExcludedPatterns(excludedPatterns: unknown): asserts excludedPatterns is string[] {
  if (!Array.isArray(excludedPatterns) || excludedPatterns.some(pattern => typeof pattern !== 'string')) {
    throw new TypeError('excludedPatterns must be an array of strings.');
  }
}

const isExtendedUnc = (value: string): boolean => value.slice(0, 8).toLowerCase() === '\\\\?\\unc\\';

function stripTrailingSeparators(value: string): string {
  const rootLength = isWin32 && isExtendedUnc(value) ?
    path.parse('\\\\' + value.slice(8)).root.length + 6 : path.parse(value).root.length;
  while (value.length > rootLength && (value.endsWith('/') || (isWin32 && value.endsWith('\\')))) {
    value = value.slice(0, -1);
  }
  return value;
}

// Node's metadata APIs reject extended drive roots that the native watcher can open.
const metadataPath = (value: string): string =>
  isWin32 && /^\\\\\?\\[A-Za-z]:\\$/.test(value) ? value.slice(4) : value;

async function canonicalPath(value: string): Promise<string> {
  if (isWin32) {
    if (isExtendedUnc(value)) {
      value = '\\\\' + value.slice(8);
    } else if (value.startsWith('\\\\?\\') && /^[a-z]:/i.test(value.slice(4))) {
      value = value.slice(4);
    }
    return stripTrailingSeparators(path.toNamespacedPath(path.resolve(value)));
  }
  // realpath must see symlink/.. before any lexical normalization.
  return fs.realpath(stripTrailingSeparators(value));
}

const pathComponents = (value: string): string[] =>
  value.split('/').filter(component => component !== '' && component !== '.');

// realpath(3) can resolve `..` as text, also inside symlink targets, without the search permission check of the
// kernel's own lookup. A spelling it returns only counts when that lookup reaches the same file.
async function isSameFile(lookup: string, physical: string): Promise<boolean> {
  const [found, expected] = await Promise.all([fs.stat(lookup, { bigint: true }), fs.stat(physical, { bigint: true })]);
  return found.dev === expected.dev && found.ino === expected.ino;
}

type ExclusionResolver = (value: string) => Promise<string | null>;

// Plain children stay lexical so they may name future paths or internal symlinks. A prefix containing `..`
// must pass one cumulative kernel lookup, preserving search permissions and the symlink traversal limit.
async function createPosixExclusionResolver(watchPath: string, canonicalWatchPath: string): Promise<ExclusionResolver> {
  const canonicalRoot = pathComponents(canonicalWatchPath);
  const knownRoots = [pathComponents(watchPath), canonicalRoot];
  const lexicalRoot = path.resolve(watchPath);
  if (lexicalRoot !== canonicalWatchPath) {
    try {
      if (await fs.realpath(lexicalRoot) === canonicalWatchPath && await isSameFile(lexicalRoot, canonicalWatchPath)) {
        knownRoots.push(pathComponents(lexicalRoot));
      }
    } catch { /* A collapsed spelling need not exist. */ }
  }
  const childPrefix = canonicalWatchPath === '/' ? '/' : canonicalWatchPath + '/';
  const isInside = (value: string): boolean => value === canonicalWatchPath || value.startsWith(childPrefix);

  return async value => {
    let components = pathComponents(value);
    const knownRoot = knownRoots.find(root => root.every((component, index) => component === components[index]));
    if (knownRoot !== undefined) {
      // Frozen root aliases remain bound to the original target even after a symlink is repointed.
      components = [...canonicalRoot, ...components.slice(knownRoot.length)];
    }
    const lastParent = components.lastIndexOf('..');
    if (knownRoot !== undefined && lastParent < canonicalRoot.length) {
      return '/' + components.join('/');
    }

    // Unknown aliases are probed cumulatively until their spelling enters the watched namespace. Never
    // rebuild a later lookup from realpath's answer: that would reset the kernel's traversal budget.
    for (let end = Math.max(1, lastParent + 1); end <= components.length; end++) {
      const lookup = '/' + components.slice(0, end).join('/');
      let physical;
      try {
        physical = await fs.realpath(lookup);
        if (!await isSameFile(lookup, physical)) return null;
      } catch {
        return null;
      }
      if (isInside(physical)) {
        return path.join(physical, ...components.slice(end));
      }
    }
    return null;
  };
}

// The directory the native watchers reported for the watched path before they received canonical paths.
function legacyDirectory(watchPath: string): string {
  if (!isWin32) return watchPath !== '/' && watchPath.endsWith('/') ? watchPath.slice(0, -1) : watchPath;
  if (watchPath.startsWith('\\\\?\\')) return watchPath;
  const resolved = path.resolve(watchPath);
  return resolved + (/[\\/]$/.test(watchPath) && !resolved.endsWith(path.sep) ? path.sep : '');
}

type DirectoryMapper = (value: string) => string;

// Native events carry canonical directories, while Linux and Windows callers keep receiving the legacy spelling.
// Returns undefined when no directory needs rewriting.
function createDirectoryMapper(watchPath: string, canonicalWatchPath: string, canonicalChildPrefix: string,
  contains: (value: string) => boolean): DirectoryMapper | undefined {
  if (process.platform === 'darwin') return undefined;
  const reportedBase = legacyDirectory(watchPath);
  const reportedParent = stripTrailingSeparators(reportedBase);
  const reportedChildPrefix = isWin32 ? reportedParent + (reportedParent.endsWith(path.sep) ? '' : path.sep) :
    reportedBase + (watchPath === '/' ? '' : '/');
  if (reportedBase === canonicalWatchPath && reportedChildPrefix === canonicalChildPrefix) return undefined;
  return value => {
    if (!contains(value)) return value;
    const suffix = value.slice(canonicalChildPrefix.length);
    return suffix ? reportedChildPrefix + suffix : reportedBase;
  };
}

interface PathContext {
  watchPath: string;
  directory: DirectoryMapper | undefined;
  isWatchPath: (value: string) => boolean;
  excludedPaths: (paths: unknown) => Promise<string[]>;
}

async function createPathContext(watchPath: string): Promise<PathContext> {
  const canonicalWatchPath = await canonicalPath(watchPath);
  if (!isWin32 && !await isSameFile(watchPath, canonicalWatchPath)) throw new Error('Not the watched path.');
  const compare = (value: string): string => isWin32 ? value.toLowerCase() : value;
  const base = compare(canonicalWatchPath);
  const canonicalChildPrefix = canonicalWatchPath.endsWith(path.sep) ? canonicalWatchPath :
    canonicalWatchPath + path.sep;
  const prefix = compare(canonicalChildPrefix);
  const contains = (value: string): boolean => {
    const compared = compare(value);
    return compared === base || compared.startsWith(prefix);
  };
  // Win32 itself resolves `..` as a string, before any reparse point.
  const excludedPath: ExclusionResolver = isWin32 ? canonicalPath :
    await createPosixExclusionResolver(watchPath, canonicalWatchPath);

  return {
    watchPath: canonicalWatchPath,
    directory: createDirectoryMapper(watchPath, canonicalWatchPath, canonicalChildPrefix, contains),
    isWatchPath: value => compare(value) === base,
    async excludedPaths(paths) {
      validateExcludedPaths(paths);
      return (await Promise.all(paths.map(excludedPath)))
        .filter((value): value is string => value !== null && contains(value));
    }
  };
}

// The most the native watcher takes. A watched file takes any debounce, so longer ones are capped to it.
const MAX_NATIVE_DEBOUNCE_MS = 60000;

// Escapes name into a pattern that matches it alone. A line break can't be escaped, so `?` stands for it: the
// events of the other names it lets through are dropped by name.
function escapePatternName(name: string): string {
  return name.replace(/[\\*?[\]!# ]/g, '\\$&').replace(/[\r\n]/g, '?');
}

type FileChange = 'appeared' | 'gone' | 'changed';

// How often a file watcher whose directory is gone looks for it. On whole seconds of the monotonic clock, as the Linux
// watcher of a directory checks its path, so that the watchers of a process wake together.
const DIRECTORY_CHECK_MS = 1000;
const untilNextCheck = (): number =>
  DIRECTORY_CHECK_MS - Number(process.hrtime.bigint() / 1000000n % BigInt(DIRECTORY_CHECK_MS));

// Whether a file looked at twice is the same one, unchanged.
const unchanged = (before: Stats | undefined, after: Stats): boolean => before !== undefined &&
  before.dev === after.dev && before.ino === after.ino && before.size === after.size &&
  before.mtimeMs === after.mtimeMs && before.ctimeMs === after.ctimeMs;

// Watches the directory of the file, natively filtered down to the file's own entry: its siblings are not reported
// and, on Linux, its subdirectories cost no inotify watch. Like the poller it replaces, it outlives its directory: when
// the watcher of the directory fails, as when the directory is renamed or removed, the file's state is looked at once,
// and the directory is looked for once a second, without any inotify watch, until it can be watched again.
async function createFileWatcher(NSFW: NativeBinding, watchedFile: string, reportedPath: string,
  eventCallback: EventCallback, errorCallback: ((error: unknown) => void) | undefined, debounceMS: number,
  excludedPaths: string[], isExcluded: (value: string) => boolean,
  excludedPatterns: string[]): Promise<Implementation> {
  // Writes through a symlink notify the directory of the file it leads to. Elsewhere canonical paths are real already.
  let target = watchedFile;
  if (isWin32) {
    try {
      target = await canonicalPath(await fs.realpath(watchedFile));
    } catch { /* Some filesystems can't tell the final path of a file: it is watched as named. */ }
  }
  let parent: string;
  try {
    parent = await canonicalPath(path.dirname(target));
  } catch {
    throw new Error('Path must be a valid path to a file or a directory.');
  }
  const name = path.basename(target);
  const compare = (value: string): string => isWin32 || process.platform === 'darwin' ? value.toLowerCase() : value;
  const isTarget = (value: string): boolean => compare(value) === compare(name);
  const directory = path.dirname(reportedPath);
  const file = path.basename(reportedPath);

  let currentExcludedPaths = excludedPaths;
  // Kept, though only a watched directory applies them.
  let currentExcludedPatterns = excludedPatterns;

  // watching: the native watcher runs, from just before it starts. waiting: its directory is looked for once a second.
  // paused: the native watcher runs paused. paused-waiting: neither runs.
  let state: 'stopped' | 'watching' | 'waiting' | 'paused' | 'paused-waiting' = 'stopped';
  // What one start() after a stop begins, until the next stop.
  interface Session {
    // The stop() calls made before the start() that began it: any later one ends it, before its job has even run.
    readonly stops: number;
    // Whether the file is there: a file renamed over the watched one, which the native watcher reports as created, is
    // reported as modified.
    exists?: boolean;
    // The file as last looked at, to tell whether it changed while no event could tell it.
    lastSeen?: Stats;
    observedDuringStat?: boolean;
    // The native watcher failed: the next job takes it into account before running.
    failed: boolean;
    // Whether the error of a directory that is there but can't be watched again was told since it last could be.
    rearmErrorDelivered: boolean;
    // A failure while watching whose look at the file a pause cut short: resuming reports what that look would have.
    unsettled?: boolean;
    directoryCheck?: ReturnType<typeof setTimeout>;
    // Replaced whenever a look for the directory is scheduled or cancelled, so that one under way can't act for
    // another.
    check?: object;
  }
  let session: Session | undefined;
  let stops = 0;
  const live = (current: Session): boolean => current === session && current.stops === stops;

  // One at a time in call order, as the native watcher runs them, so each finds the state the one before left: a pause
  // can't be undone by a start called before it.
  let jobs: Promise<unknown> = Promise.resolve();
  const serial = <T>(job: () => Promise<T>): Promise<T> => {
    const result = jobs.then(async () => {
      await takeFailure();
      return job();
    });
    jobs = result.catch(() => {});
    return result;
  };
  // A throw from a callback is uncaught, as for a directory, instead of ending what called it.
  const uncaught = (callback: () => void): void => {
    try {
      callback();
    } catch (thrown) {
      process.nextTick(() => { throw thrown; });
    }
  };

  const report = (current: Session, changes: FileChange[]): void => {
    const events: nsfw.FileChangeEvent[] = [];
    for (const change of changes) {
      let action: nsfw.ActionType.CREATED | nsfw.ActionType.DELETED | nsfw.ActionType.MODIFIED;
      if (change === 'gone') {
        if (!current.exists) continue;
        action = nsfw.ActionType.DELETED;
      } else if (!current.exists) {
        action = nsfw.ActionType.CREATED;
      } else {
        action = nsfw.ActionType.MODIFIED;
      }
      current.exists = change !== 'gone';
      // Replaced within a batch, say by a rename over it that Windows reports as a removal first: modified.
      if (action === nsfw.ActionType.CREATED && events[events.length - 1]?.action === nsfw.ActionType.DELETED) {
        events.pop();
        action = nsfw.ActionType.MODIFIED;
      }
      if (action === nsfw.ActionType.MODIFIED && events[events.length - 1]?.action === action) continue;
      events.push({ action, kind: nsfw.EntryKind.FILE, directory, file });
    }
    if (events.length > 0 && !currentExcludedPaths.some(isExcluded)) uncaught(() => eventCallback(events));
  };

  const look = (): Promise<Stats | undefined> => fs.stat(target).catch(() => undefined);
  const isDirectory = (value: string): Promise<boolean> =>
    fs.stat(value).then(stats => stats.isDirectory(), () => false);
  // Should Node's metadata APIs reject an extended share root, as they do an extended drive root, the share is looked
  // at through its plain spelling.
  const directoryExists = async (): Promise<boolean> => await isDirectory(metadataPath(parent)) ||
    (isWin32 && isExtendedUnc(parent) && isDirectory('\\\\' + parent.slice(8)));

  // A single look once watching starts, as no event tells whether the file is there. Taken after the watch is armed:
  // it sees what changed before, and start() resolves knowing the state every later change is told from. When the
  // watcher comes back after a failure, what changed meanwhile is reported ('changes'). When it resumes from a pause
  // that kept the look after a failure from reporting, only a removal is ('removal'), as a pause takes the rest as is.
  const establish = async (current: Session, reconcile: 'none' | 'removal' | 'changes'): Promise<void> => {
    const before = current.exists;
    const previous = current.lastSeen;
    current.exists = undefined;
    current.observedDuringStat = undefined;
    const stats = await look();
    current.lastSeen = stats;
    const touched = current.observedDuringStat !== undefined;
    const found = current.observedDuringStat ?? stats !== undefined;
    if (reconcile === 'none') {
      current.exists = found;
      return;
    }
    current.exists = before;
    if (!live(current)) return;
    if (!found) {
      report(current, ['gone']);
    } else if (reconcile === 'removal') {
      current.exists = true;
    } else if (!before || touched || !stats || !unchanged(previous, stats)) {
      report(current, ['changed']);
    }
  };

  const onEvents = (events: nsfw.FileChangeEvent[]): void => {
    const current = session;
    if (!current || state !== 'watching') return;
    const changes: FileChange[] = [];
    for (const event of events) {
      if (event.action === nsfw.ActionType.RENAMED) {
        const from = isTarget(event.oldFile);
        const to = isTarget(event.newFile);
        // Both: renamed to another case of its name on a case-insensitive filesystem.
        if (from || to) changes.push(from && to ? 'changed' : from ? 'gone' : 'appeared');
      } else if (isTarget(event.file)) {
        changes.push(event.action === nsfw.ActionType.CREATED ? 'appeared' :
          event.action === nsfw.ActionType.DELETED ? 'gone' : 'changed');
      }
    }
    if (current.exists !== undefined) {
      report(current, changes);
    } else if (changes.length > 0) {
      // A callback can arrive after stat observed the file but before its result reaches JavaScript.
      current.observedDuringStat = changes[changes.length - 1] !== 'gone';
    }
  };

  // Whatever failed, the native watcher has stopped: its directory renamed or removed, its event queue overflowed, a
  // network share gone. None of it reaches errorCallback, as the poller never failed. A stop() called meanwhile stops
  // the native watcher itself.
  const onError = (): void => {
    const current = session;
    if (!current || !live(current) || (state !== 'watching' && state !== 'paused')) return;
    current.failed = true;
    void serial(async () => {});
  };

  const pattern = '/' + escapePatternName(name);
  // js/spec/index-slow-spec.js builds a native watcher with these options to test what FSEvents losses it reports.
  const native = new NSFW(parent, onEvents, {
    debounceMS: Math.min(debounceMS, MAX_NATIVE_DEBOUNCE_MS),
    errorCallback: onError,
    excludedPaths: [],
    // Nothing below the entry either, should a directory replace the file.
    excludedPatterns: ['/*', '!' + pattern, pattern + '/*'],
    respectGitignore: false,
    followFileSymlinks: false,
    recursive: false
  });

  const cancelDirectoryCheck = (current: Session): void => {
    clearTimeout(current.directoryCheck);
    current.directoryCheck = undefined;
    current.check = undefined;
  };

  // Looks for the directory from the next whole second on. After a failure, first looks at the file, to report its
  // removal if it went with its directory. Both looks run outside the job queue, so that a stat stalled on a share gone
  // away holds no stop() or pause().
  const scheduleDirectoryCheck = (current: Session, afterFailure = false): void => {
    cancelDirectoryCheck(current);
    const check = {};
    current.check = check;
    const due = (): boolean => current.check === check && live(current) && state === 'waiting';
    if (afterFailure) {
      current.unsettled = true;
      void look().then(stats => {
        if (!due()) return;
        current.unsettled = false;
        if (!stats) report(current, ['gone']);
        // eventCallback may have stopped the watcher.
        if (due()) scheduleDirectoryCheck(current);
      });
      return;
    }
    current.directoryCheck = setTimeout(async () => {
      current.directoryCheck = undefined;
      const found = await directoryExists();
      if (!due()) return;
      if (!found) {
        scheduleDirectoryCheck(current);
        return;
      }
      void serial(async () => {
        if (!due()) return;
        try {
          if (await arm(current, true)) {
            await establish(current, 'changes');
            return;
          }
        } catch (error) {
          if (!live(current)) return;
          // The directory is back but can't be watched, say for want of inotify watches: told once, and tried again.
          if (!current.rearmErrorDelivered && errorCallback) {
            current.rearmErrorDelivered = true;
            uncaught(() => errorCallback?.(error));
          }
        }
        if (live(current)) scheduleDirectoryCheck(current);
      });
    }, untilNextCheck());
  };

  // Starts the native watcher and resolves true, or leaves the watcher waiting and resolves false when the directory is
  // not there; rejects when it is but can't be watched. The directory is looked at first, unless it was just found, as
  // on macOS a watch of a missing path starts all the same, and again after, should it have gone meanwhile.
  const arm = async (current: Session, found = false): Promise<boolean> => {
    state = 'waiting';
    if (!found && !await directoryExists()) return false;
    // Before the run exists, so that its error is taken.
    state = 'watching';
    try {
      await native.start();
    } catch (error) {
      state = 'waiting';
      if (!await directoryExists()) return false;
      throw error;
    }
    if (await directoryExists()) {
      current.rearmErrorDelivered = false;
      return true;
    }
    state = 'waiting';
    await native.stop().catch(() => {});
    return false;
  };

  // The native watcher failed: the file's removal, if it went with its directory, is reported, and its directory is
  // watched again, or looked for, from the next whole second on, so that a watcher that keeps failing can't spin.
  const takeFailure = async (): Promise<void> => {
    const current = session;
    if (!current?.failed) return;
    current.failed = false;
    if (!live(current)) return;
    await native.stop().catch(() => {});
    if (state === 'paused') {
      // The native watcher stays paused through its runs until resumed.
      await native.resume();
      state = 'paused-waiting';
      return;
    }
    state = 'waiting';
    scheduleDirectoryCheck(current, true);
  };

  // Unlike a directory watcher's, these never reject over the state they find: starting or resuming a running watcher
  // and stopping or pausing one that isn't do nothing, and resuming starts a stopped one. Nor do they reject when the
  // file's directory is gone: it is watched once it is back.
  const start = (): Promise<void> => {
    const stopsBefore = stops;
    return serial(async () => {
      if (state === 'watching' || state === 'waiting') return;
      if (state === 'paused' && session) {
        await native.resume();
        state = 'watching';
        await establish(session, 'none');
        return;
      }
      const resumed = state === 'paused-waiting' && session !== undefined;
      const current: Session = resumed && session ? session : { stops: stopsBefore, failed: false,
        rearmErrorDelivered: false };
      session = current;
      // What the run reports before the look establish() takes is told from that look, unless a failure left the file's
      // state to settle from the one before it.
      const unsettled = current.unsettled ?? false;
      current.unsettled = false;
      if (!unsettled) current.exists = undefined;
      let watching;
      try {
        watching = await arm(current);
      } catch (error) {
        // A resumed watcher keeps waiting for a directory it can watch.
        if (!resumed) {
          state = 'stopped';
          session = undefined;
          throw error;
        }
        watching = false;
      }
      await establish(current, unsettled ? 'removal' : 'none');
      if (!watching && live(current)) scheduleDirectoryCheck(current);
    });
  };

  return {
    start,
    resume: start,
    stop() {
      stops++;
      return serial(async () => {
        const current = session;
        if (!current) return;
        const was = state;
        state = 'stopped';
        session = undefined;
        cancelDirectoryCheck(current);
        // The native watcher stays paused through its runs until resumed.
        if (was === 'paused') await native.resume();
        // A run that failed meanwhile has stopped already.
        if (was === 'watching' || was === 'paused') await native.stop().catch(() => {});
      });
    },
    pause: () => serial(async () => {
      if (state === 'watching') {
        state = 'paused';
        await native.pause();
      } else if (state === 'waiting' && session) {
        cancelDirectoryCheck(session);
        state = 'paused-waiting';
      }
    }),
    getExcludedPaths: async () => [...currentExcludedPaths],
    updateExcludedPaths: async paths => {
      currentExcludedPaths = paths;
      return [...currentExcludedPaths];
    },
    getExcludedPatterns: async () => [...currentExcludedPatterns],
    updateExcludedPatterns: async patterns => {
      currentExcludedPatterns = patterns;
      return [...currentExcludedPatterns];
    }
  };
}

async function buildNSFW(watchPath: string, eventCallback: EventCallback, {
  debounceMS = 500,
  errorCallback: _errorCallback,
  excludedPaths,
  excludedPatterns = [],
  nativeBinding,
  respectGitignore = false,
  followFileSymlinks = false
}: Partial<nsfw.Options> = {}): Promise<{ implementation: Implementation; context: PathContext }> {
  if (excludedPaths === undefined) {
    excludedPaths = [];
  } else {
    warnExcludedPaths();
  }
  if (Number.isInteger(debounceMS)) {
    if (debounceMS < 1) {
      throw new Error('Minimum debounce is 1ms.');
    }
  } else {
    throw new Error('debounceMS must be an integer.');
  }

  validateExcludedPatterns(excludedPatterns);
  if (typeof respectGitignore !== 'boolean') {
    throw new TypeError('respectGitignore must be a boolean.');
  }
  if (typeof followFileSymlinks !== 'boolean') {
    throw new TypeError('followFileSymlinks must be a boolean.');
  }
  const NSFW = await resolveBinding(nativeBinding);

  const errorCallback = _errorCallback || ((nsfwError: unknown) => { throw nsfwError; });

  if (!path.isAbsolute(watchPath)) {
    throw new Error('Path to watch must be an absolute path.');
  }

  let context: PathContext, stats: Stats;
  try {
    context = await createPathContext(watchPath);
    stats = await fs.stat(metadataPath(context.watchPath));
  } catch {
    throw new Error('Path must be a valid path to a file or a directory.');
  }

  if (typeof eventCallback !== 'function') throw new TypeError('eventCallback must be a function.');

  const effectivePaths = await context.excludedPaths(excludedPaths);
  const { directory } = context;
  const callback: EventCallback = directory === undefined ? eventCallback :
    events => eventCallback(events.map(event => ({
      ...event,
      directory: directory(event.directory),
      ...('newDirectory' in event && event.newDirectory !== undefined ?
        { newDirectory: directory(event.newDirectory) } : {})
    })));
  let implementation: Implementation;

  if (stats.isDirectory()) {
    implementation = new NSFW(context.watchPath, callback,
      {
        debounceMS,
        errorCallback,
        excludedPaths: effectivePaths,
        excludedPatterns: [...excludedPatterns],
        respectGitignore,
        followFileSymlinks
      });
  } else if (stats.isFile()) {
    if (_errorCallback && typeof _errorCallback !== 'function') {
      throw new TypeError('options.errorCallback must be a function.');
    }
    implementation = await createFileWatcher(NSFW, context.watchPath, watchPath, eventCallback,
      _errorCallback || undefined, debounceMS, effectivePaths, context.isWatchPath, [...excludedPatterns]);
  } else {
    throw new Error('Path must be a valid path to a file or a directory');
  }
  return { implementation, context };
}

function createWatcher(implementation: Implementation, context: PathContext): nsfw.NSFW {
  // Calls reach the watcher in call order. Canonicalizing an update can wait for the filesystem, so a call made
  // while an earlier one has not reached the watcher yet waits behind it; any other call goes straight to the
  // watcher, which creates its job synchronously.
  let waiting = 0;
  let updates: Promise<unknown> = Promise.resolve();
  const whenReady = <T>(call: () => Promise<T>): Promise<T> => {
    if (waiting === 0) return call();
    waiting++;
    return updates.then(() => {
      waiting--;
      return call();
    });
  };
  const watcher: nsfw.NSFW = {
    start: () => whenReady(() => implementation.start()),
    stop: () => whenReady(() => implementation.stop()),
    pause: () => whenReady(() => implementation.pause()),
    resume: () => whenReady(() => implementation.resume()),
    getExcludedPaths: () => {
      warnExcludedPaths();
      return whenReady(() => implementation.getExcludedPaths());
    },
    getExcludedPatterns: () => whenReady(() => implementation.getExcludedPatterns()),
    updateExcludedPatterns: async patterns => {
      validateExcludedPatterns(patterns);
      const snapshot = [...patterns];
      return whenReady(() => implementation.updateExcludedPatterns(snapshot));
    },
    updateExcludedPaths: async paths => {
      warnExcludedPaths();
      validateExcludedPaths(paths);
      const snapshot = [...paths];
      waiting++;
      const update = updates.then(async () => {
        let canonicalPaths;
        try {
          canonicalPaths = await context.excludedPaths(snapshot);
        } finally {
          waiting--;
        }
        // The native FIFO orders execution; waiting for completion here would keep stop()
        // from reaching an update that needs to yield its unfinished scan to shutdown.
        return { result: implementation.updateExcludedPaths(canonicalPaths) };
      });
      updates = update.catch(() => {});
      return (await update).result;
    }
  };
  // Watchers were once constructed by nsfw itself, so `watcher instanceof nsfw` keeps holding.
  return Object.assign(Object.create(nsfw.prototype), watcher);
}

/**
 * Returns a Promise that resolves to the created NSFW Object.
 * @param watchPath - the path that nsfw should watch
 * @param eventCallback - callback that will be fired when NSFW has change events
 * @param options - options
 */
function nsfw(watchPath: string, eventCallback: (events: Array<nsfw.FileChangeEvent>) => void,
  options?: Partial<nsfw.Options>): Promise<nsfw.NSFW> {
  return buildNSFW(watchPath, eventCallback, options)
    .then(({ implementation, context }) => createWatcher(implementation, context));
}

// The namespace carries the public types; `export =` needs it merged with the function.
// eslint-disable-next-line @typescript-eslint/no-namespace
namespace nsfw {
  export const actions: typeof ActionType = {
    CREATED: ActionType.CREATED,
    DELETED: ActionType.DELETED,
    MODIFIED: ActionType.MODIFIED,
    RENAMED: ActionType.RENAMED
  };

  export const kinds: typeof EntryKind = {
    FILE: EntryKind.FILE,
    DIRECTORY: EntryKind.DIRECTORY,
    UNKNOWN: EntryKind.UNKNOWN
  };

  export interface NSFW {
    /** Returns a Promise that resolves after NSFW has paused listening to events. */
    pause: () => Promise<void>;
    /** Returns a Promise that resolves after NSFW has resumed listening to events. */
    resume: () => Promise<void>;
    /** Returns a Promise that resolves when the NSFW object has started watching the path. */
    start: () => Promise<void>;
    /** Returns a Promise that resolves when NSFW object has halted. */
    stop: () => Promise<void>;
    /**
     * Returns a Promise that resolves with the excluded paths: absolute paths inside the watched path, spelled
     * under its canonical form (its real path on Linux and macOS, its `\\?\` form on Windows).
     * @deprecated Use `getExcludedPatterns()` and `excludedPatterns` instead.
     */
    getExcludedPaths: () => Promise<string[]>;
    /**
     * Replaces the excluded paths. Rejects unless every path is absolute, and ignores the ones outside the watched
     * path; resolves with the paths this call set, without those and without trailing separators, spelled
     * like the ones getExcludedPaths() returns, even if a later call has replaced them.
     * @deprecated Use `updateExcludedPatterns()` instead: `/build` excludes `build` in the watched directory.
     */
    updateExcludedPaths: (excludedPaths: string[]) => Promise<string[]>;
    /** Returns a Promise that resolves with the excluded patterns. */
    getExcludedPatterns: () => Promise<string[]>;
    /**
     * Replaces the excluded patterns, see `Options.excludedPatterns`. Rejects unless every pattern is a string;
     * resolves with the patterns this call set, even if a later call has replaced them.
     */
    updateExcludedPatterns: (excludedPatterns: string[]) => Promise<string[]>;
  }

  export const enum ActionType {
    CREATED = 0,
    DELETED = 1,
    MODIFIED = 2,
    RENAMED = 3
  }

  /**
   * What the entry of an event is: the entry itself, not what a symlink leads to. A symlink is a `FILE`, except on
   * Windows, where a symlink or a junction to a directory is a `DIRECTORY`. `FILE` stands for anything that isn't a
   * directory. `UNKNOWN` when the system can't tell, see the README.
   */
  export const enum EntryKind {
    FILE = 0,
    DIRECTORY = 1,
    UNKNOWN = 2
  }

  export type CreatedFileEvent = GenericFileEvent<ActionType.CREATED>;
  export type DeletedFileEvent = GenericFileEvent<ActionType.DELETED>;
  export type ModifiedFileEvent = GenericFileEvent<ActionType.MODIFIED>;
  export type FileChangeEvent = CreatedFileEvent | DeletedFileEvent | ModifiedFileEvent | RenamedFileEvent;

  export interface RenamedFileEvent {
    /** the type of event that occurred */
    action: ActionType.RENAMED;
    /** what the entry that moved is */
    kind: EntryKind;
    /** the directory before a rename */
    directory: string;
    /**  the name of the file before a rename*/
    oldFile: string;
    /** the destination directory of the rename */
    newDirectory: string;
    /** the name of the file after a rename */
    newFile: string;
  }

  export interface GenericFileEvent<T extends ActionType> {
    /** the type of event that occurred */
    action: T;
    /** what the entry that changed is */
    kind: EntryKind;
    /** the location the event took place */
    directory: string;
    /** the name of the file that was changed(Not available for rename events) */
    file: string;
  }

  export interface Options {
    /** time in milliseconds to debounce the event callback */
    debounceMS?: number;
    /** callback to fire in the case of errors */
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    errorCallback?: (err: any) => void;
    /**
     * absolute paths inside the watched path to be excluded; the ones outside it are ignored
     * @deprecated Use `excludedPatterns` instead: `/build` excludes `build` in the watched directory.
     */
    excludedPaths?: string[];
    /**
     * Glob patterns with `.gitignore` syntax, relative to the watched path, whose matches are excluded: `*.log`
     * matches at any depth, `/build` only at the top, `dist/` only directories, `**` spans directories and `!`
     * includes again what an earlier pattern excluded. They take precedence over the ignore files of
     * `respectGitignore`. Can be replaced with `updateExcludedPatterns()`. Only for a watched directory.
     */
    excludedPatterns?: string[];
    /**
     * Excludes what git ignores in the repository the watched path is in: its `.gitignore` files, which are read
     * again when they change, its `info/exclude` and the user's `core.excludesFile`. The watched path itself is
     * never excluded. Outside a repository it has no effect. Set at creation. Only for a watched directory.
     * Defaults to false.
     */
    respectGitignore?: boolean;
    /**
     * The addon to use instead of the one nsfw finds next to itself: the absolute path to its `.node` file, or the
     * module a `require()` of that file returned. Overrides the `NSFW_NATIVE_BINDING` environment variable. Each
     * file is loaded once per thread, and watchers created with different files are independent of each other.
     */
    nativeBinding?: string | NativeAddon;
    /**
     * Also follows, on macOS and Windows, the symlinks to files that were already below the watched path when the
     * watcher started: a change to the file a symlink leads to is reported under the symlink too, see "Symlinked
     * files" in the README. It walks the watched tree once, in the background, skipping excluded and ignored
     * directories. Without it only the symlinks that appear while watching are followed there. On Linux every one is
     * followed anyway, found by the scan the watcher already does, so it has no effect. Only for a watched directory.
     * Defaults to false.
     */
    followFileSymlinks?: boolean;
  }

  /** The watcher class that nsfw's `.node` file exports. */
  export type NativeAddon = new (...args: never[]) => NSFW;
}

// For the specs; defined as getters, which load the default addon on first use and keep them out of the declarations.
Object.defineProperties(nsfw, {
  _native: { get: loadDefaultBinding },
  getAllocatedInstanceCount: { get: () => loadDefaultBinding().getAllocatedInstanceCount }
});

export = nsfw;
