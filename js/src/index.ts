import { promises as fs, Stats } from 'fs';
import path = require('path');

type Implementation = nsfw.NSFW;

interface NativeBinding {
  new (watchPath: string, eventCallback: EventCallback, options: Required<nsfw.Options>): Implementation;
  getAllocatedInstanceCount?: () => number;
}

type EventCallback = (events: nsfw.FileChangeEvent[]) => void;

// A local build in build/Release wins over the prebuilt binary for this platform in prebuilds/.
// eslint-disable-next-line @typescript-eslint/no-require-imports
const NSFW: NativeBinding = require('node-gyp-build')(path.join(__dirname, '..', '..'));

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

function createFilePoller(watchPath: string, eventCallback: EventCallback, debounceMS: number,
  excludedPaths: string[], reportedPath: string, isExcluded: (value: string) => boolean,
  excludedPatterns: string[]): Implementation {
  const directory = path.dirname(reportedPath);
  const file = path.basename(reportedPath);

  let fileStatus: Stats | null = null;
  let filePollerInterval: ReturnType<typeof setInterval> | undefined;

  let currentExcludedPaths = excludedPaths;
  // Kept, though only a watched directory applies them.
  let currentExcludedPatterns = excludedPatterns;

  const report = (action: nsfw.ActionType.CREATED | nsfw.ActionType.DELETED | nsfw.ActionType.MODIFIED): void => {
    if (!currentExcludedPaths.some(isExcluded)) eventCallback([{ action, directory, file }]);
  };

  const getStatus = async (): Promise<void> => {
    try {
      const status = await fs.stat(watchPath);
      if (fileStatus === null) {
        fileStatus = status;
        report(nsfw.ActionType.CREATED);
      } else if (
        status.mtime.getTime() !== fileStatus.mtime.getTime() ||
        status.ctime.getTime() !== fileStatus.ctime.getTime()
      ) {
        fileStatus = status;
        report(nsfw.ActionType.MODIFIED);
      }
    } catch {
      if (fileStatus !== null) {
        fileStatus = null;
        report(nsfw.ActionType.DELETED);
      }
    }
  };

  const start = async (): Promise<void> => {
    try {
      fileStatus = await fs.stat(watchPath);
    } catch {
      fileStatus = null;
    }

    filePollerInterval = setInterval(getStatus, debounceMS);
  };

  const stop = async (): Promise<void> => {
    clearInterval(filePollerInterval);
  };

  return {
    start,
    stop,
    pause: stop,
    resume: start,
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
  respectGitignore = false
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

  const errorCallback = _errorCallback || ((nsfwError: unknown) => { throw nsfwError; });

  if (!path.isAbsolute(watchPath)) {
    throw new Error('Path to watch must be an absolute path.');
  }

  let context: PathContext, stats: Stats;
  try {
    context = await createPathContext(watchPath);
    // Node's metadata APIs reject extended drive roots that the native watcher can open.
    const metadataPath = isWin32 && /^\\\\\?\\[A-Za-z]:\\$/.test(context.watchPath) ?
      context.watchPath.slice(4) : context.watchPath;
    stats = await fs.stat(metadataPath);
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
        respectGitignore
      });
  } else if (stats.isFile()) {
    implementation = createFilePoller(context.watchPath, eventCallback, debounceMS, effectivePaths,
      watchPath, context.isWatchPath, [...excludedPatterns]);
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

  export type CreatedFileEvent = GenericFileEvent<ActionType.CREATED>;
  export type DeletedFileEvent = GenericFileEvent<ActionType.DELETED>;
  export type ModifiedFileEvent = GenericFileEvent<ActionType.MODIFIED>;
  export type FileChangeEvent = CreatedFileEvent | DeletedFileEvent | ModifiedFileEvent | RenamedFileEvent;

  export interface RenamedFileEvent {
    /** the type of event that occurred */
    action: ActionType.RENAMED;
    /** the directory before a rename */
    directory: string;
    /**  the name of the file before a rename*/
    oldFile: string;
    /** the new location of the file(only useful on linux) */
    newDirectory: string;
    /** the name of the file after a rename */
    newFile: string;
  }

  export interface GenericFileEvent<T extends ActionType> {
    /** the type of event that occurred */
    action: T;
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
  }
}

// For the specs; Object.assign keeps them out of the declarations.
Object.assign(nsfw, { _native: NSFW });
if (NSFW.getAllocatedInstanceCount) {
  Object.assign(nsfw, { getAllocatedInstanceCount: NSFW.getAllocatedInstanceCount });
}

export = nsfw;
