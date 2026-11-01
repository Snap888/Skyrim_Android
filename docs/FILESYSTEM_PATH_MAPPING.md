# Skyrim Windows-to-Android path mapping

The runtime presents the game to the recompiled Windows executable as
`C:\\Skyrim`. On Android, `g_game_dir` is the folder selected in the launcher
(the directory containing `TESV.exe` and `Data/`).

For the installation reported during testing:

- Windows game root: `C:\\Skyrim\\`
- Android game root: `/storage/emulated/0/Download/Flud/The Elder Scrolls V - Skyrim/`
- Windows voice path: `C:\\Skyrim\\DATA\\SOUND\\Voice\\dawnguard.esm\\maleoldgrumpy\\...`
- Expected Android path:
  `/storage/emulated/0/Download/Flud/The Elder Scrolls V - Skyrim/Data/Sound/Voice/dawnguard.esm/maleoldgrumpy/...`

The mapping is a root substitution. The Windows `DATA` component is retained
as the relative path below the game root; it is not added a second time or
removed. Android's actual component spelling is resolved case-insensitively
against directory entries, so `DATA/SOUND` can resolve to `Data/Sound`.

## Runtime implementation

- `GAME_WIN_PREFIX` is `\\skyrim\\` in `runtime/game.h`.
- `runtime/kernel32.c:rt_host_path` removes the Windows drive prefix,
  matches the game-root prefix case-insensitively, appends the remaining
  relative path to `g_game_dir`, and calls `resolve_ci`.
- `resolve_ci` resolves each existing path component using the real
  on-disk spelling. It does not rewrite the game-relative path by inserting
  or deleting a `Data` directory.
- `runtime/main.c` receives the selected game directory as `argv[1]` from
  the Android launcher and stores it in `g_game_dir`.

The mapper now accepts both the exact game root (`C:\\Skyrim`) and paths
under it (`C:\\Skyrim\\...`), while rejecting sibling prefixes such as
`C:\\SkyrimSE`.

## Diagnostic logging

For the first 32 paths containing `\\Sound\\Voice\\`, the runtime logs:

```
VOICE PATH MAP #N: win=<Windows path> game_root=<selected game folder> host=<resolved Android path> exists=<0|1>
```

Use these fields to distinguish:
1. an incorrect game folder supplied by the launcher (`game_root`);
2. an incorrect drive-prefix substitution (`host` does not start below the game root);
3. a missing loose file or a case/path resolution miss (`exists=0`).

A missing loose voice file does not by itself prove that the BSA archive is
missing or unreadable. BSA archive opening is a separate operation; check its
own `CreateFileA`/BSA logs before concluding that archive fallback failed.

## Test procedure

1. Select the directory containing `TESV.exe` and `Data/` in the launcher.
2. Start the game and reproduce a voice lookup.
3. Collect `port/log.txt` and verify that a Windows path below
   `C:\\Skyrim\\DATA\\SOUND\\Voice\\` maps below the selected Android
   game root exactly once.
4. Verify the resolved path uses the real Android directory names
   `Data/Sound/Voice`.

This diagnostic does not claim that a particular loose FUZ file must exist;
some assets may be packaged in BSA archives.
