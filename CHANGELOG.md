# Changelog

## Unreleased

Behaviour changes in excluded paths, so this needs a minor release: calls that used to succeed can now reject, and
paths that used to be forwarded to the watcher are now ignored.

### Changed
- `nsfw()` and `updateExcludedPaths()` handle excluded paths the same way:
  - Relative paths reject with `Excluded path must be an absolute path.` Before, `updateExcludedPaths()` accepted
    them, and so did `nsfw()` on Windows.
  - Absolute paths outside the watched path are ignored instead of throwing or being forwarded to the watcher, and are
    left out of what `getExcludedPaths()` and `updateExcludedPaths()` resolve with. Before, `nsfw()` rejected them on
    Linux and macOS unless they started with the watched path as a string, like a sibling sharing it as a prefix
    (`/a/foo-bar` when watching `/a/foo`) or `/a/foo/../bar`, while `nsfw()` on Windows and `updateExcludedPaths()`
    forwarded them all. A forwarded ancestor of the watched path excluded every event on macOS and Windows; it is
    now ignored too.
  - Whether a path is inside is decided lexically, and on Linux and macOS case-sensitively: a differently cased path,
    or one under the real path of a watched path that goes through a symlink (`/private/tmp/w/build` when watching
    `/tmp/w`), is ignored, also on update, where it used to be forwarded and took effect on macOS. On Windows the check
    is case-insensitive and the `\\?\` form of a path counts as the same path as its plain form.
  - Paths inside the watched path are rewritten as the watched path as it was passed, without trailing separators,
    followed by the rest with `.`, `..` and repeated separators resolved and joined with the platform separator, and
    that is what the watcher uses and what `getExcludedPaths()` and `updateExcludedPaths()` resolve with. Before, a path such as `/a/b/../w/x` when watching `/a/w` was forwarded
    as is and never matched.
- Non-array `excludedPaths`, or non-string elements, reject with a `TypeError`; `updateExcludedPaths()` now rejects
  instead of throwing synchronously.
- `updateExcludedPaths()` resolves with the paths that call set, without the ignored ones and rewritten as above;
  before it resolved with whatever the latest call had set.
- All trailing separators of excluded paths are dropped (`\` too on Windows), not just one `/`; roots are kept.
- Typings: code that silenced the old `[string]` type of `excludedPaths` with `// @ts-expect-error` now fails with
  "Unused '@ts-expect-error' directive"; drop the directive.
- `node-addon-api` is bounded to `^8.0.0` instead of `*`, and `engines.node` is `^18 || ^20 || >=21`, the versions
  `node-addon-api` 8 supports, instead of `>=10.16.0`.
- On macOS the addon is compiled as `gnu++17` with the deployment target of the Node or Electron headers it is built
  against (macOS 11.0 for Node 22, 13.5 for Node 24 and 26) instead of C++11 and macOS 10.7. Linux and Windows keep
  using the standard of the headers, C++17 or newer.

### Fixed
- A fresh `npm install nsfw` compiles again (#212). `node-addon-api` 8.9.2 headers need C++17, but the build forced
  C++11 on macOS.
- Excluding a directory no longer excludes siblings whose name starts with it (`/a/foo` excluded `/a/foobar`) on
  macOS and Windows.
- `updateExcludedPaths()` no longer races with the watcher threads that read the excluded paths on macOS, Windows and
  Linux, where it also changed the inotify watch tree while events were being handled.
- macOS no longer leaks the excluded path strings on every update and every event.
- On macOS, excluded paths work when the watched path goes through a symlink, such as `/tmp` or `os.tmpdir()`.
  FSEvents reports events under the real path, which the excluded paths, given under the watched path, never matched.
- On Linux, `updateExcludedPaths()` works on directories created after `start()`. It looked their parent up instead,
  so it kept watching the excluded directory and could stop watching an unrelated one with the same name.
- On macOS, excluding a watched `/` also excludes the events of its direct children.
- `getExcludedPaths()` and `updateExcludedPaths()` work when watching a single file instead of throwing a `TypeError`.
- Typings: `excludedPaths` is `string[]`, `getExcludedPaths()` is declared and `updateExcludedPaths()` resolves with
  `string[]`.

## 2.3.0

### Security
- Update `minimatch` 3.1.2 → 3.1.3 and 5.1.6 → 5.1.8 to fix ReDoS vulnerability
- Force `serialize-javascript` ≥ 7.0.3 via yarn resolution to fix RCE vulnerability (CVE-2020-7660)

### Breaking (development only)
- Node.js 20+ is now required for development/testing due to `serialize-javascript` v7. This does **not** affect consumers of the package at runtime.
- CI no longer tests against Node.js 18
