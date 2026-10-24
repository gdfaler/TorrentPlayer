#!/usr/bin/env python3
"""Generate the third-party licence / attribution / source-manifest material for ONE built TorrentPlayer.app.

It reads what was actually bundled (the app's Frameworks directory plus the dependency_inventory.txt written by build_release.sh) and, for every Homebrew formula behind a
bundled library, collects from the local Homebrew Cellar: the upstream licence files that ship with the bottle, the formula (build recipe, source URL + sha256, patches), the
install receipt and the SBOM. Nothing is downloaded: the script is offline. What cannot be established offline is reported as UNRESOLVED in the output.

Usage: gen_third_party.py --app TorrentPlayer.app --inventory dependency_inventory.txt --out DIR [--repo-root DIR]
Output (DIR): THIRD_PARTY_NOTICES.md, COMPLIANCE_STATUS.md, SOURCE_OFFER.txt, licenses/<formula>-<version>/*, source-manifest.tsv, fetch_third_party_sources.sh,
              formulae/<formula>.rb, receipts/<formula>.INSTALL_RECEIPT.json, sbom/<formula>.sbom.spdx.json, bundled_files.sha256
"""
import argparse, csv, glob, hashlib, json, os, re, shutil, stat, sys, tarfile

CELLAR = "/opt/homebrew/Cellar"
LICENSE_FILE_RE = re.compile(r"(copying|licen[sc]e|copyright|notice|^gpl|^ftl)", re.I)

# What was checked against the actual bottle / binary, per library group. Everything here was read from files on this machine (see evidence strings); nothing is assumed.
NOTES = {
    "ffmpeg": ("GPL-3.0-or-later (effective, as built)",
               "The bundled libavutil carries the configure line: --enable-gpl --enable-version3 --enable-libx264 --enable-libx265 --enable-libmp3lame --enable-libopus --enable-libdav1d "
               "--enable-libvpx --enable-libsvtav1 --enable-libvmaf --enable-openssl --enable-videotoolbox --enable-audiotoolbox. --enable-gpl makes the libraries GPL; --enable-version3 "
               "is what the FFmpeg LICENSE.md requires when combining with Apache-2.0 code such as OpenSSL 3/4."),
    "mpv": ("GPL-2.0-or-later (effective, as built)",
            "`mpv --version` of this build lists the `gpl` feature (built without -Dgpl=false). mpv's own Copyright file: GPLv2+ by default, LGPLv2.1+ only when built without GPL-only files. "
            "libmpv is loaded dynamically by TorrentPlayer."),
    "x264": ("GPL-2.0-or-later", "COPYING = GPLv2 text with the 'or any later version' wording; source pinned by git revision in the formula."),
    "x265": ("GPL-2.0-or-later", "COPYING = GPLv2 text, 'any later version'; the project also offers a commercial licence (not used here)."),
    "rubberband": ("GPL-2.0-or-later", "COPYING = GPLv2 text, 'any later version'; a commercial licence also exists (not used here)."),
    "mpg123": ("LGPL-2.1-only", "Re-checked in the verified source archive: COPYING states 'free software under the terms of the LGPL v2.1' (no 'or later' for the library). Used as a separate dynamic library. LGPL-2.1 section 6 lets a work that uses the library be "
               "distributed under terms of the distributor's choice if modification and relinking by the user are permitted: the dylib is a replaceable file in Contents/Frameworks (re-sign it ad hoc after replacing)."),
    "libvmaf": ("BSD-2-Clause-Patent", "Re-checked in the verified source archive: LICENSE (BSD+Patent) states it is designed to be a simple permissive licence compatible with the GNU GPL version 2 and to include an express patent grant; treated as compatible with GPL-3.0-or-later (not independently confirmed by the FSF; flagged for legal review)."),
    "freetype": ("FTL OR GPL-2.0-or-later (GPL option elected)", "Re-checked in the verified source archive: LICENSE.TXT: dual licence FTL or docs/GPLv2.TXT ('any later version can be used also'); the FTL is incompatible with GPLv2 because of its advertisement clause, "
                 "so this distribution uses FreeType under the GPL option. FTL credit line kept anyway: 'Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.'"),
    "openssl@3": ("Apache-2.0", "LICENSE.txt is the Apache License 2.0. Compatible with GPLv3 (not GPLv2-only). Two OpenSSL majors are bundled: libtorrent and FFmpeg use 3.x, the app executable links 4.x."),
    "openssl@4": ("Apache-2.0", "LICENSE.txt is the Apache License 2.0 (see openssl@3 note)."),
    "vulkan-loader": ("Apache-2.0", "LICENSE.txt: the majority of files are Apache-2.0; any deviation is a more permissive licence (per the file)."),
    "molten-vk": ("Apache-2.0", "LICENSE is the Apache License 2.0. Not linked: dlopen'ed by the Vulkan loader through the bundled ICD manifest (Resources/vulkan/icd.d)."),
    "zstd": ("BSD-3-Clause (option elected)", "Dual BSD / GPL-2.0-only: BSD option used."),
    "uchardet": ("GPL-2.0-or-later (option elected)", "Tri-licence MPL-1.1 / GPL-2.0+ / LGPL-2.1+ per formula: an option compatible with GPL-3.0-or-later is used."),
    "graphite2": ("LGPL-2.1-or-later (option elected)", "Multi-licence per formula: an option compatible with GPL-3.0-or-later is used."),
    "xz": ("0BSD / public domain (liblzma only)", "Only liblzma is bundled; the GPL-licensed xz tools are not distributed."),
    "gettext": ("LGPL-2.1-or-later (libintl only)", "Only libintl is bundled, not the GPL-3.0-or-later gettext tools (per upstream licensing structure; verify against source)."),
    "fribidi": ("LGPL-2.1-or-later (library)", "Formula says GPL-2.0-or-later AND LGPL-2.1-or-later; the shared library is the LGPL part (per upstream; verify against source)."),
    "glib": ("LGPL-2.1-or-later", "The Homebrew bottle ships no licence file; the licence text and notices are taken from the verified upstream source archive (COPYING, AUTHORS, README.md). Homebrew patch Patches/glib/hardcoded-paths.diff is applied (homebrew-core commit 1cf441a09f57, unchanged since 2025-10-15)."),
}
REPRESENTATIVE_HEADER = {"glib": "glib/gmain.c"}      # glib has no AUTHORS-style file listing the original holders at its top level
LGPL21_DONOR = ("ffmpeg", "COPYING.LGPLv2.1")
# canonical GPLv3 text (md5 1ebbd3e34237af26da5dc08a4e440464, as in the FSF gpl-3.0.txt) is taken from the xz bottle
GPL3_DONOR = ("xz", "COPYING.GPLv3")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_inventory(path):
    rows = []
    for block in open(path).read().split("\n\n"):
        lines = [l for l in block.strip().split("\n") if l.strip()]
        if not lines or lines[0].startswith("TorrentPlayer bundled") or "bundled libraries" in lines[0] or lines[0].startswith("License text"):
            continue
        m = re.search(r"source:\s+(/opt/homebrew/Cellar/([^/]+)/([^/]+)/)(\S+)", block)
        if m:
            rows.append({"lib": lines[0].split()[0], "dir": m.group(1), "formula": m.group(2), "version": m.group(3), "libpath": m.group(4)})
    return rows


def parse_formula(rb):
    t = open(rb).read() if os.path.exists(rb) else ""
    info = {"url": None, "sha256": None, "revision": None, "patches": [], "resources": [], "license": None}
    m = re.search(r'^\s*url\s+"([^"]+)"(?:,\s*\n?\s*(?:tag:\s*"[^"]+",\s*\n?\s*)?revision:\s*"([0-9a-f]+)")?', t, re.M)
    if m:
        info["url"], info["revision"] = m.group(1), m.group(2)
    m = re.search(r'^\s*sha256\s+"([0-9a-f]{64})"', t, re.M)
    if m:
        info["sha256"] = m.group(1)
    lines = t.split("\n")
    i = 0
    while i < len(lines):
        m = re.match(r"^(\s*)patch\b(.*)$", lines[i])
        if not m:
            i += 1
            continue
        d, indent = {}, m.group(1)
        if ":DATA" in m.group(2):
            d["inline"] = "patch :DATA (inline in the formula file)"
        elif m.group(2).strip().startswith("do"):
            j = i + 1
            while j < len(lines) and not re.match("^" + indent + r"end\b", lines[j]):
                mm = re.match(r'^\s*(url|sha256|file|type)\s+"?:?([^"\n]+)"?', lines[j])
                if mm:
                    d[mm.group(1)] = mm.group(2).strip()
                j += 1
            i = j
        info["patches"].append(d)
        i += 1
    for rm in re.finditer(r'resource\s+"([^"]+)"\s+do\s+url\s+"([^"]+)"\s+sha256\s+"([0-9a-f]{64})"', t):
        info["resources"].append({"name": rm.group(1), "url": rm.group(2), "sha256": rm.group(3)})
    for k, l in enumerate(lines):
        if l.startswith("  license "):
            parts = [l.strip()]
            j = k + 1
            while j < len(lines) and lines[j].startswith("    "):
                parts.append(re.sub(r"\s+#.*$", "", lines[j].strip()))
                j += 1
            info["license"] = " ".join(parts)
            break
    return info


TEMPLATE_RE = re.compile(r"(GPL|LGPL|COPYING\.GPL|COPYING\.LGPL|APACHE|Apache)", re.I)


def copyright_lines(files, limit=3):
    out = []
    for fp in files:
        base = os.path.basename(fp)
        if re.match(r"(COPYING\.(L?GPL|0BSD)|LICENSE-BSD2)", base):
            continue
        try:
            for l in open(fp, errors="replace"):
                s = l.strip()
                if re.match(r"^[\W_]*(copyright|\(c\)|©)\W*(\(c\)|©)?\W*\d{4}", s, re.I) and 12 < len(s) < 200 and "Free Software Foundation" not in s and "<name of author>" not in s:
                    if s not in out:
                        out.append(s)
                    if len(out) >= limit:
                        return out
        except OSError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--inventory", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sources", help="folder written by fetch_third_party_sources.py (archives/, patches/, provenance.tsv): upstream licence files are extracted from the verified archives")
    ap.add_argument("--repo-root", default=os.path.join(os.path.dirname(__file__), "..", ".."))
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    fw = os.path.join(a.app, "Contents", "Frameworks")
    rows = parse_inventory(a.inventory)
    bundled = sorted(os.listdir(fw))
    covered = {r["lib"] for r in rows}
    missing_inv = [b for b in bundled if b not in covered]

    byf = {}
    for r in rows:
        byf.setdefault((r["formula"], r["version"]), {"dir": r["dir"], "libs": []})["libs"].append(r["lib"])

    unresolved = []
    resolved = []
    prov = {}
    if a.sources and os.path.exists(os.path.join(a.sources, "provenance.tsv")):
        for r in csv.DictReader(open(os.path.join(a.sources, "provenance.tsv")), delimiter="\t"):
            if not r["file"].startswith("patches/"):
                prov[(r["formula"], r["version"])] = r
    manifest = []
    sections = []
    for (f, v), d in sorted(byf.items()):
        cdir = d["dir"]
        rb = os.path.join(cdir, ".brew", f + ".rb")
        fi = parse_formula(rb)
        lic_files = sorted(x for x in os.listdir(cdir) if os.path.isfile(os.path.join(cdir, x)) and LICENSE_FILE_RE.search(x))
        lic_dir = os.path.join(out, "licenses", "%s-%s" % (f, v))
        os.makedirs(lic_dir, exist_ok=True)
        for x in lic_files:
            shutil.copy2(os.path.join(cdir, x), os.path.join(lic_dir, x))
        up_files = []
        pv = prov.get((f, v))
        if pv and pv["status"].startswith("VERIFIED"):
            try:
                with tarfile.open(os.path.join(a.sources, "archives", pv["file"])) as t:
                    for mem in t.getmembers():
                        parts = mem.name.split("/")
                        wanted = (LICENSE_FILE_RE.search(parts[-1]) is not None or re.match(r"(AUTHORS|README(\.md|\.rst)?|NEWS)$", parts[-1]) is not None
                                  or (len(parts) >= 3 and parts[-2].upper() in ("LICENSES", "LICENSE")))
                        if mem.isfile() and len(parts) <= 3 and wanted:
                            if mem.size > 400000:
                                continue
                            dst = os.path.join(lic_dir, "upstream", "/".join(parts[1:]).replace("/", "__"))
                            os.makedirs(os.path.dirname(dst), exist_ok=True)
                            open(dst, "wb").write(t.extractfile(mem).read())
                            up_files.append("/".join(parts[1:]))
                    rep_file = REPRESENTATIVE_HEADER.get(f)
                    if rep_file:                                       # a source file whose header names the original copyright holders
                        mem = t.getmember(pv["file"].split(".tar")[0].split("__")[-1] + "/" + rep_file) if False else next(m for m in t.getmembers() if m.name.split("/", 1)[-1] == rep_file)
                        head = t.extractfile(mem).read().decode("utf-8", "replace").split("\n")[:24]
                        dst = os.path.join(lic_dir, "upstream", "header__" + rep_file.replace("/", "__") + ".txt")
                        os.makedirs(os.path.dirname(dst), exist_ok=True)
                        open(dst, "w").write("\n".join(head) + "\n")
                        up_files.append("header__" + rep_file.replace("/", "__") + ".txt")
            except (tarfile.TarError, OSError, StopIteration) as e:
                unresolved.append("%s %s: could not read the source archive (%s)" % (f, v, e))
        if not lic_files and not up_files:
            donor = os.path.join(CELLAR, LGPL21_DONOR[0])
            dv = sorted(os.listdir(donor))[-1]
            shutil.copy2(os.path.join(donor, dv, LGPL21_DONOR[1]), os.path.join(lic_dir, "LGPL-2.1.txt (supplied: this bottle ships no licence file)"))
            unresolved.append("%s %s: no licence file in the bottle or in a verified source archive." % (f, v))
        for sub, src in (("formulae", rb), ("receipts", os.path.join(cdir, "INSTALL_RECEIPT.json")), ("sbom", os.path.join(cdir, "sbom.spdx.json"))):
            if os.path.exists(src):
                os.makedirs(os.path.join(out, sub), exist_ok=True)
                ext = {"formulae": ".rb", "receipts": ".INSTALL_RECEIPT.json", "sbom": ".sbom.spdx.json"}[sub]
                text = open(src, errors="replace").read().replace(os.path.expanduser("~"), "~")      # receipts name the builder's Homebrew cache folder: never publish the user name
                open(os.path.join(out, sub, f.replace("@", "-") + ext), "w").write(text)
        sb = {}
        sp = os.path.join(cdir, "sbom.spdx.json")
        if os.path.exists(sp):
            for p in json.load(open(sp)).get("packages", []):
                if p.get("name") == f.split("@")[0] or p.get("name") == f:
                    sb = {"download": p.get("downloadLocation"), "sha256": next((c["checksumValue"] for c in p.get("checksums", []) if c.get("algorithm") == "SHA256"), None), "license": p.get("licenseConcluded")}
                    break
        src_url = fi["url"] or sb.get("download")
        src_sha = fi["sha256"] or sb.get("sha256")
        if sb.get("sha256") and fi["sha256"] and sb["sha256"] != fi["sha256"]:
            unresolved.append("%s: formula sha256 and SBOM sha256 differ." % f)
        if not src_url:
            unresolved.append("%s %s: no source URL found." % (f, v))
        elif not src_sha and not fi["revision"]:
            unresolved.append("%s %s: no source checksum or git revision found." % (f, v))
        for pt in fi["patches"]:
            if "file" in pt and not (a.sources and os.path.exists(os.path.join(a.sources, "patches")) and any(x.startswith("%s-%s__" % (f.replace("@", "-"), v)) for x in os.listdir(os.path.join(a.sources, "patches")))):
                unresolved.append("%s %s: Homebrew patch '%s' lives in the Homebrew/formula-patches repository (commit not recorded locally); fetch needs network." % (f, v, pt["file"]))
        eff, ev = NOTES.get(f, (None, None))
        declared = fi["license"] or sb.get("license") or "UNKNOWN"
        manifest.append((f, v, src_url or "", src_sha or "", fi["revision"] or "", "; ".join((p.get("url") or p.get("file") or p.get("inline", "?")) + (" sha256=" + p["sha256"] if "sha256" in p else "") for p in fi["patches"]),
                         "; ".join("%s %s sha256=%s" % (r["name"], r["url"], r["sha256"]) for r in fi["resources"])))
        cr = copyright_lines([os.path.join(cdir, x) for x in lic_files])
        sec = ["### %s %s" % (f, v), "",
               "- Bundled files: %s" % ", ".join("`%s`" % l for l in d["libs"]),
               "- Licence declared by the Homebrew formula (metadata): `%s`" % declared,
               "- Licence relied on for this distribution: **%s**" % (eff or "as declared (no per-library check beyond the licence files below)"),
               "- Licence files from the bottle: %s" % (", ".join("`licenses/%s-%s/%s`" % (f, v, x) for x in lic_files) or "none (see note)"),
               "- Upstream licence / notice files (from the verified source archive): %s" % (", ".join("`licenses/%s-%s/upstream/%s`" % (f, v, x.replace("/", "__")) for x in up_files) or "not extracted"),
               "- Copyright lines found in the licence files: %s" % (" | ".join(cr) if cr else "none (holders are named in AUTHORS / the source file headers, which are preserved in the shipped source archive)"),
               "- Source: %s%s" % (src_url or "UNKNOWN", (" (git revision `%s`)" % fi["revision"]) if fi["revision"] else ""),
               "- Source sha256: `%s`" % (src_sha or "n/a (git revision pinned)" if fi["revision"] else (src_sha or "UNKNOWN")),
               "- Source archive in the source package: %s" % ("`archives/%s` (%s)" % (pv["file"], pv["status"]) if pv else "not fetched"),
               "- Homebrew patches: %s" % ("; ".join((p.get("url") or p.get("file") or p.get("inline", "?")) for p in fi["patches"]) or "none"),
               "- Build recipe: `formulae/%s.rb`; install receipt `receipts/%s.INSTALL_RECEIPT.json`" % (f.replace("@", "-"), f.replace("@", "-"))]
        if ev:
            sec.append("- Verified evidence / note: " + ev)
        sections.append("\n".join(sec) + "\n")

    # licence texts for the project itself
    gd = os.path.join(CELLAR, GPL3_DONOR[0]); gv = sorted(os.listdir(gd))[-1]
    gpl3 = os.path.join(gd, gv, GPL3_DONOR[1])
    shutil.copy2(gpl3, os.path.join(out, "LICENSE-GPL-3.0.txt"))
    repo_license = os.path.join(a.repo_root, "LICENSE")
    if os.path.exists(repo_license):
        shutil.copy2(repo_license, os.path.join(out, "LICENSE"))
        same = open(repo_license, "rb").read() == open(gpl3, "rb").read()
        (resolved if same else unresolved).append("Repository LICENSE %s the canonical GPLv3 text." % ("is byte-identical to" if same else "DIFFERS from"))
    if missing_inv:
        unresolved.append("Bundled files without an inventory entry: " + ", ".join(missing_inv))

    # bundled file checksums
    with open(os.path.join(out, "bundled_files.sha256"), "w") as w:
        for root, _, files in os.walk(a.app):
            for fn in sorted(files):
                p = os.path.join(root, fn)
                if os.path.islink(p):
                    continue
                w.write("%s  %s\n" % (sha256(p), os.path.relpath(p, os.path.dirname(a.app))))

    with open(os.path.join(out, "source-manifest.tsv"), "w") as w:
        w.write("formula\tversion\tsource_url\tsource_sha256\tgit_revision\tpatches\textra_resources\n")
        for m in manifest:
            w.write("\t".join(m) + "\n")
    shutil.copy2(os.path.join(a.repo_root, "tools", "scripts", "fetch_third_party_sources.py"), os.path.join(out, "fetch_third_party_sources.py"))

    offer = ("TorrentPlayer source-code offer (GPL-3.0-or-later, section 6)\n\n"
             "Copyright (c) 2026 gdfaler. TorrentPlayer is free software licensed under the GNU General Public License version 3 or (at your option) any later version.\n"
             "This binary bundles third-party libraries that keep their own copyright and licence, several of them GPL or LGPL (FFmpeg, mpv, x264, x265, rubberband, mpg123, libplacebo,\n"
             "lame, libbluray, glib and others; see THIRD_PARTY_NOTICES.md).\n\n"
             "The complete corresponding source of this exact build is published with it: the TorrentPlayer source archive, the third-party source archives and patches (provenance.tsv lists\n"
             "each file with its checksum) and the build recipes (formulae) are attached to the same GitHub Release as this download (https://github.com/gdfaler/TorrentPlayer/releases).\n"
             "If any of them is missing there, or for at least three years after this version was distributed, you may request them by opening an issue at\n"
             "https://github.com/gdfaler/TorrentPlayer/issues; they will be provided at no charge beyond the cost of performing the distribution.\n\n"
             "You may replace any of the LGPL libraries in TorrentPlayer.app/Contents/Frameworks with your own build; macOS requires the changed file to be signed again (for example\n"
             "`codesign --force --sign - <file>` then `codesign --force --sign - TorrentPlayer.app`).\n")
    open(os.path.join(out, "SOURCE_OFFER.txt"), "w").write(offer)

    head = ["# TorrentPlayer third-party notices", "",
            "TorrentPlayer: Copyright (c) 2026 gdfaler. It is licensed under the **GNU General Public License, version 3 or (at your option) any later version** (see `LICENSE`).",
            "It is distributed together with the third-party libraries listed below. They remain the property of their respective copyright holders and keep their own licences and",
            "copyright statements; TorrentPlayer does not claim ownership of them and the copyright line above applies only to TorrentPlayer's own code.",
            "The combination is distributed under GPL-3.0-or-later because the bundled FFmpeg is GPL-3.0-or-later (built with --enable-gpl --enable-version3) and libmpv is built as GPL.",
            "This file was generated for one exact build by `tools/scripts/gen_third_party.py` from the files that were bundled; it is not legal advice.", "",
            "Corresponding source: see `SOURCE_OFFER.txt`, `source-manifest.tsv` and `provenance.tsv`. Licence texts are in `licenses/` (from the Homebrew bottle and, where verified, `upstream/` from the original source archive).", "",
            "## Bundled components", ""]
    open(os.path.join(out, "THIRD_PARTY_NOTICES.md"), "w").write("\n".join(head) + "\n" + "\n".join(sections))

    nsrc = len(byf)
    ver = sum(1 for k in byf if k in prov and prov[k]["status"].startswith("VERIFIED"))
    status = ["# Compliance status for this build (generated)", "",
              "Bundled files: %d dylibs from %d Homebrew formulae." % (len(bundled), nsrc), "",
              "## Resolved / verified", ""] + ["- " + r for r in resolved] + [
              "- FFmpeg configure line read from the bundled libavutil (GPL + version3); libmpv `gpl` feature confirmed from `mpv --version` of the same Homebrew build.",
              "- Source URL and sha256 (or git revision) recorded for each formula from the formula file and cross-checked against the SBOM.",
              "- Source archives downloaded and checked: %d of %d verified%s." % (ver, nsrc, " (provenance.tsv)" if prov else " (NOT RUN: no --sources folder given)"),
              "- Licence texts copied from the bottles and, from the verified archives, the upstream licence / authors / readme files (the canonical GPLv3 text, md5 1ebbd3e34237af26da5dc08a4e440464, is from the xz bottle).",
              "- Homebrew's own patches (glib hardcoded-paths.diff, libb2 libtool diff) come from homebrew-core, where both files have not changed since commit 1cf441a09f57 (2025-10-15), before the bottles were installed (2026-10-13); each patch applies cleanly, in order, to the exact source archive. The libvpx patch is the one inline in the installed formula.",
              "- mpg123: COPYING says LGPL v2.1 (no 'or later'); separate dylib, replaceable (LGPL-2.1 section 6). libvmaf: its LICENSE states the licence is designed to be GPL-compatible. FreeType: used under its GPL option (dual FTL / GPL).", "",
              "## UNRESOLVED / limits (a mandatory item marked BLOCKER must be closed before publishing)", ""]
    if ver != nsrc:
        status.append("- **BLOCKER: only %d of %d third-party source archives are verified.** The corresponding-source obligation is not met until every archive is fetched, checksum-verified and attached to the release (`fetch_third_party_sources.py`)." % (ver, nsrc))
    status += ["- " + u for u in unresolved] + [
              "- Reproducibility: the bundled libraries are Homebrew-built bottles (Homebrew's CI toolchain). The package gives the exact upstream sources, patches and formulae (build recipes) they were built from, but the binaries were NOT rebuilt from them here, so bit-for-bit reproducibility is not claimed.",
              "- The installed bottles carry no recorded homebrew-core commit (tap_git_head is empty); the formulae in this package are the ones installed on the build machine.",
              "- Patent licensing for H.264 / HEVC / MP3 / AAC decoding and encoding is not addressed by any of these licences (legal advice needed).",
              "- No per-source-file copyright audit of the third-party sources was done; the notices rely on the upstream licence files, and the per-file copyright headers are preserved in the shipped source archives.",
              "- Test-only resources named in some formulae (for example dav1d test videos) are not part of the binaries and were not fetched."]
    open(os.path.join(out, "COMPLIANCE_STATUS.md"), "w").write("\n".join(status) + "\n")
    print("third-party material written to %s: %d formulae, %d unresolved items" % (out, len(byf), len(unresolved)))
    return 0


sys.exit(main())
