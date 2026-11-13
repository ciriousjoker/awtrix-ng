#!/usr/bin/env python3
"""Builds the TC002 release natively on an Apple Silicon Mac: no Docker, no VM, no Rosetta.

  tools/tc002/macos/build.py OUTPUT_DIR [--commit REV | --working-tree] [--jobs N]
                             [--version VERSION] [--no-update-check | --update-api URL --update-ota URL]
                             [--repository OWNER/NAME] [--tag TAG] [--no-usb]

OUTPUT_DIR receives bundle/ (as build_bundle.sh writes it), awtrix-ng-tc002.awup and, unless
--no-usb, tc002-sources.tar.gz and usb-awtrix-ng-tc002.zip.

What is built here from source, with the GCC 15.2 cross compiler of messense/homebrew-macos-cross-
toolchains and musl 1.2.6 (Buildroot's patches) rebuilt for Cortex-A7: OpenSSL, liblzma, libjpeg
and every program of the tc002 CMake preset (awtrix-linux, awtrix-tc002d, awtrix-tc002-audio-pcm,
awtrix-tc002-flash, dhcp-callback), the web UI and the assets of share/.

What is taken from the official release of the base version, checked against the release's own
manifest after the release ZIP matched its pinned SHA-256 (pins.json): what needs the glibc
toolchain or the Linux kernel tree, or is never changed here: the kernel modules, the loader,
loop.ko, udhcpc (BusyBox), wpa_supplicant, the CA bundle and licenses.txt.gz. The build refuses to
reuse them when their recipes or sources differ from the base commit.

Every download is checked against a pinned SHA-256 and cached in TC002_DL_DIR (default
TC002_CACHE/downloads, TC002_CACHE default ~/.cache/awtrix-tc002).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
PINS = json.loads((HERE / "pins.json").read_text())

# Files of a release that come from the base release: path -> where the base release has it.
REUSED = {
    "bin/udhcpc": "release",
    "bin/wpa_supplicant": "release",
    "lib/modules/aic8800_bsp.ko": "release",
    "lib/modules/aic8800_fdrv.ko": "release",
    "lib/modules/awtrix_pcm.ko": "release",
    "share/ca-certificates.crt": "release",
    "share/licenses.txt.gz": "release",
    "lib/libawtrix-loader.so": "host",
    "lib/modules/loop.ko": "host",
}
# What the reused files are made from. A build whose commit changes any of these from the base
# commit would ship binaries that no longer match their source, so it stops.
REUSED_SOURCES = (
    "tools/tc002/busybox", "tools/tc002/wpa_supplicant", "tools/tc002/kernel", "tools/tc002/ca",
    "tools/tc002/licenses", "src/platform/tc002/kmod", "src/platform/tc002/loader",
    "src/platform/tc002/flasher/flash_write.c", "src/platform/tc002/flasher/slot_io.c",
    "src/platform/tc002/flasher/mtd_table.c", "src/platform/tc002/contract/release_slot.c",
    "src/platform/tc002/contract/release_slot.h", "src/platform/posix/sha256.c",
    "cmake/Tc002Loader.cmake", "cmake/Tc002LoaderCheck.cmake", "cmake/Tc002Flash.cmake",
    "cmake/toolchains/tc002-glibc.cmake", "LICENSE.md", "LICENSES", "assets/tc002/vad/NOTICE",
    "tools/tc002/bluetooth/NOTICE", "tools/tc002/mcu/release/LICENSE.txt",
)
# licenses.txt.gz names the versions of the libraries built here: they must stay those of the base.
VERSIONED_RECIPES = ("tools/tc002/openssl/build.sh", "tools/tc002/lzma/build.sh", "tools/tc002/jpeg/build.sh")
ARCH_FLAGS = "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm"
# What the Buildroot toolchain was configured with (--with-cpu, --with-fpu, --with-float). The
# recipes rely on these defaults (OpenSSL adds -mthumb, which on the toolchain's own ARMv5TE
# default would be Thumb-1, which has no hard-float ABI), so the wrappers pass them first.
DEFAULT_ARCH = "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
MUSL_PATCHES_DIR = "package/musl"
TOOLS = ("ar", "as", "ld", "nm", "objcopy", "objdump", "ranlib", "readelf", "strip", "size", "strings",
         "gcc-ar", "gcc-nm", "gcc-ranlib", "c++filt", "addr2line")
COMPILERS = ("gcc", "g++", "cpp", "c++", "cc")
SCRIPT_KEY = "macos-sysroot-4"


class BuildError(Exception):
    pass


def log(message):
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def run(command, cwd=None, env=None, quiet=False, log_file=None):
    if log_file:
        with open(log_file, "ab") as stream:
            result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            tail = Path(log_file).read_bytes()[-4000:].decode("utf-8", "replace")
            raise BuildError(f"{' '.join(map(str, command))} failed, see {log_file}:\n{tail}")
        return ""
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=quiet, text=True)
    if result.returncode:
        detail = (result.stdout or "") + (result.stderr or "") if quiet else ""
        raise BuildError(f"{' '.join(map(str, command))} failed ({result.returncode}) {detail[-4000:]}")
    return result.stdout if quiet else ""


def sha256(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch(dl_dir, name, url, digest):
    """TC002_DL_DIR/name once it has the pinned SHA-256, downloaded from url when missing
    (the same rule as tools/tc002/lib/fetch.sh)."""
    path = Path(dl_dir) / name
    if path.exists():
        if sha256(path) != digest:
            raise BuildError(f"{path} does not have the pinned SHA-256 {digest}; remove it to download it again")
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + ".part")
    log(f"download {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "awtrix-tc002-build"})
    with urllib.request.urlopen(request, timeout=600) as response, open(partial, "wb") as stream:
        shutil.copyfileobj(response, stream, 1 << 20)
    if sha256(partial) != digest:
        partial.unlink()
        raise BuildError(f"{url} does not have the pinned SHA-256 {digest}")
    partial.rename(path)
    return path


def shell_value(path, name):
    """NAME=value from a recipe, with $OTHER references of the same file expanded."""
    text = Path(path).read_text()
    values = dict(re.findall(r"^([A-Z0-9_]+)=([^\s#]*)", text, re.M))
    value = values[name]
    for _ in range(5):
        value = re.sub(r"\$\{?([A-Z0-9_]+)\}?", lambda m: values.get(m[1], m[0]), value)
    return value


def need_tools():
    missing = [tool for tool in ("cmake", "ninja", "mksquashfs", "unsquashfs", "git", "perl", "patch")
               if not shutil.which(tool)]
    if missing:
        raise BuildError(f"missing {', '.join(missing)}; install them, for example: brew install "
                         + " ".join("squashfs" if t in ("mksquashfs", "unsquashfs") else t for t in missing))
    if sys.version_info < (3, 11):
        raise BuildError("needs Python 3.11 or later")


def make_program():
    return shutil.which("gmake") or "make"


def jobs_default():
    return os.cpu_count() or 4


# --- toolchain ----------------------------------------------------------------------------------

def base_toolchain(cache, dl_dir):
    """The prefix of the GCC 15.2 cross compiler, extracted once into TC002_CACHE/toolchains."""
    pin = PINS["toolchain"]
    root = cache / "toolchains" / "gcc15-musl"
    prefix = root / pin["prefix"]
    marker = root / ".sha256"
    if Path(str(prefix) + "gcc").exists() and marker.exists() and marker.read_text().strip() == pin["sha256"]:
        return prefix
    if Path(str(prefix) + "gcc").exists() and not marker.exists():
        # Extracted by hand: accepted when the tarball beside it is the pinned one.
        fetch(dl_dir, pin["file"], pin["url"], pin["sha256"])
        marker.write_text(pin["sha256"] + "\n")
        return prefix
    tarball = fetch(dl_dir, pin["file"], pin["url"], pin["sha256"])
    log(f"extract the cross compiler into {root}")
    partial = root.with_name(root.name + ".partial")
    shutil.rmtree(partial, ignore_errors=True)
    partial.mkdir(parents=True)
    run(["tar", "-xzf", str(tarball), "-C", str(partial)])
    shutil.rmtree(root, ignore_errors=True)
    partial.rename(root)
    marker.write_text(pin["sha256"] + "\n")
    return prefix


def musl_sources(src, dl_dir):
    recipe = src / "tools/tc002/install/build_bundle.sh"
    version = shell_value(recipe, "MUSL_VERSION")
    musl = fetch(dl_dir, f"musl-{version}.tar.gz", shell_value(recipe, "MUSL_URL"),
                 shell_value(recipe, "MUSL_SHA256"))
    lock = json.loads((src / "tools/system/sources.lock.json").read_text())["buildroot"]
    buildroot = fetch(dl_dir, Path(lock["url"]).name, lock["url"], lock["sha256"])
    return version, musl, buildroot, lock["version"]


def musl_toolchain(cache, dl_dir, src, jobs):
    """Wrappers TC002_CACHE/toolchains/tc002-musl-KEY/bin/arm-linux-* around the GCC 15.2 cross
    compiler, whose sysroot holds musl (the version and Buildroot patches of the upstream
    toolchain) built for Cortex-A7, and the Linux headers of the toolchain."""
    real = base_toolchain(cache, dl_dir)
    version, musl, buildroot, br_version = musl_sources(src, dl_dir)
    with tarfile.open(buildroot) as archive:
        members = sorted((m for m in archive.getmembers()
                          if m.name.startswith(f"buildroot-{br_version}/{MUSL_PATCHES_DIR}/")
                          and m.name.endswith(".patch") and m.isfile()), key=lambda m: m.name)
        patches = [(Path(m.name).name, archive.extractfile(m).read()) for m in members]
    key_source = hashlib.sha256()
    for part in (SCRIPT_KEY, PINS["toolchain"]["sha256"], sha256(musl), ARCH_FLAGS, DEFAULT_ARCH):
        key_source.update(part.encode() + b"\n")
    for name, data in patches:
        key_source.update(name.encode() + b"\n" + data)
    key = key_source.hexdigest()[:12]
    root = cache / "toolchains" / f"tc002-musl-{key}"
    prefix = root / "bin" / "arm-linux-"
    if (root / ".done").exists():
        return prefix, real
    log(f"build musl {version} for Cortex-A7 into {root}")
    partial = root.with_name(root.name + ".partial")
    shutil.rmtree(partial, ignore_errors=True)
    work = partial / "work"
    work.mkdir(parents=True)
    run(["tar", "-xzf", str(musl), "-C", str(work)])
    source = work / f"musl-{version}"
    for name, data in patches:
        result = subprocess.run(["patch", "-p1", "-s", "--no-backup-if-mismatch"], cwd=source, input=data,
                                capture_output=True)
        if result.returncode:
            raise BuildError(f"musl patch {name} does not apply: {result.stdout.decode()}{result.stderr.decode()}")
    sysroot = partial / "sysroot"
    real_sysroot = Path(run([str(real) + "gcc", "-print-sysroot"], quiet=True).strip())
    shutil.copytree(real_sysroot / "usr/include", sysroot / "usr/include", symlinks=True)
    env = dict(clean_environment(), CC=str(real) + "gcc", AR=str(real) + "ar", RANLIB=str(real) + "ranlib",
               CFLAGS=f"{ARCH_FLAGS} -Os -g0 -fno-lto")
    logfile = work / "musl.log"
    run(["./configure", "--target=armv7-linux-musleabihf", "--host=armv7-linux-musleabihf", "--prefix=/usr",
         "--libdir=/usr/lib", "--syslibdir=/lib", "--disable-gcc-wrapper", "--enable-static", "--enable-shared"],
        cwd=source, env=env, log_file=logfile)
    run([make_program(), f"-j{jobs}"], cwd=source, env=env, log_file=logfile)
    run([make_program(), f"DESTDIR={sysroot}", "install-libs", "install-headers"], cwd=source, env=env,
        log_file=logfile)
    (sysroot / "MUSL-VERSION").write_text(f"musl {version} with the patches of Buildroot {br_version}, "
                                          f"CFLAGS {ARCH_FLAGS} -Os\n")
    bindir = partial / "bin"
    bindir.mkdir()
    final_sysroot = root / "sysroot"
    for tool in COMPILERS + TOOLS:
        target = Path(str(real) + tool)
        if not target.exists():
            continue
        wrapper = bindir / f"arm-linux-{tool}"
        extra = f" --sysroot='{final_sysroot}' {DEFAULT_ARCH}" if tool in COMPILERS else ""
        wrapper.write_text(f"#!/bin/sh\nexec '{target}'{extra} \"$@\"\n")
        wrapper.chmod(0o755)
    shutil.rmtree(work)
    shutil.rmtree(root, ignore_errors=True)
    partial.rename(root)
    (root / ".done").write_text(key + "\n")
    return prefix, real


# --- base release -------------------------------------------------------------------------------

def base_release(cache, dl_dir, base_version):
    """The verified official release of base_version: (directory of the installed release,
    directory of the ZIP's bundle, its manifest, the pin)."""
    pin = PINS["base"].get(base_version)
    if not pin:
        raise BuildError(f"tools/tc002/macos/pins.json pins no base release for {base_version}; add the "
                         "SHA-256 of its usb-awtrix-ng-tc002.zip and tc002-sources.tar.gz")
    url = f"https://github.com/{pin['repository']}/releases/download/{pin['tag']}/{pin['usb']['file']}"
    archive = fetch(dl_dir, f"{pin['tag']}-{pin['usb']['file']}", url, pin["usb"]["sha256"])
    root = cache / "base" / pin["tag"]
    done = root / ".sha256"
    if not (done.exists() and done.read_text().strip() == pin["usb"]["sha256"]):
        log(f"unpack the base release {pin['tag']}")
        partial = root.with_name(root.name + ".partial")
        shutil.rmtree(partial, ignore_errors=True)
        partial.mkdir(parents=True)
        with zipfile.ZipFile(archive) as zipped:
            for info in zipped.infolist():
                target = (partial / "zip" / info.filename).resolve()
                if not str(target).startswith(str((partial / "zip").resolve()) + os.sep):
                    raise BuildError(f"unsafe path {info.filename} in {archive}")
                if info.is_dir():
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(zipped.read(info))
        run(["unsquashfs", "-no-xattrs", "-q", "-d", str(partial / "release"),
             str(partial / "zip/bundle/release.img")], quiet=True)
        shutil.rmtree(root, ignore_errors=True)
        partial.rename(root)
        done.write_text(pin["usb"]["sha256"] + "\n")
    manifest = json.loads((root / "zip/bundle/manifest.json").read_text())
    image = root / "zip/bundle/release.img"
    if sha256(image) != manifest["image"]["sha256"]:
        raise BuildError(f"{image} differs from its manifest")
    released = json.loads((root / "release/manifest.json").read_text())
    if released.get("release") != manifest["release"]:
        raise BuildError("the base release image carries another release")
    for entry in manifest["files"]:
        if sha256(root / "release" / entry["path"]) != entry["sha256"]:
            raise BuildError(f"base release file {entry['path']} differs from its manifest")
    for entry in manifest["hostOnly"]:
        if sha256(root / "zip/bundle" / entry["path"]) != entry["sha256"]:
            raise BuildError(f"base release file {entry['path']} differs from its manifest")
    return root, manifest, pin


def check_reuse(src, commit_rev, base_commit, working_tree):
    """Stops when the build changes what a reused binary of the base release was made from."""
    if subprocess.run(["git", "-C", str(REPO), "cat-file", "-e", f"{base_commit}^{{commit}}"],
                      capture_output=True).returncode:
        raise BuildError(f"the base commit {base_commit} is not in this repository; fetch the upstream tags")
    command = ["git", "-C", str(REPO), "diff", "--name-only", base_commit]
    if not working_tree:
        command.append(commit_rev)
    changed = run(command + ["--", *REUSED_SOURCES], quiet=True).split()
    for recipe in VERSIONED_RECIPES:
        base = run(["git", "-C", str(REPO), "show", f"{base_commit}:{recipe}"], quiet=True)
        if re.search(r"^VERSION=.*$", base, re.M)[0] != re.search(r"^VERSION=.*$", (src / recipe).read_text(), re.M)[0]:
            changed.append(recipe)
    if changed:
        raise BuildError("these files differ from the base release, whose binaries this build reuses: "
                         + ", ".join(changed) + "; build on Linux with tools/tc002/install/build_bundle.sh")


# Flags of the host's own compiler (Homebrew's LDFLAGS and the like) must not reach the cross builds.
HOST_FLAGS = ("SOURCE_DATE_EPOCH", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBS", "CPATH", "C_INCLUDE_PATH",
              "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "SDKROOT",
              "MACOSX_DEPLOYMENT_TARGET", "CC", "CXX", "AR", "RANLIB", "LD", "CMAKE_TOOLCHAIN_FILE")


def clean_environment():
    return {key: value for key, value in os.environ.items() if key not in HOST_FLAGS}


# --- build --------------------------------------------------------------------------------------

def export_source(work, rev, working_tree, incremental=False):
    src = work / "src"
    if not (working_tree and incremental):
        shutil.rmtree(src, ignore_errors=True)
    src.mkdir(parents=True, exist_ok=True)
    dirty = False
    commit = run(["git", "-C", str(REPO), "rev-parse", "--short=12", f"{rev}^{{commit}}"], quiet=True).strip()
    counter = int(run(["git", "-C", str(REPO), "log", "-1", "--format=%ct", rev], quiet=True))
    if working_tree:
        run(["rsync", "-a", "--delete", "--exclude", ".git", "--exclude", ".pio", "--exclude", "site",
             "--exclude", "node_modules", "--exclude", "/build*", f"{REPO}/", f"{src}/"])
        dirty = bool(run(["git", "-C", str(REPO), "status", "--porcelain"], quiet=True).strip())
    else:
        archive = subprocess.Popen(["git", "-C", str(REPO), "-c", "core.autocrlf=false", "archive",
                                    "--format=tar", rev], stdout=subprocess.PIPE)
        extract = subprocess.run(["tar", "-x", "-C", str(src)], stdin=archive.stdout)
        archive.stdout.close()
        if archive.wait() or extract.returncode:
            raise BuildError(f"git archive {rev} failed")
    return src, commit, counter, dirty


def build_libraries(src, work, env, jobs):
    outputs = {}
    for name in ("openssl", "lzma", "jpeg"):
        log(f"build {name}")
        out = work / name / "out"
        # The lzma and jpeg recipes keep their CMake build directory, which remembers the compiler.
        shutil.rmtree(work / name / "build", ignore_errors=True)
        run(["bash", str(src / f"tools/tc002/{name}/build.sh"), str(work / name), str(out)], env=env,
            log_file=work / f"{name}.log")
        outputs[name] = out
    return outputs


def build_programs(src, work, env, jobs, version, libs, deps, incremental=False, options=()):
    build = work / "build-arm"
    if not incremental:
        shutil.rmtree(build, ignore_errors=True)
    openssl, lzma, jpeg = libs["openssl"], libs["lzma"], libs["jpeg"]
    log("configure the tc002 preset")
    run(["cmake", "--preset", "tc002", "-B", str(build), "-DBUILD_TESTING=OFF", f"-DAWTRIX_DEPS={deps}",
         f"-DAWTRIX_RELEASE_VERSION={version}", f"-DOPENSSL_INCLUDE_DIR={openssl}/include",
         f"-DOPENSSL_CRYPTO_LIBRARY={openssl}/lib/libcrypto.a", f"-DOPENSSL_SSL_LIBRARY={openssl}/lib/libssl.a",
         f"-DLIBLZMA_INCLUDE_DIR={lzma}/include", f"-DLIBLZMA_LIBRARY={lzma}/lib/liblzma.a",
         f"-DJPEG_INCLUDE_DIR={jpeg}/include", f"-DJPEG_LIBRARY={jpeg}/lib/libjpeg.a", *options],
        cwd=src, env=env, log_file=work / "cmake.log")
    log(f"build the programs with {jobs} jobs")
    run(["cmake", "--build", str(build), "--parallel", str(jobs)], env=env, log_file=work / "cmake.log")
    commands = run(["ninja", "-C", str(build), "-t", "commands", "awtrix-linux"], quiet=True)
    if f"{openssl}/lib/libcrypto.a" not in commands.strip().splitlines()[-1]:
        raise BuildError(f"awtrix-linux did not link {openssl}/lib/libcrypto.a")
    return build


def check_binaries(bundle, prefix):
    for binary in sorted((bundle / "bin").iterdir()):
        run([f"{prefix}strip", str(binary)])
        header = run([f"{prefix}readelf", "-h", "-l", "-A", str(binary)], quiet=True)
        if (not re.search(r"Machine:\s*ARM", header) or "Tag_ABI_VFP_args: VFP registers" not in header
                or "INTERP" in header):
            raise BuildError(f"{binary} is not a static ARM hard-float executable")


def assemble(src, work, build, bundle, base_root, base_manifest, env):
    shutil.rmtree(bundle, ignore_errors=True)
    run(["cmake", "--install", str(build), "--component", "tc002", "--prefix", str(bundle), "--strip"],
        env=env, quiet=True)
    entries = {e["path"]: e for e in base_manifest["files"] + base_manifest["hostOnly"]}
    for path, where in REUSED.items():
        source = base_root / ("release" if where == "release" else "zip/bundle") / path
        target = bundle / path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(int(entries[path]["mode"], 8))
        if sha256(target) != entries[path]["sha256"]:
            raise BuildError(f"{path} changed while copying")
    run([sys.executable, str(src / "tools/tc002/mcu/bundle.py"), str(work / "mcu-patch"),
         str(bundle / "share/mcu")], quiet=True)


def base_compiler_args(base_manifest, work):
    """bundle.py takes compilers as NAME=GCC; the loader and the modules come from the base release,
    so their compiler is the one its manifest names."""
    record = base_manifest.get("compilers", {}).get("glibc")
    if not record:
        return []
    path = work / "glibc-compiler.json"
    path.write_text(json.dumps(record))
    return [f"--compiler-record=glibc={path}"]


def source_companion(out, src, commit_full, dl_dir, base_pin, toolchain_pin, musl_tarball, buildroot_tarball,
                     repository, tag):
    """tc002-sources.tar.gz for a release built here: this commit, every tarball compiled into it,
    and where the corresponding sources of the reused base binaries are."""
    from_bundle = src / "tools/tc002/install/build_bundle.sh"
    tarballs = [musl_tarball, buildroot_tarball]
    for recipe in ("openssl", "lzma", "jpeg"):
        text = (src / f"tools/tc002/{recipe}/build.sh").read_text()
        version = shell_value(src / f"tools/tc002/{recipe}/build.sh", "VERSION")
        url = re.search(r'^URL="?([^"\n]+)"?', text, re.M)[1].replace("${VERSION}", version).replace("$VERSION", version)
        tarballs.append(fetch(dl_dir, Path(url).name, url, shell_value(src / f"tools/tc002/{recipe}/build.sh", "SHA256")))
    base64 = shell_value(from_bundle, "BASE64_COMMIT")
    tarballs.append(fetch(dl_dir, f"base64_arduino-{base64}.tar.gz", shell_value(from_bundle, "BASE64_URL"),
                          shell_value(from_bundle, "BASE64_SHA256")))
    base_sources = (f"https://github.com/{base_pin['repository']}/releases/download/{base_pin['tag']}/"
                    f"{base_pin['sources']['file']}")
    readme = f"""Corresponding sources of AWTRIX NG {tag} for the Ulanzi TC002 ({repository})

awtrix-ng-{commit_full}.tar.gz   the application commit this release was built from
musl, OpenSSL, xz, libjpeg-turbo, base64_arduino tarballs, and the musl patches of the
Buildroot tarball: everything compiled into the programs of bin/ besides the GCC runtime

The programs of bin/ except udhcpc and wpa_supplicant were built on macOS with
tools/tc002/macos/build.py and the GCC {toolchain_pin['description']}.

These files of the release are byte for byte those of AWTRIX NG {base_pin['tag']} and were not
rebuilt: {', '.join(sorted(REUSED))}.
Their corresponding sources (BusyBox, wpa_supplicant, libnl, the Linux kernel tree, the AIC Wi-Fi
driver, awtrix_pcm, loop) are in the companion archive of that release:
{base_sources}
SHA-256 {base_pin['sources']['sha256']}
"""
    out = Path(out)
    with tempfile.TemporaryDirectory(prefix=".tc002-sources-", dir=out.parent) as temporary:
        stage = Path(temporary)
        archive = stage / f"awtrix-ng-{commit_full}.tar.gz"
        with open(archive, "wb") as stream:
            git = subprocess.Popen(["git", "-c", "tar.umask=0022", "-C", str(REPO), "archive", "--format=tar.gz",
                                    f"--prefix=awtrix-ng/", commit_full], stdout=stream)
            if git.wait():
                raise BuildError("git archive failed")
        for tarball in tarballs:
            shutil.copyfile(tarball, stage / Path(tarball).name)
        (stage / "README.txt").write_text(readme)
        files = {p.name: {"sha256": sha256(p), "bytes": p.stat().st_size} for p in sorted(stage.iterdir())}
        (stage / "manifest.json").write_text(json.dumps({"schema_version": 1, "application_commit": commit_full,
                                                          "base_sources": {"url": base_sources,
                                                                           "sha256": base_pin["sources"]["sha256"]},
                                                          "files": files}, indent=2) + "\n")
        with tarfile.open(out, "w:gz", format=tarfile.PAX_FORMAT) as tar:
            for path in sorted(stage.iterdir()):
                info = tar.gettarinfo(str(path), "tc002-sources/" + path.name)
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = 0
                info.mode = 0o644
                with open(path, "rb") as stream:
                    tar.addfile(info, stream)
    return out


def origin_repository():
    url = run(["git", "-C", str(REPO), "remote", "get-url", "origin"], quiet=True).strip()
    match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$", url)
    return match[1] if match else "Blueforcer/awtrix-ng"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path)
    parser.add_argument("--commit", default="HEAD")
    parser.add_argument("--working-tree", action="store_true")
    parser.add_argument("--jobs", type=int, default=jobs_default())
    parser.add_argument("--work", type=Path)
    parser.add_argument("--repository", help="OWNER/NAME of the GitHub release (default: origin)")
    parser.add_argument("--tag", help="the release tag the USB package names (default: v<version>)")
    parser.add_argument("--no-usb", action="store_true", help="build no source archive and no USB package")
    parser.add_argument("--incremental", action="store_true",
                        help="with --working-tree: keep the build directory between runs (development only)")
    parser.add_argument("--version", default=os.environ.get("AWTRIX_VERSION", ""),
                        help="the version the clock reports instead of the version file, for example "
                             "1.2.3+product-v1.0.0 (default: AWTRIX_VERSION)")
    parser.add_argument("--no-update-check", action="store_true", help="the web UI offers no update check")
    parser.add_argument("--update-api", help="release feed of the web UI's update check")
    parser.add_argument("--update-ota", help="OTA index of the web UI's update check")
    args = parser.parse_args(argv)
    if args.working_tree and args.commit != "HEAD":
        parser.error("--commit and --working-tree exclude each other")
    if args.incremental and not args.working_tree:
        parser.error("--incremental needs --working-tree")
    started = time.monotonic()
    try:
        if sys.platform != "darwin":
            log("warning: this script is meant for macOS; Linux builds use tools/tc002/install/build_bundle.sh")
        need_tools()
        cache = Path(os.environ.get("TC002_CACHE") or Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
                     / "awtrix-tc002").absolute()
        dl_dir = Path(os.environ.get("TC002_DL_DIR") or cache / "downloads").absolute()
        work = (args.work or cache / "macos").absolute()
        work.mkdir(parents=True, exist_ok=True)
        output = args.output.absolute()
        output.mkdir(parents=True, exist_ok=True)
        for log_name in ("openssl.log", "lzma.log", "jpeg.log", "cmake.log"):
            (work / log_name).unlink(missing_ok=True)

        src, commit, counter, dirty = export_source(work, args.commit, args.working_tree, args.incremental)
        commit_full = run(["git", "-C", str(REPO), "rev-parse", f"{args.commit}^{{commit}}"], quiet=True).strip()
        base_version = (src / "version").read_text().strip()
        reported = args.version.strip() or base_version
        version = f"{reported}-g{commit}{'-dirty' if dirty else ''}"
        options = []
        if args.version.strip():
            options.append(f"-DAWTRIX_VERSION_OVERRIDE={reported}")
        if args.no_update_check:
            options.append("-DAWTRIX_WEBUI_UPDATE_CHECK=OFF")
        if args.update_api:
            options.append(f"-DAWTRIX_WEBUI_UPDATE_API={args.update_api}")
        if args.update_ota:
            options.append(f"-DAWTRIX_WEBUI_UPDATE_OTA={args.update_ota}")
        log(f"building {version} (counter {counter}) on base {base_version}")

        prefix, real = musl_toolchain(cache, dl_dir, src, args.jobs)
        # No SOURCE_DATE_EPOCH: mksquashfs refuses it next to the -mkfs-time bundle.py passes.
        env = dict(clean_environment(), TC002_MUSL_PREFIX=str(prefix), TC002_CACHE=str(cache), TC002_DL_DIR=str(dl_dir))
        if shutil.which("gmake"):
            env["MAKE"] = shutil.which("gmake")

        base_root, base_manifest, base_pin = base_release(cache, dl_dir, base_version)
        check_reuse(src, args.commit, base_pin["commit"], args.working_tree)
        run([sys.executable, str(src / "tools/tc002/mcu/bundle.py"), str(src / "tools/tc002/mcu/release"),
             str(work / "mcu-patch")], quiet=True)

        libs = build_libraries(src, work, env, args.jobs)
        deps = work / "deps"
        shutil.rmtree(deps, ignore_errors=True)
        (deps / "base64/src").mkdir(parents=True)
        recipe = src / "tools/tc002/install/build_bundle.sh"
        b64 = shell_value(recipe, "BASE64_COMMIT")
        tarball = fetch(dl_dir, f"base64_arduino-{b64}.tar.gz", shell_value(recipe, "BASE64_URL"),
                        shell_value(recipe, "BASE64_SHA256"))
        with tarfile.open(tarball) as archive:
            data = archive.extractfile(f"base64_arduino-{b64}/src/base64.hpp").read()
        (deps / "base64/src/base64.hpp").write_bytes(data)

        build = build_programs(src, work, env, args.jobs, version, libs, deps, args.incremental, options)
        bundle = output / "bundle"
        assemble(src, work, build, bundle, base_root, base_manifest, env)
        check_binaries(bundle, prefix)
        log("write the manifest and the release image")
        run([sys.executable, str(src / "tools/tc002/install/bundle.py"), "manifest", str(bundle), "--version", version,
             "--commit", commit, "--counter", str(counter), *(["--dirty"] if dirty else []),
             f"--compiler=musl={prefix}gcc", *base_compiler_args(base_manifest, work)], env=env)
        run([sys.executable, str(src / "tools/tc002/install/bundle.py"), "image", str(bundle)], env=env)

        awup = output / "awtrix-ng-tc002.awup"
        awup.unlink(missing_ok=True)
        run([sys.executable, str(src / "tools/tc002/install/tc002_install.py"), "package", "--bundle", str(bundle),
             "--out", str(awup), *(["--allow-dirty"] if dirty else [])], env=env)
        if not args.no_usb:
            repository = args.repository or os.environ.get("GITHUB_REPOSITORY") or origin_repository()
            tag = args.tag or f"v{reported}"
            sources = output / "tc002-sources.tar.gz"
            sources.unlink(missing_ok=True)
            log("write tc002-sources.tar.gz")
            _, musl_tarball, buildroot_tarball, _ = musl_sources(src, dl_dir)
            source_companion(sources, src, commit_full, dl_dir, base_pin, PINS["toolchain"], musl_tarball,
                             buildroot_tarball, repository, tag)
            usb = output / "usb-awtrix-ng-tc002.zip"
            usb.unlink(missing_ok=True)
            run([sys.executable, str(src / "tools/tc002/install/browser_package.py"), str(bundle), str(usb),
                 "--license", str(src / "LICENSE.md"), "--notices", str(src / "THIRD-PARTY-NOTICES.md"),
                 "--source-url", f"https://github.com/{repository}/releases/download/{tag}/tc002-sources.tar.gz",
                 "--source-sha256", sha256(sources), *(["--allow-dirty"] if dirty else [])], env=env)
        for path in sorted(output.iterdir()):
            if path.is_file():
                log(f"{path.name}: {path.stat().st_size} bytes, SHA-256 {sha256(path)}")
        log(f"done in {int(time.monotonic() - started)} s")
        return 0
    except (BuildError, OSError, subprocess.SubprocessError, KeyError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
