#!/usr/bin/env python3
"""Builds and runs the TC002 host tests that do not need Linux, with the Mac's own clang.

  tools/tc002/macos/host_tests.py [--out DIR] [NAME ...]

The CMake host build and its CTest suite need Linux (CI runs them). These suites are portable
C++: the wake word engine bit for bit against its fixtures, the multi-model listener, the model
store and its HTTP routes, and the assistant router. NAME picks some of them.
"""
import argparse
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
FIXTURES = "tests/tc002/runtime/wake"
SUITES = {
    "tc002-wake": (["tests/tc002/runtime/test_wake.cpp", *ENGINE, WAKE + "Endpointer.cpp",
                    "src/platform/posix/Files.cpp"],
                   [FIXTURES, "hey_awtrix", MODEL, FIXTURES, "vad", "assets/tc002/vad/vad.tflite", FIXTURES]),
    "tc002-wake-listener": (["tests/tc002/runtime/test_wake_listener.cpp", *LISTENER], [MODEL]),
    "tc002-wake-models": (["tests/tc002/runtime/test_wake_models.cpp", WAKE + "WakeApi.cpp", WAKE + "WakeModelStore.cpp",
                           "src/platform/tc002/voice/HttpStream.cpp", *LISTENER, "src/platform/posix/Digest.cpp",
                           "src/platform/posix/sha256.c", "tools/tc002/macos/test_shims.cpp"], [MODEL]),
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
        objects = []
        for source in (s for s in sources if s.endswith(".c")):
            obj = out / (Path(source).stem + ".o")
            subprocess.run(["clang", "-O2", "-w", "-I", "src", "-c", source, "-o", str(obj)], cwd=REPO, check=True)
            objects.append(str(obj))
        # The engine's float paths are exact only without fused multiply-adds, as on the clock.
        build = ["clang++", "-std=c++17", "-O2", "-ffp-contract=off", "-w", "-I", "src", "-I", "tests",
                 "-I", "src/platform/linux/include", "-o", str(binary),
                 *(s for s in sources if not s.endswith(".c")), *objects]
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
