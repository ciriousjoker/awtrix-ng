#!/usr/bin/env python3
"""Pins an upstream AWTRIX NG release as a base of the macOS build.

  tools/tc002/macos/pin_base.py TAG [--repository OWNER/NAME]

Writes pins.json's base entry for TAG (vX.Y.Z): the commit the tag names and the SHA-256 GitHub
reports for its usb-awtrix-ng-tc002.zip and tc002-sources.tar.gz. build.py later downloads the ZIP
and refuses it unless it has that digest. Uses GITHUB_TOKEN or GH_TOKEN when set.
"""
import argparse
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = {"usb": "usb-awtrix-ng-tc002.zip", "sources": "tc002-sources.tar.gz"}


def api(path):
    request = urllib.request.Request(f"https://api.github.com/{path}",
                                     headers={"Accept": "application/vnd.github+json",
                                              "User-Agent": "awtrix-tc002-pin"})
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tag")
    parser.add_argument("--repository", default="Blueforcer/awtrix-ng")
    args = parser.parse_args(argv)
    match = re.fullmatch(r"v(\d+\.\d+\.\d+)", args.tag)
    if not match:
        parser.error("TAG must be vX.Y.Z")
    release = api(f"repos/{args.repository}/releases/tags/{args.tag}")
    digests = {asset["name"]: asset.get("digest", "") for asset in release.get("assets", [])}
    entry = {"repository": args.repository, "tag": args.tag}
    ref = api(f"repos/{args.repository}/git/ref/tags/{args.tag}")["object"]
    while ref["type"] == "tag":
        ref = api(f"repos/{args.repository}/git/tags/{ref['sha']}")["object"]
    entry["commit"] = ref["sha"]
    for key, name in ASSETS.items():
        digest = digests.get(name, "")
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
            print(f"error: {args.tag} has no {name} with a SHA-256 digest", file=sys.stderr)
            return 1
        entry[key] = {"file": name, "sha256": digest.split(":", 1)[1]}
    path = HERE / "pins.json"
    pins = json.loads(path.read_text())
    pins["base"][match[1]] = entry
    path.write_text(json.dumps(pins, indent=2) + "\n")
    print(f"pinned {args.tag}: {entry['usb']['sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
