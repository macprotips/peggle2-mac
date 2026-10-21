#!/bin/bash
# Signs the app bundle ad hoc (Developer ID signing for distribution is separate; see README). Older builds
# wrote a custom Finder icon into the bundle (an Icon\r file and extended attributes); codesign rejects them.
set -e
rm -f "$1/Icon"$'\r'
xattr -cr "$1"
codesign --force --sign - "$1"
