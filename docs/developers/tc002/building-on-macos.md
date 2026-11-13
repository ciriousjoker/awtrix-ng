---
only: [tc002]
---

# Building on macOS

`tools/tc002/macos/build.sh` builds a TC002 release natively on an Apple Silicon Mac: the bundle
that `deploy` writes, the web update package and the USB installation package. It needs no Docker,
no virtual machine and no Rosetta. The [Linux build](install-tools.md#build-the-bundle) stays the
reference: it builds every part from source, the Mac build reuses the parts that need a Linux host.

## What it builds and what it reuses

| Part | On the Mac |
|---|---|
| `awtrix-linux`, `awtrix-tc002d`, `awtrix-tc002-audio-pcm`, `awtrix-tc002-flash`, `dhcp-callback` | built from the commit, the `tc002` CMake preset |
| OpenSSL, liblzma, libjpeg | built with their recipes in `tools/tc002/` |
| musl 1.2.6 | built with the patches of the pinned Buildroot, for Cortex-A7 |
| web UI, boot sound, wake word, voice activity model, speech voice, MCU extension | from the commit |
| `libawtrix-loader.so`, `loop.ko`, `aic8800_bsp.ko`, `aic8800_fdrv.ko`, `awtrix_pcm.ko` | reused: they need the glibc toolchain or the kernel tree |
| `udhcpc` (BusyBox), `wpa_supplicant`, `ca-certificates.crt`, `licenses.txt.gz` | reused: they never change here |

The reused files come from the official release of the base version: the version file without
its `-sc.N`. The script downloads that release's `usb-awtrix-ng-tc002.zip`, checks it against the
SHA-256 pinned in `tools/tc002/macos/pins.json`, unpacks its release image and checks every file
against the release's manifest. It stops when the commit changed what a reused file is made of: the
recipes, the loader, the kernel modules or the licence texts. Such a release needs the Linux build.

The manifest names both compilers: the musl one it used, and the glibc one of the base release,
whose loader it ships.

## Toolchain

The cross compiler is GCC 15.2 of
[messense/homebrew-macos-cross-toolchains](https://github.com/messense/homebrew-macos-cross-toolchains)
v15.2.0 (`armv7-unknown-linux-musleabihf`, aarch64-darwin), pinned by SHA-256. Its runtime libraries
target ARMv5TE, which the Cortex-A7 runs, through the kernel's helpers for atomic operations. The
script therefore:

- rebuilds musl 1.2.6 with Buildroot's patches for Cortex-A7 into a private sysroot;
- wraps the compiler as `arm-linux-gcc` and `arm-linux-g++` with that sysroot and the defaults the
  Buildroot toolchain was configured with: `-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`;
- removes the host's compiler flags (`CFLAGS`, `LDFLAGS` and the like) from the cross builds.

`libstdc++` of this toolchain keeps its reference counts behind a mutex (its lock policy is fixed
when GCC is built), so `shared_ptr` costs a little more than on the Buildroot toolchain. It behaves
the same.

## Setup

```sh
brew install cmake ninja squashfs
```

Python 3.11 or later, `git`, `perl`, `curl` and `patch` come with macOS or Xcode's command line
tools. Node.js is needed only to regenerate the web UI.

## Build

```sh
export TC002_CACHE=/path/with/space/awtrix-tc002    # default ~/.cache/awtrix-tc002
tools/tc002/macos/build.sh OUT
```

| Option | Effect |
|---|---|
| `--commit REV` | build this commit (default `HEAD`), exported with `git archive` |
| `--working-tree` | build the checkout as it is; the version ends in `-dirty` |
| `--incremental` | with `--working-tree`: keep the build directory, for quick rebuilds |
| `--jobs N` | parallel jobs (default: all cores) |
| `--no-usb` | build neither the source archive nor the USB package |
| `--tag TAG`, `--repository OWNER/NAME` | the release the USB package names for its sources (default `v<version>` of `origin`) |

`OUT` receives `bundle/`, `awtrix-ng-tc002.awup`, `tc002-sources.tar.gz` and
`usb-awtrix-ng-tc002.zip`. The first build downloads the toolchain, musl, Buildroot, the library
sources and the base release (about 250 MB) and takes about four minutes on an M2. Later builds
take about two minutes, `--incremental` well under one.

`tc002-sources.tar.gz` holds the commit, every source tarball compiled into the programs and the
musl patches. For the reused files it names the companion archive of the base release.

## Install

```sh
TMPDIR=/private/tmp python3 tools/tc002/install/tc002_install.py --serial SERIAL deploy --bundle OUT/bundle
```

`TMPDIR` must not be a symbolic link: the default `/var/folders/…` is one (`/var` points to
`/private/var`). Once the clock is on a network, the web UI also takes `OUT/awtrix-ng-tc002.awup`.

## Tests

The CMake host build needs Linux, and CI runs it. The suites that do not need Linux run with the
Mac's own clang:

```sh
python3 tools/tc002/macos/host_tests.py
```

They are the wake word engine against its fixtures, the wake word listener, the wake word models and
their routes, and the assistant router.

## A new base version

When the fork takes a new upstream release, `pins.json` needs that release:

```sh
python3 tools/tc002/macos/pin_base.py vX.Y.Z
```

It records the commit of the tag and the SHA-256 GitHub reports for its USB package and sources.
The fork's upstream sync does this itself.

## Related

- [Install tools](install-tools.md)
- [TC002 developer guide](index.md)
