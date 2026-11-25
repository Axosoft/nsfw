// Compiled with `tsc --noEmit` against the published declarations, so a change to the public types that breaks this
// usage fails the build.
import nsfw = require('nsfw');

async function watch(): Promise<void> {
  const options: Partial<nsfw.Options> = {
    debounceMS: 100,
    errorCallback: (error: Error) => { throw error; },
    excludedPaths: ['/watched/ignored'],
    excludedPatterns: ['*.log', '!keep.log'],
    respectGitignore: true
  };
  const watcher: nsfw.NSFW = await nsfw('/watched', (events: nsfw.FileChangeEvent[]) => {
    for (const event of events) {
      switch (event.action) {
        case nsfw.actions.CREATED:
        case nsfw.actions.DELETED:
        case nsfw.actions.MODIFIED: {
          const changed: nsfw.CreatedFileEvent | nsfw.DeletedFileEvent | nsfw.ModifiedFileEvent = event;
          const file: string = changed.file;
          const isDirectory: boolean = changed.kind === nsfw.kinds.DIRECTORY;
          void [file, isDirectory];
          break;
        }
        case nsfw.ActionType.RENAMED: {
          const renamed: nsfw.RenamedFileEvent = event;
          const moved: string[] = [renamed.directory, renamed.oldFile, renamed.newDirectory, renamed.newFile];
          const kind: nsfw.EntryKind = renamed.kind;
          void [moved, kind];
          break;
        }
        default: {
          const unreachable: never = event;
          void unreachable;
        }
      }
    }
  }, options);
  await nsfw('/watched', () => {});
  await nsfw('/watched', () => {}, { nativeBinding: '/app/resources/nsfw.node' });
  const addon: nsfw.NativeAddon = class {} as unknown as nsfw.NativeAddon;
  await nsfw('/watched', () => {}, { nativeBinding: addon });

  const started: Promise<void> = watcher.start();
  await started;
  await watcher.pause();
  await watcher.resume();
  const paths: string[] = await watcher.updateExcludedPaths(await watcher.getExcludedPaths());
  const patterns: string[] = await watcher.updateExcludedPatterns(await watcher.getExcludedPatterns());
  void [paths, patterns];
  const { stop } = watcher;
  await stop();

  const generic: nsfw.GenericFileEvent<nsfw.ActionType.CREATED> =
    { action: 0, kind: nsfw.EntryKind.FILE, directory: '/', file: 'a' };
  const renamedAction: nsfw.ActionType.RENAMED = nsfw.actions.RENAMED;
  void [generic, renamedAction];

  // @ts-expect-error The watched path is a string.
  await nsfw(1, () => {});
  // @ts-expect-error The event callback is required.
  await nsfw('/watched');
  // @ts-expect-error Unknown options are rejected.
  await nsfw('/watched', () => {}, { debounce: 100 });
  // @ts-expect-error nativeBinding is a path or the loaded addon.
  await nsfw('/watched', () => {}, { nativeBinding: 1 });
  // @ts-expect-error A class that does not build watchers is not the addon.
  await nsfw('/watched', () => {}, { nativeBinding: class {} });
  // @ts-expect-error A renamed event has no `file`.
  const missing: nsfw.RenamedFileEvent['file'] = '';
  // @ts-expect-error `actions` is read-only.
  nsfw.actions.CREATED = nsfw.ActionType.DELETED;
  // @ts-expect-error `kinds` is read-only.
  nsfw.kinds.FILE = nsfw.EntryKind.DIRECTORY;
  // @ts-expect-error Every event tells its kind.
  const unkinded: nsfw.DeletedFileEvent = { action: nsfw.actions.DELETED, directory: '/', file: 'a' };
  void unkinded;
  // @ts-expect-error Internals are not part of the declarations.
  void nsfw._native;
  void missing;
}

void watch;
