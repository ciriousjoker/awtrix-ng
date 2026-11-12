#!/bin/sh
# Builds the TC002 release natively on an Apple Silicon Mac. See build.py for the options and what
# it builds and reuses; docs/developers/tc002/building-on-macos.md for the setup.
#
#   tools/tc002/macos/build.sh OUTPUT_DIR [--commit REV | --working-tree] [--jobs N] [--no-usb]
exec python3 "$(dirname "$0")/build.py" "$@"
