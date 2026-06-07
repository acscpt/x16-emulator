# Releasing this fork

How fork releases are versioned, branched, and published. Fork-specific; not part of upstream.

## Branch model

- **`develop`** is the integration/working branch. It stays a clean "upstream base + fork changes": `version.h` is kept byte-identical to upstream so upstream syncs never conflict on it. Features land here.
- **`master`** is the release vehicle: the only place the fork version stamp lives, and where release tags are cut. It is also the GitHub default branch, so the landing page's Releases section is populated (that section only shows releases whose tag is reachable from the default branch, and the tags live on master).
- **`upstream-master`** mirrors upstream, for syncing.

## Versioning

The fork marker lives in `VER_NAME`, not a separate define, so the existing banners pick it up with no code change:

```c
#define VER       "50"                          // upstream base, untouched
#define VER_NAME  "next-acscpt.1"               // upstream codename + fork build
#define VER_INFO  "### Release 50 (\"next-acscpt.1\")"
```

The banner then reads `Commander X16 Emulator r50 (next-acscpt.1), <git-rev>`.

- `VER` stays the pristine upstream number.
- `next` is upstream's own codename for the unreleased dev line this descends from; `acscpt.<n>` is the fork build number, bumped per release.
- This stamp is applied **on `master` only**. `develop` keeps upstream's `VER_NAME`, so develop builds report plain `r50 (next)` and are identified as the fork by their git revision.

## Cutting a release `r50-next-acscpt.<n>`

The release tag mirrors the banner: `r<VER>-<VER_NAME>`, e.g. `r50-next-acscpt.1`. It is the GitHub Release title too.

1. **On `develop`**, finalise the changelog (a fork-only file, so no upstream conflict): promote `## [Unreleased]` in `FORK-CHANGES.md` to `## [acscpt.<n>] - <YYYY-MM-DD>`, and add a fresh empty `## [Unreleased]` above it. Commit and push.

2. **On `master`**, merge `develop`, then make the release commit: set `VER_NAME` to `"next-acscpt.<n>"` (and the name inside `VER_INFO`) in `src/version.h`, commit, `git tag r50-next-acscpt.<n>`, and push `master` plus the tag.

3. **CI publishes.** `.github/workflows/release.yml` triggers on the tag (it matches `*acscpt*`), builds the platforms, and creates the GitHub Release with the binaries attached. The Linux and Windows builds gate publishing; macOS is best-effort and never blocks. It can also be re-run via *workflow_dispatch* against an existing tag.

`RELEASES.md` is upstream's per-release log: leave it untouched. Fork release notes live in `FORK-CHANGES.md`.

## What a release contains

Lean and ROM-free. Each release attaches the emulator and `makecart` per platform, nothing else:

- **Tested:** Linux x86_64, Windows x86_64 (DLLs bundled, self-contained).
- **Untested:** macOS x86_64, macOS arm64 (built by CI, not run).

Built with `-DENABLE_FLUIDSYNTH=ON -DENABLE_TRACE=OFF`.

Deliberately **not** included, and why:

- **`rom.bin`** is separately licensed and mixes proprietary (Commodore KERNAL, Microsoft BASIC) and GPLv3 components, so it is not the fork's to redistribute. The release notes link users to [x16-rom](https://github.com/X16Community/x16-rom/releases).
- **Trace** (`rom_lst.h` / `rom_labels.h`) is the ROM's assembly listing, so compiling it into a distributed binary would carry the same proprietary + GPLv3 terms. Trace stays a local build feature (`build_rom_debug.sh`); it is never shipped.
- **Docs** are linked to [x16-docs](https://github.com/X16Community/x16-docs) rather than bundled, since their licensing is unclear.
