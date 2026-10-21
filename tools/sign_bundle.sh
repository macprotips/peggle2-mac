#!/bin/bash
# Signs an app bundle ad hoc, so it runs on Apple silicon. Extended attributes (Finder metadata) would make
# codesign fail, so they are cleared first. usage: sign_bundle.sh <app>
set -euo pipefail
xattr -cr "$1"
codesign --force --sign - "$1"
