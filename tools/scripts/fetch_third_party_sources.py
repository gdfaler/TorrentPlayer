#!/usr/bin/env python3
"""Download and verify the corresponding third-party source for ONE TorrentPlayer release.

Input: source-manifest.tsv (written by gen_third_party.py from the libraries that were actually bundled). Needs network access; nothing else in the build uses it.
For every manifest row it fetches the exact source archive named there (never a newer one), checks its sha256 against the value recorded from the Homebrew formula, fetches every
patch the formula applies (URL patches are checked against the formula's sha256; Homebrew's own Patches/*.diff files are fetched from homebrew-core and are checked by a dry-run
`patch` against the exact source archive), and records provenance. Git-pinned sources are cloned and checked out at the exact revision, which is verified.
Output (TARGET): archives/, patches/, provenance.tsv, FETCH_REPORT.md.   Exit status 0 only if every source and patch verified.

Usage: fetch_third_party_sources.py --manifest source-manifest.tsv --formulae DIR --out TARGET
"""
import argparse, csv, datetime, hashlib, json, os, re, shutil, subprocess, sys, tarfile, tempfile, urllib.parse

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()

def curl(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:           # already fetched by an earlier run: keep it (its checksum is verified again below)
        return True, ""
    r = subprocess.run(["curl", "-fsSL", "--retry", "3", "--max-time", "600", "-o", dest, url], capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip()

def basename(url):
    return os.path.basename(urllib.parse.urlparse(url).path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--formulae", required=True, help="folder with the formula .rb files (for the inline patches)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    arch_dir, patch_dir = os.path.join(a.out, "archives"), os.path.join(a.out, "patches")
    os.makedirs(arch_dir, exist_ok=True); os.makedirs(patch_dir, exist_ok=True)
    prov, problems = [], []
    rows = list(csv.DictReader(open(a.manifest), delimiter="\t"))
    for r in rows:
        f, v, url, sha, rev = r["formula"], r["version"], r["source_url"], r["source_sha256"], r["git_revision"]
        key = "%s-%s" % (f.replace("@", "-"), v)
        status, got, fn, size = "FAILED", "", "", 0
        if rev:                                                         # git-pinned source
            work = os.path.join(a.out, "git-" + key)
            if not os.path.isdir(work):
                subprocess.run(["git", "clone", "--quiet", url, work], check=False)
            subprocess.run(["git", "-C", work, "checkout", "--quiet", rev], check=False)
            head = subprocess.run(["git", "-C", work, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            if head == rev:
                fn = key + "-git-" + rev[:12] + ".tar.gz"
                subprocess.run(["git", "-C", work, "archive", "--format=tar.gz", "--prefix=%s/" % key, "-o", os.path.abspath(os.path.join(arch_dir, fn)), "HEAD"], check=True)
                got, size, status = sha256(os.path.join(arch_dir, fn)), os.path.getsize(os.path.join(arch_dir, fn)), "VERIFIED git revision " + rev
                shutil.rmtree(work, ignore_errors=True)
            else:
                status = "FAILED: checked-out revision %s != %s" % (head, rev)
        else:
            fn = basename(url)
            if (f == "luajit" or "/archive/" in url) and not fn.endswith((".tar.gz", ".tar.bz2", ".tar.xz", ".tgz")):
                fn = key + ".tar.gz"
            fn = key + "__" + fn if not fn.startswith(f.split("@")[0]) else fn
            dest = os.path.join(arch_dir, fn)
            ok, err = curl(url, dest)
            if ok:
                got, size = sha256(dest), os.path.getsize(dest)
                status = "VERIFIED sha256" if got == sha else "FAILED: sha256 %s != expected %s" % (got, sha)
            else:
                status = "FAILED: download (%s)" % err[:120]
        if not status.startswith("VERIFIED"):
            problems.append("%s %s: %s" % (f, v, status))
        prov.append((f, v, url, fn, size, got, sha or ("git " + rev), status))

        # patches
        for p in [x.strip() for x in (r["patches"] or "").split("; ") if x.strip()]:
            if p.startswith("http"):
                m = re.match(r"(\S+)(?: sha256=([0-9a-f]{64}))?", p)
                purl, psha = m.group(1), m.group(2)
                pfn = "%s__%s.patch" % (key, re.sub(r"[^A-Za-z0-9]+", "_", purl.split("/commit/")[-1].split("?")[0]) if "/commit/" in purl else basename(purl))
                ok, err = curl(purl, os.path.join(patch_dir, pfn))
                pg = sha256(os.path.join(patch_dir, pfn)) if ok else ""
                ps = "VERIFIED sha256" if ok and pg == psha else ("FAILED: sha256 %s != %s" % (pg, psha) if ok else "FAILED: download")
                prov.append((f, v, purl, "patches/" + pfn, os.path.getsize(os.path.join(patch_dir, pfn)) if ok else 0, pg, psha or "", ps))
                if not ps.startswith("VERIFIED"): problems.append("%s patch %s: %s" % (f, purl, ps))
            elif p.startswith("Patches/"):                              # Homebrew's own patch file (in the homebrew-core repository)
                purl = "https://raw.githubusercontent.com/Homebrew/homebrew-core/main/" + p
                pfn = "%s__%s" % (key, p.replace("/", "_"))
                ok, err = curl(purl, os.path.join(patch_dir, pfn))
                last = ""
                api = subprocess.run(["curl", "-fsSL", "--max-time", "60", "https://api.github.com/repos/Homebrew/homebrew-core/commits?per_page=1&path=" + p], capture_output=True, text=True)
                try:
                    c = json.loads(api.stdout)[0]
                    last = "; last changed in homebrew-core commit %s on %s" % (c["sha"][:12], c["commit"]["committer"]["date"][:10])
                except Exception:
                    last = "; last-change commit could not be determined"
                prov.append((f, v, purl, "patches/" + pfn, os.path.getsize(os.path.join(patch_dir, pfn)) if ok else 0, sha256(os.path.join(patch_dir, pfn)) if ok else "", "none published: verified by applying it to the source", ("DOWNLOADED from homebrew-core main" + last) if ok else "FAILED: download"))
                if not ok: problems.append("%s patch %s: download failed" % (f, p))
            elif p.startswith("patch :DATA"):                           # inline in the formula after __END__
                rb = os.path.join(a.formulae, f.replace("@", "-") + ".rb")
                txt = open(rb).read().split("\n__END__\n", 1)[1] if os.path.exists(rb) and "\n__END__\n" in open(rb).read() else ""
                pfn = "%s__inline.patch" % key
                open(os.path.join(patch_dir, pfn), "w").write(txt)
                prov.append((f, v, "formula " + os.path.basename(rb), "patches/" + pfn, len(txt), hashlib.sha256(txt.encode()).hexdigest(), "inline in the formula file", "EXTRACTED from the installed formula" if txt else "FAILED: no __END__ block"))
                if not txt: problems.append("%s inline patch missing" % f)

    # apply each formula's patches IN ORDER (they build on each other) to a scratch copy of the exact source archive; a patch that does not apply is a problem
    for f, v in sorted({(p[0], p[1]) for p in prov if p[3].startswith("patches/")}):
        src = next((x for x in prov if x[0] == f and x[1] == v and not x[3].startswith("patches/")), None)
        if not src or "FAILED" in src[7]:
            continue
        tmp = tempfile.mkdtemp()
        try:
            with tarfile.open(os.path.join(arch_dir, src[3])) as t:
                t.extractall(tmp)
            top = os.path.join(tmp, os.listdir(tmp)[0])
            for i, p in enumerate(prov):
                if p[0] != f or p[1] != v or not p[3].startswith("patches/") or "FAILED" in p[7]:
                    continue
                ok = False
                for strip in ("1", "0"):
                    res = subprocess.run(["patch", "-p" + strip, "-s", "-d", top, "-i", os.path.abspath(os.path.join(a.out, p[3]))], capture_output=True, text=True)
                    if res.returncode == 0:
                        ok = True
                        break
                prov[i] = p[:7] + (p[7] + ("; applies cleanly (in order) to the source archive" if ok else "; DOES NOT APPLY"),)
                if not ok:
                    problems.append("%s patch %s does not apply to the source archive" % (f, p[3]))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    with open(os.path.join(a.out, "provenance.tsv"), "w") as w:
        w.write("formula\tversion\tupstream_url\tfile\tbytes\tsha256_computed\tsha256_expected\tstatus\n")
        for p in prov:
            w.write("\t".join(str(x) for x in p) + "\n")
    srcs = [p for p in prov if not p[3].startswith("patches/")]
    with open(os.path.join(a.out, "FETCH_REPORT.md"), "w") as w:
        w.write("# Third-party source fetch report\n\nFetched %s\n\n- Sources: %d, verified: %d\n- Patches: %d\n- Total archive bytes: %d\n\n" % (datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
                len(srcs), sum(1 for p in srcs if p[7].startswith("VERIFIED")), len(prov) - len(srcs), sum(p[4] for p in prov)))
        w.write("## Problems\n\n" + ("\n".join("- " + x for x in problems) if problems else "none") + "\n")
    print("sources %d verified %d patches %d problems %d" % (len(srcs), sum(1 for p in srcs if p[7].startswith("VERIFIED")), len(prov) - len(srcs), len(problems)))
    for x in problems:
        print("PROBLEM:", x)
    return 0 if not problems else 1

sys.exit(main())
