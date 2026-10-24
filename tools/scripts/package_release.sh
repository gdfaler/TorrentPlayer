#!/bin/bash
# Builds the release-candidate folder for ONE already built app (tools/scripts/build_release.sh <name>): the arm64 ZIP, SHA-256 checksums, build metadata,
# the third-party licence / notice / source-manifest material, the TorrentPlayer source archive and the release notes. It does not publish anything and uses no network.
# Usage: tools/scripts/package_release.sh <build-name>      (reads dist/<build-name>/TorrentPlayer.app; writes dist/release/<version>-<build-name>/)
# Refuses to overwrite an existing output folder. A release must be built from a clean, committed tree: BUILD_METADATA.json records whether the tree was dirty.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
NAME=${1:?usage: package_release.sh <build-name>}
case "$NAME" in *[!A-Za-z0-9._-]*) echo "bad build name" >&2; exit 2;; esac
BUILD=$ROOT/dist/$NAME; APP=$BUILD/TorrentPlayer.app
[ -d "$APP" ] || { echo "no app at $APP (run tools/scripts/build_release.sh $NAME first)" >&2; exit 1; }
[ -d "$BUILD/third_party" ] || { echo "no $BUILD/third_party: this build predates the licence step; rebuild it" >&2; exit 1; }
VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist")
OUT=$ROOT/dist/release/$VER-$NAME
[ ! -e "$OUT" ] || { echo "REFUSING: $OUT already exists" >&2; exit 1; }
mkdir -p "$OUT"
ZIP=TorrentPlayer-$VER-macos-arm64.zip
SRC=TorrentPlayer-$VER-source.tar.gz
RECIPES=TorrentPlayer-$VER-third-party-build-recipes.tar.gz
TPSRC=TorrentPlayer-$VER-third-party-sources.tar
SRCDIR=${TP_THIRD_PARTY_SOURCES:-$ROOT/dist/sources/$VER}
[ -f "$SRCDIR/provenance.tsv" ] || { echo "no fetched third-party sources at $SRCDIR (run tools/scripts/fetch_third_party_sources.py first)" >&2; exit 1; }

echo "== 0/6 checksums of every file in the SIGNED app (the third-party step ran before signing, so this list replaces its pre-signing one)"
(cd "$BUILD" && find TorrentPlayer.app -type f -print0 | sort -z | xargs -0 shasum -a 256 > "$BUILD/third_party/bundled_files.sha256")

echo "== 1/6 ZIP (ditto: keeps the bundle layout and the signature; no resource forks / extended attributes / quarantine flags)"
(cd "$BUILD" && ditto -c -k --keepParent --norsrc --noextattr --noacl --noqtn TorrentPlayer.app "$OUT/$ZIP")

echo "== 2/6 TorrentPlayer source archive (tracked files, plus untracked files that are not ignored)"
TREE=$(mktemp -d); LIST=$TREE/list
git ls-files -co --exclude-standard > "$LIST"
tar -czf "$OUT/$SRC" -s ",^,TorrentPlayer-$VER-source/," -T "$LIST"
rm -rf "$TREE"

echo "== 3/6 third-party build recipes, licence texts and source manifest"
cp "$BUILD/third_party/THIRD_PARTY_NOTICES.md" "$BUILD/third_party/SOURCE_OFFER.txt" "$BUILD/third_party/COMPLIANCE_STATUS.md" "$OUT/"
cp "$ROOT/LICENSE" "$OUT/LICENSE"
cp "$BUILD/third_party/source-manifest.tsv" "$OUT/source-manifest.tsv"
[ -f "$ROOT/docs/RELEASE_NOTES.md" ] && cp "$ROOT/docs/RELEASE_NOTES.md" "$OUT/RELEASE_NOTES.md"
(cd "$BUILD/third_party" && tar -czf "$OUT/$RECIPES" -s ",^,TorrentPlayer-$VER-third-party/," formulae receipts sbom licenses source-manifest.tsv fetch_third_party_sources.py bundled_files.sha256 COMPLIANCE_STATUS.md)

echo "== 3b/6 third-party source archives and patches (already fetched and verified by fetch_third_party_sources.py; re-checked here)"
python3 - "$SRCDIR" <<'PYC'
import csv, hashlib, os, sys
d = sys.argv[1]; bad = []
for r in csv.DictReader(open(os.path.join(d, "provenance.tsv")), delimiter="\t"):
    p = os.path.join(d, r["file"] if r["file"].startswith("patches/") else os.path.join("archives", r["file"]))
    if not r["status"].startswith(("VERIFIED", "DOWNLOADED", "EXTRACTED")) or not os.path.exists(p) or hashlib.sha256(open(p, "rb").read()).hexdigest() != r["sha256_computed"]:
        bad.append(r["file"])
sys.exit("source package check FAILED: %s" % bad) if bad else print("all provenance rows present, checksum-consistent and verified")
PYC
(cd "$SRCDIR" && tar -cf "$OUT/$TPSRC" -s ",^,TorrentPlayer-$VER-third-party-sources/," archives patches provenance.tsv FETCH_REPORT.md)
cp "$SRCDIR/provenance.tsv" "$OUT/provenance.tsv"

echo "== 4/6 build metadata"
TPSRC="$TPSRC" python3 - "$APP" "$BUILD" "$OUT" "$ZIP" "$SRC" "$RECIPES" "$VER" <<'PY'
import json, os, re, subprocess, sys, hashlib, platform
app, build, out, zipn, srcn, recn, ver = sys.argv[1:8]
def sh(*a):
    try: return subprocess.run(a, capture_output=True, text=True, check=True).stdout.strip()
    except Exception: return "unknown"
def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""): h.update(c)
    return h.hexdigest()
home = os.path.expanduser("~")
dirty = sh("git", "status", "--porcelain").splitlines()
deps = []
for blk in open(os.path.join(build, "dependency_inventory.txt")).read().split("\n\n"):
    m = re.search(r"formula:\s+(\S+) (\S+)", blk)
    if m: deps.append({"formula": m.group(1), "version": m.group(2), "library": blk.strip().split("\n")[0].split()[0]})
plist = lambda k: sh("/usr/libexec/PlistBuddy", "-c", "Print :" + k, app + "/Contents/Info.plist")
meta = {
  "app": {"name": "TorrentPlayer", "version": ver, "build": plist("CFBundleVersion"), "bundle_id": plist("CFBundleIdentifier"), "min_macos": plist("LSMinimumSystemVersion"),
          "architectures": ["arm64"], "signing": "ad-hoc (not notarized)", "license": "GPL-3.0-or-later"},
  "source": {"git_revision": sh("git", "rev-parse", "HEAD"), "working_tree_clean": not dirty, "uncommitted_entries": len(dirty),
             "note": "a release must be built from a clean, committed tree" if dirty else "built from a clean tree"},
  "build_host": {"macos": sh("sw_vers", "-productVersion"), "machine": platform.machine(), "swift": sh("swiftc", "--version").split("\n")[0], "clang": sh("clang", "--version").split("\n")[0],
                 "cmake": sh("cmake", "--version").split("\n")[0], "ninja": sh("ninja", "--version")},
  "bundled_libraries": deps,
  "artifacts": {n: {"sha256": sha(os.path.join(out, n)), "bytes": os.path.getsize(os.path.join(out, n))} for n in (zipn, srcn, recn, os.environ["TPSRC"])},
  "bundled_files_sha256": "see third-party recipes archive: bundled_files.sha256",
}
text = json.dumps(meta, indent=2).replace(home, "~")
open(os.path.join(out, "BUILD_METADATA.json"), "w").write(text + "\n")
print("recorded %d bundled libraries; working tree clean: %s" % (len(deps), meta["source"]["working_tree_clean"]))
PY

echo "== 5/6 checksums"
(cd "$OUT" && shasum -a 256 "$ZIP" "$SRC" "$RECIPES" "$TPSRC" provenance.tsv BUILD_METADATA.json THIRD_PARTY_NOTICES.md SOURCE_OFFER.txt COMPLIANCE_STATUS.md source-manifest.tsv LICENSE $( [ -f RELEASE_NOTES.md ] && echo RELEASE_NOTES.md ) > SHA256SUMS)

echo "== 6/6 verify the ZIP: extract to a scratch folder, signature, bundle identity and file-by-file hashes"
V=$(mktemp -d); ditto -x -k "$OUT/$ZIP" "$V"
fail=0
[ -d "$V/TorrentPlayer.app" ] || { echo "FAIL: ZIP does not contain TorrentPlayer.app at its top level"; fail=1; }
[ -z "$(find "$V" -name '__MACOSX' -o -name '._*' | head -1)" ] || { echo "FAIL: AppleDouble entries in the ZIP"; fail=1; }
codesign --verify --deep --strict "$V/TorrentPlayer.app" 2>&1 | sed 's/^/codesign: /' || fail=1
BAD=$(cd "$V" && shasum -a 256 -c "$BUILD/third_party/bundled_files.sha256" 2>&1 | grep -v ': OK$' || true)
[ -z "$BAD" ] || { echo "FAIL after extraction: $BAD"; fail=1; }
[ "$(find "$V/TorrentPlayer.app" -type f | wc -l)" = "$(find "$APP" -type f | wc -l)" ] || { echo "FAIL: file count differs after extraction"; fail=1; }
[ -n "$V" ] && echo "scratch extraction kept for inspection: $V"
BAD=$(cd "$OUT" && shasum -a 256 -c SHA256SUMS 2>&1 | grep -v ': OK$' || true)
[ -z "$BAD" ] || { echo "FAIL checksum: $BAD"; fail=1; }
echo "release candidate: $OUT"
ls -l "$OUT" | awk 'NR>1{print "  " $5 "\t" $9}'
# manifest coverage: every bundled formula has a verified source row, every bundled library is named in the notices
python3 - "$BUILD" "$OUT" <<'PYC' || fail=1
import csv, os, re, sys
build, out = sys.argv[1:3]
inv = {m.group(1) for m in re.finditer(r"formula:\s+(\S+) ", open(os.path.join(build, "dependency_inventory.txt")).read())}
man = {r["formula"] for r in csv.DictReader(open(os.path.join(out, "source-manifest.tsv")), delimiter="\t")}
prov = {r["formula"] for r in csv.DictReader(open(os.path.join(out, "provenance.tsv")), delimiter="\t") if r["status"].startswith("VERIFIED")}
notices = open(os.path.join(out, "THIRD_PARTY_NOTICES.md")).read()
libs = sorted(os.listdir(os.path.join(build, "TorrentPlayer.app/Contents/Frameworks")))
problems = ["no manifest row: %s" % x for x in inv - man] + ["manifest row without bundled library: %s" % x for x in man - inv] + ["source not verified: %s" % x for x in man - prov] + ["not in notices: %s" % x for x in libs if "`%s`" % x not in notices]
print("coverage: %d bundled formulae, %d manifest rows, %d verified sources, %d bundled files in the notices%s" % (len(inv), len(man), len(prov), len(libs) - len([x for x in libs if "`%s`" % x not in notices]), "" if not problems else "; PROBLEMS: " + "; ".join(problems)))
sys.exit(1 if problems else 0)
PYC
[ "$fail" = 0 ] && echo "package verification passed (NOT tested on another Mac; NOT notarized; NOT published)" || { echo "package verification FAILED" >&2; exit 1; }
