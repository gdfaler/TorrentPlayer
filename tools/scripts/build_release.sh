#!/bin/bash
# Self-contained release build of TorrentPlayer.app (Apple Silicon, macOS 27.0+).
# - Builds the C++ core and the Swift app in an isolated directory under dist/ (never touches build/).
# - Copies the non-system dynamic library closure into Contents/Frameworks and rewrites install names.
# - Signs ad-hoc (NOT notarized; Gatekeeper will warn on other Macs), verifies the result, writes a dependency inventory.
# Usage: tools/scripts/build_release.sh [output-name]     default output: dist/release   (TP_BUILD_TESTING=1 additionally compiles the test-only GlassTestDriver)
# Refuses to reuse an existing output directory; it never deletes anything.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
NAME=${1:-release}
case "$NAME" in ""|*[!A-Za-z0-9._-]*) echo "bad output name: $NAME" >&2; exit 2;; esac
D=$ROOT/dist; OUT=$D/$NAME; APP=$OUT/TorrentPlayer.app; CORE=$D/core_build; OBJ=$OUT/obj
[ ! -e "$OUT" ] || { echo "REFUSING: $OUT already exists (choose another output name)" >&2; exit 1; }
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1
export PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:${PKG_CONFIG_PATH:-}

echo "== 1/6 core (isolated build dir: dist/core_build)"
mkdir -p "$D"
cmake -G Ninja -S . -B "$CORE" > /dev/null
ninja -C "$CORE" tpcore

echo "== 2/6 app"
mkdir -p "$OBJ" "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
clang -c -O2 -Wall -Wextra -Wno-unused-parameter -arch arm64 -mmacosx-version-min=27.0 -I core/include -I app/Sources -I /opt/homebrew/opt/mpv/include app/Sources/tp_mpv_bridge.c -o "$OBJ/tp_mpv_bridge.o"
LT_LIBS=$(pkg-config --libs libtorrent-rasterbar)
TESTFLAG=(); [ "${TP_BUILD_TESTING:-}" = "1" ] && TESTFLAG=(-D TP_TESTING)      # testing builds only: compiles the env-gated GlassTestDriver (release builds contain no such hooks)
swiftc -O -swift-version 5 -target arm64-apple-macosx27.0 -module-name TorrentPlayer ${TESTFLAG[@]+"${TESTFLAG[@]}"} \
  -import-objc-header app/Sources/bridge.h -Xcc -I$ROOT/core/include -Xcc -I$ROOT/app/Sources -Xcc -I/opt/homebrew/opt/mpv/include \
  app/Sources/*.swift "$OBJ/tp_mpv_bridge.o" "$CORE/libtpcore.a" \
  -L/opt/homebrew/opt/mpv/lib -lmpv $LT_LIBS -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto -lc++ \
  -framework AppKit -framework QuartzCore -framework UniformTypeIdentifiers \
  -o "$APP/Contents/MacOS/TorrentPlayer"
cp app/Resources/Info.plist "$APP/Contents/Info.plist"

echo "== 3/6 bundle the dynamic library closure"
python3 - "$APP" "$OUT/dependency_inventory.txt" <<'PY'
import os, re, shutil, subprocess, sys
app, inv = sys.argv[1], sys.argv[2]
exe = app + "/Contents/MacOS/TorrentPlayer"
fw = app + "/Contents/Frameworks"
SYS = ("/usr/lib/", "/System/")

def run(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout

def own_id(p):
    o = run("otool", "-D", p).splitlines()[1:]
    return o[0].strip() if o else None

def dep_list(p):
    me, r, skipped = own_id(p), [], False
    for l in run("otool", "-L", p).splitlines()[1:]:
        l = l.strip()
        if not l:
            continue
        d = l.split(" (compatibility")[0]
        if me and d == me and not skipped:
            skipped = True
            continue
        r.append(d)
    return r

def rpaths(p):
    out, res = run("otool", "-l", p).splitlines(), []
    for i, l in enumerate(out):
        if l.strip() == "cmd LC_RPATH":
            for m in out[i + 1:i + 4]:
                m = m.strip()
                if m.startswith("path "):
                    res.append(m[5:].rsplit(" (offset", 1)[0])
    return res

def expand(rp, ref):
    return rp.replace("@loader_path", os.path.dirname(ref)).replace("@executable_path", os.path.dirname(exe))

def resolve(dep, ref):
    if dep.startswith(SYS):
        return None
    if dep.startswith("@rpath/"):
        rest = dep[len("@rpath/"):]
        for rp in rpaths(ref):
            e = expand(rp, ref)
            c = os.path.join(e, rest)
            if os.path.exists(c):
                return os.path.realpath(c)
            if e.startswith(SYS):
                return None
        sys.exit("cannot resolve %s referenced by %s" % (dep, ref))
    if dep.startswith("@loader_path/"):
        c = os.path.join(os.path.dirname(ref), dep[len("@loader_path/"):])
    elif dep.startswith("@executable_path/"):
        c = os.path.join(os.path.dirname(exe), dep[len("@executable_path/"):])
    else:
        c = dep
    if os.path.exists(c):
        return os.path.realpath(c)
    sys.exit("cannot resolve %s referenced by %s" % (dep, ref))

# discover the closure on the ORIGINAL files
name_of, edges, queue, seen = {}, {}, [exe], set()
while queue:
    f = queue.pop()
    if f in seen:
        continue
    seen.add(f)
    edges[f] = []
    for d in dep_list(f):
        real = resolve(d, f)
        if real is None:
            continue
        if real not in name_of:
            nm = os.path.basename(d)
            if nm in name_of.values():
                sys.exit("two different libraries share the file name %s" % nm)
            name_of[real] = nm
        edges[f].append((d, name_of[real]))
        queue.append(real)

# copy, set ids, rewrite references, drop non-system rpaths
def patch(src, dst, is_exe):
    for d, nm in edges[src]:
        new = ("@executable_path/../Frameworks/" if is_exe else "@loader_path/") + nm
        run("install_name_tool", "-change", d, new, dst)
    for rp in rpaths(src):
        if not expand(rp, src).startswith(SYS):
            run("install_name_tool", "-delete_rpath", rp, dst)

for real, nm in sorted(name_of.items(), key=lambda kv: kv[1]):
    dst = os.path.join(fw, nm)
    shutil.copy2(real, dst)
    os.chmod(dst, 0o755)
    run("install_name_tool", "-id", "@rpath/" + nm, dst)
    patch(real, dst, False)
patch(exe, exe, True)

# dependency inventory for the licensing review
def minos(p):
    try:
        m = re.search(r"minos\s+(\S+)", run("vtool", "-show-build", p))
        return m.group(1) if m else "?"
    except Exception:
        return "?"

def license_of(real):
    m = re.search(r"/Cellar/([^/]+)/([^/]+)/", real)
    if not m:
        return None, None, "not from a Homebrew Cellar"
    f, v = m.group(1), m.group(2)
    rb = "/opt/homebrew/Cellar/%s/%s/.brew/%s.rb" % (f, v, f)
    if not os.path.exists(rb):
        return f, v, "formula file not found locally (%s)" % rb
    lines, out = open(rb).read().splitlines(), []
    for i, l in enumerate(lines):
        if l.startswith("  license "):
            out.append(l.strip())
            j = i + 1
            while j < len(lines) and lines[j].startswith("    "):
                out.append(lines[j].strip())
                j += 1
            break
    return f, v, " ".join(out) if out else "no license line in the formula (UNKNOWN)"

with open(inv, "w") as w:
    w.write("TorrentPlayer bundled dependency inventory (generated by build_release.sh)\n")
    w.write("License text is the Homebrew formula's declared license (metadata only; not legal advice, not verified against the built binaries).\n\n")
    w.write("%d bundled libraries\n\n" % len(name_of))
    for real, nm in sorted(name_of.items(), key=lambda kv: kv[1]):
        f, v, lic = license_of(real)
        w.write("%s\n  source:   %s\n  formula:  %s %s\n  license:  %s\n  minos:    %s\n\n" % (nm, real, f, v, lic, minos(real)))
print("bundled %d libraries" % len(name_of))
PY

echo "== 3b/6 MoltenVK (the Vulkan driver mpv renders through; the loader dlopens it via an ICD file, so it is not part of the linked closure)"
MVK=$(readlink -f /opt/homebrew/opt/molten-vk)
[ -f "$MVK/lib/libMoltenVK.dylib" ] || { echo "FAIL: molten-vk is not installed in Homebrew ($MVK)" >&2; exit 1; }
cp "$MVK/lib/libMoltenVK.dylib" "$APP/Contents/Frameworks/libMoltenVK.dylib"
chmod 755 "$APP/Contents/Frameworks/libMoltenVK.dylib"
install_name_tool -id @rpath/libMoltenVK.dylib "$APP/Contents/Frameworks/libMoltenVK.dylib"
mkdir -p "$APP/Contents/Resources/vulkan/icd.d"
MVK_API=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['ICD']['api_version'])" "$MVK/etc/vulkan/icd.d/MoltenVK_icd.json")
cat > "$APP/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" <<ICD
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../../Frameworks/libMoltenVK.dylib",
        "api_version" : "$MVK_API",
        "is_portability_driver" : true
    }
}
ICD
MVK_VER=$(basename "$MVK"); MVK_LIC=$(grep -m1 '^  license ' "$MVK/.brew/molten-vk.rb" 2>/dev/null | sed 's/^ *//' || true)
printf 'libMoltenVK.dylib (loaded at run time through Resources/vulkan/icd.d/MoltenVK_icd.json, not linked)\n  source:   %s/lib/libMoltenVK.dylib\n  formula:  molten-vk %s\n  license:  %s\n  minos:    %s\n\n' "$MVK" "$MVK_VER" "${MVK_LIC:-UNKNOWN}" "$(vtool -show-build "$APP/Contents/Frameworks/libMoltenVK.dylib" | awk '/minos/{print $2; exit}')" >> "$OUT/dependency_inventory.txt"

echo "== 4/6 ad-hoc signing"
for f in "$APP"/Contents/Frameworks/*.dylib; do codesign --force --sign - "$f" 2> /dev/null; done
codesign --force --sign - "$APP" 2> /dev/null

echo "== 5/6 verification"
fail=0
check_file() {
  local f="$1" base id dep
  local archs; archs=$(lipo -archs "$f")
  [ "$archs" = "arm64" ] || { echo "FAIL arch of $f: $archs"; fail=1; }
  id=$(otool -D "$f" | sed -n '2p')
  while IFS= read -r dep; do
    [ -n "$dep" ] || continue
    [ "$dep" != "$id" ] || continue
    case "$dep" in
      /usr/lib/*|/System/*) ;;
      @executable_path/../Frameworks/*) [ -f "$APP/Contents/MacOS/../Frameworks/${dep#@executable_path/../Frameworks/}" ] || { echo "FAIL missing bundled lib for $dep (in $f)"; fail=1; } ;;
      @loader_path/*) [ -f "$(dirname "$f")/${dep#@loader_path/}" ] || { echo "FAIL missing bundled lib for $dep (in $f)"; fail=1; } ;;
      *) echo "FAIL external reference in $f: $dep"; fail=1 ;;
    esac
  done < <(otool -L "$f" | tail -n +2 | awk '{print $1}')
  otool -l "$f" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | while IFS= read -r rp; do
    case "$rp" in /usr/lib/*|/System/*) ;; *) echo "FAIL non-system rpath in $f: $rp"; exit 1 ;; esac
  done || fail=1
}
check_file "$APP/Contents/MacOS/TorrentPlayer"
for f in "$APP"/Contents/Frameworks/*.dylib; do check_file "$f"; done
ICDJSON="$APP/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
[ -f "$ICDJSON" ] || { echo "FAIL missing ICD file"; fail=1; }
ICDLIB=$(python3 -c "import json,os,sys; p=sys.argv[1]; print(os.path.normpath(os.path.join(os.path.dirname(p), json.load(open(p))['ICD']['library_path'])))" "$ICDJSON" 2> /dev/null)
[ -f "$ICDLIB" ] || { echo "FAIL ICD library_path does not resolve inside the bundle: $ICDLIB"; fail=1; }
plutil -lint "$APP/Contents/Info.plist" || fail=1
BID=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$APP/Contents/Info.plist")
[ "$BID" = "io.github.gdfaler.TorrentPlayer" ] || { echo "FAIL bundle id: $BID"; fail=1; }
codesign --verify --strict --verbose=2 "$APP" 2>&1 | sed 's/^/codesign: /' || fail=1
for f in "$APP"/Contents/Frameworks/*.dylib; do codesign --verify --strict "$f" 2> /dev/null || { echo "FAIL signature $f"; fail=1; }; done
# informational: Homebrew paths embedded as strings (config/data locations); NOT counted as failures
{ for f in "$APP/Contents/MacOS/TorrentPlayer" "$APP"/Contents/Frameworks/*.dylib; do
    strings -a "$f" | grep -E '/opt/homebrew|/usr/local/(Cellar|opt|lib|etc|share)' | sed "s#^#$(basename "$f"): #" || true
  done; } | sort -u > "$OUT/embedded_paths.txt"
echo "embedded Homebrew-style path strings: $(wc -l < "$OUT/embedded_paths.txt" | tr -d ' ') (see $OUT/embedded_paths.txt)"

echo "== 6/6 result"
echo "app:       $APP"
echo "libraries: $(ls "$APP/Contents/Frameworks" | wc -l | tr -d ' ')"
echo "inventory: $OUT/dependency_inventory.txt"
[ "$fail" -eq 0 ] || { echo "VERIFICATION FAILED"; exit 1; }
echo "verification passed (static checks only; NOT tested on another Mac; NOT notarized)"
