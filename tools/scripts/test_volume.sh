#!/bin/bash
# Dedicated APFS test volume (sparse disk image) so the volume free-space auditor sees ONLY cache data. No admin rights needed.
# usage: test_volume.sh create <size-GiB> | attach | detach | path
IMG="${TP_BUILD_DIR:-$(dirname "$0")/../../build}/tp_test_volume.sparseimage"
NAME=TPTestVol
case "$1" in
  create) [ -f "$IMG" ] || hdiutil create -size "${2:-8}g" -fs APFS -volname $NAME -type SPARSE -quiet "${IMG%.sparseimage}" ;;
  attach) [ -d "/Volumes/$NAME" ] || hdiutil attach "$IMG" -nobrowse -noverify -quiet; touch "/Volumes/$NAME/.metadata_never_index"; echo "/Volumes/$NAME" ;;
  detach) hdiutil detach "/Volumes/$NAME" -quiet -force 2>/dev/null; ;;
  path) echo "/Volumes/$NAME" ;;
esac
