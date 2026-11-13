#!/usr/bin/env python3
"""Builds and runs the TC002 host tests that do not need Linux, with the Mac's own clang.

  tools/tc002/macos/host_tests.py [--out DIR] [NAME ...]

The CMake host build and its CTest suite need Linux (CI runs them). These suites are portable
C++: the wake word engine bit for bit against its fixtures, the multi-model listener, the model
store and its HTTP routes, and the assistant router. NAME picks some of them.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
WAKE = "src/platform/tc002/wake/"
ENGINE = [WAKE + name for name in ("WakeFft.cpp", "WakeFrontend.cpp", "WakeModel.cpp", "WakeLoader.cpp",
                                   "WakeOperators.cpp", "WakeKernels.cpp", "ModelStream.cpp",
                                   "ProbabilityWindow.cpp", "WakeDetector.cpp")]
CONTRACT = ["src/platform/tc002/contract/" + name for name in ("MicrophoneClient.cpp", "MicrophoneStream.cpp",
                                                               "Pcm16.cpp")]
LISTENER = [WAKE + "WakeListener.cpp", WAKE + "ModelFiles.cpp", *ENGINE, *CONTRACT,
            "src/core/api/JsonReader.cpp", "src/platform/posix/Files.cpp", "src/system/Log.cpp"]
MODEL = "assets/tc002/wake/hey_awtrix.tflite"
# The portable engine with Berry, as cmake/Core.cmake builds awtrix_core.
CORE = sorted(str(p.relative_to(REPO)) for p in (REPO / "src/core").rglob("*.c*")) + [
    "src/platform/linux/settings/Settings.cpp", "src/platform/linux/settings/Config.cpp",
    "src/platform/linux/render/OutputGrade.cpp", "src/platform/linux/render/PageZoom.cpp",
    "src/platform/linux/host/HostScriptHeap.cpp",
    *sorted(str(p.relative_to(REPO)) for p in (REPO / "lib/berry").glob("*/*.c")
            if p.parent.name in ("src", "default"))]
FIXTURES = "tests/tc002/runtime/wake"
# Third-party headers, outside -Werror as in the CMake build; the engine's defines.
SYSTEM = ["-isystem", "src/platform/linux/host/vendor", "-isystem", "lib/berry/src", "-isystem", "lib/berry",
          "-DAWTRIX_NATIVE", '-DAWTRIX_NG_VERSION="host"']
SUITES = {
    "tc002-wake": (["tests/tc002/runtime/test_wake.cpp", *ENGINE, WAKE + "Endpointer.cpp",
                    "src/platform/posix/Files.cpp"],
                   [FIXTURES, "hey_awtrix", MODEL, FIXTURES, "vad", "assets/tc002/vad/vad.tflite", FIXTURES]),
    "tc002-wake-listener": (["tests/tc002/runtime/test_wake_listener.cpp", *LISTENER], [MODEL]),
    "tc002-wake-models": (["tests/tc002/runtime/test_wake_models.cpp", WAKE + "WakeApi.cpp", WAKE + "WakeModelStore.cpp",
                           "src/platform/tc002/voice/HttpStream.cpp", *LISTENER, "src/platform/posix/Digest.cpp",
                           "src/platform/posix/sha256.c", "tools/tc002/macos/test_shims.cpp"], [MODEL]),
    "tc002-wake-scripting": (["tests/tc002/runtime/test_wake_scripting.cpp", WAKE + "WakeScripting.cpp",
                              "src/platform/linux/script/ExtensionHost.cpp", *CORE], []),
    "tc002-assistant-router": (["tests/tc002/runtime/test_assistant_router.cpp",
                                "src/platform/tc002/voice/AssistantRouter.cpp",
                                "src/platform/tc002/runtime/MicrophoneHub.cpp", *LISTENER], [MODEL]),
}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", metavar="NAME")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    unknown = [name for name in args.names if name not in SUITES]
    if unknown:
        parser.error(f"unknown suite {', '.join(unknown)}; known: {', '.join(SUITES)}")
    out = (args.out or Path(tempfile.mkdtemp(prefix="awtrix-host-tests-"))).absolute()
    out.mkdir(parents=True, exist_ok=True)
    failed = []
    for name in args.names or SUITES:
        sources, arguments = SUITES[name]
        binary = out / name
        # C sources and the engine are libraries here: built once, without -Werror, as CMake does.
        objects = []
        library = [s for s in sources if s.endswith(".c") or s in CORE]
        jobs = []
        for source in library:
            obj = out / (source.replace("/", "_") + ".o")
            objects.append(str(obj))
            if obj.exists() and obj.stat().st_mtime >= (REPO / source).stat().st_mtime:
                continue
            compiler = ["clang"] if source.endswith(".c") else ["clang++", "-std=c++17"]
            jobs.append(subprocess.Popen([*compiler, "-O2", "-ffp-contract=off", "-w", *SYSTEM, "-I", "src",
                                          "-I", "src/platform/linux/include", "-c", source, "-o", str(obj)], cwd=REPO))
            if len(jobs) >= (os.cpu_count() or 4):
                if jobs.pop(0).wait():
                    failed.append(name)
        if any(job.wait() for job in jobs):
            failed.append(name)
        # The engine's float paths are exact only without fused multiply-adds, as on the clock.
        # -Wall -Wextra as the CMake build has them; -Werror as CI sets it.
        build = ["clang++", "-std=c++17", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unknown-warning-option", *SYSTEM, "-I", "src", "-I", "tests",
                 "-I", "src/platform/linux/include", "-o", str(binary),
                 *(s for s in sources if s not in library), *objects]
        print(f"build {name}", flush=True)
        if subprocess.run(build, cwd=REPO).returncode:
            failed.append(name)
            continue
        result = subprocess.run([str(binary), *arguments], cwd=REPO, capture_output=True, text=True)
        print(f"{name}: {'ok' if result.returncode == 0 else 'FAILED'}", flush=True)
        if result.returncode:
            print(result.stdout + result.stderr, file=sys.stderr)
            failed.append(name)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
