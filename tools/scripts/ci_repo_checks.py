#!/usr/bin/env python3
"""Repository hygiene, packaging-consistency and workflow-security checks for pull request CI. Standard library only; needs a git checkout.

Usage: tools/scripts/ci_repo_checks.py [--commit-range BASE..HEAD]
Every failed assertion is printed and the exit status is 1; 0 means every check passed.

The protected-file check uses tools/ci/protected_tracked_files.sha256, the tracked subset of the hash-locked files (the full manifest and some of its
files live outside Git). If the full manifest docs/proposals/G3_CHURN_F_HASHES.txt exists in the working directory it is verified too.
"""
import argparse, hashlib, os, plistlib, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(ROOT)
failures, notes = [], []


def fail(check, msg):
    failures.append(f"[{check}] {msg}")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


tracked = [p for p in git("ls-files", "-z").split("\0") if p]
SELF = "tools/scripts/ci_repo_checks.py"


def text_of(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    return None if b"\0" in data[:8000] else data.decode("utf-8", "replace")


# ---------------------------------------------------------------- required files and licence
def check_required():
    required = ["LICENSE", "README.md", "CMakeLists.txt", "docs/RELEASE_NOTES.md", "docs/DISTRIBUTION_NOTES.md", "app/Resources/Info.plist",
                "tools/scripts/build_release.sh", "tools/scripts/package_release.sh", "tools/scripts/gen_third_party.py",
                "tools/scripts/fetch_third_party_sources.py", "tools/ci/protected_tracked_files.sha256"]
    for p in required:
        if p not in tracked:
            fail("required-files", f"missing or untracked: {p}")
    lic = text_of("LICENSE") or ""
    for needle in ("GNU GENERAL PUBLIC LICENSE", "Version 3, 29 June 2007", "END OF TERMS AND CONDITIONS"):
        if needle not in lic:
            fail("license", f"LICENSE does not contain {needle!r}: it must be the GPL version 3 text")
    readme = text_of("README.md") or ""
    if "GPL" not in readme and "General Public License" not in readme:
        fail("license", "README.md does not state the licence")


# ---------------------------------------------------------------- tracked artefacts and private material
FORBIDDEN_PATHS = [
    (r"^(dist|build|ci_build)/", "build output or release folder"),
    (r"(^|/)" + "CLA" + "UDE" + r"\.md$", "local session context file"),
    (r"^docs/ai_" + "context/", "local session context folder"),
    (r"^docs/(gate_results|proposals)/", "local evidence/proposal folder (ignored by design)"),
    (r"\.(zip|tar|tgz|dmg|pkg|ips|jsonl|sparseimage|log)$|\.tar\.(gz|bz2|xz)$", "archive, crash report, telemetry or log"),
    (r"\.(dylib|a|o|so)$|\.app/", "compiled binary or app bundle content"),
    (r"(^|/)(SHA256SUMS|BUILD_METADATA\.json|provenance\.tsv|source-manifest\.tsv)$", "release asset"),
    (r"^TorrentPlayer-[0-9].*", "release asset name"),
    (r"(^|/)session_backup/", "local session backup"),
    (r"(^|/)(\.env|id_rsa|id_ed25519)$|\.(pem|p12|key)$", "credential-like file"),
]
MAX_FILE_BYTES = 5 * 1024 * 1024


def check_tracked_files():
    for p in tracked:
        for pattern, why in FORBIDDEN_PATHS:
            if re.search(pattern, p):
                fail("tracked-files", f"{p}: {why}")
        try:
            if os.path.getsize(p) > MAX_FILE_BYTES:
                fail("tracked-files", f"{p}: larger than {MAX_FILE_BYTES // 1048576} MiB")
        except OSError:
            pass


SECRET = re.compile(r"AKIA[0-9A-Z]{16}|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_\w{20,}|xox[baprs]-[\w-]{10,}|-----BEGIN [A-Z ]*PRIVATE KEY-----|sk-[A-Za-z0-9]{24,}")
USERPATH = re.compile(r"/Users/(?!x\b|Shared\b|you\b|name\b|user\b)[A-Za-z0-9_.-]+|/private/var/folders/|/home/[a-z][a-z0-9_-]+/")
IPV4 = re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
TOOLWORDS = re.compile("|".join(["cla" + "ude", "anthr" + "opic", "open" + "ai", "chat" + "gpt", "copi" + "lot"]), re.I)


def check_private_content():
    for p in tracked:
        if p == SELF:
            continue
        t = text_of(p)
        if t is None:
            continue
        in_tests = p.startswith("tests/")
        for lineno, line in enumerate(t.splitlines(), 1):
            if SECRET.search(line):
                fail("private-content", f"{p}:{lineno}: looks like a credential")
            if USERPATH.search(line):
                fail("private-content", f"{p}:{lineno}: machine-specific path")
            if TOOLWORDS.search(line):
                fail("private-content", f"{p}:{lineno}: names an assistant tool or vendor")
            for e in EMAIL.findall(line):
                if not e.endswith("@users.noreply.github.com"):
                    fail("private-content", f"{p}:{lineno}: e-mail address {e}")
            if not in_tests:                                                    # synthetic fixture addresses live in tests/
                for ip in IPV4.findall(line):
                    octs = [int(x) for x in ip.split(".")]
                    if all(o <= 255 for o in octs) and octs[0] != 127 and ip != "0.0.0.0":
                        fail("private-content", f"{p}:{lineno}: IPv4 address {ip} outside tests/")


# ---------------------------------------------------------------- protected files
def verify_manifest(manifest, label):
    n = 0
    for line in open(manifest, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line.strip():
            continue
        m = re.match(r"^([0-9a-f]{64})  (.+)$", line)
        if not m:
            fail("protected-files", f"{label}: malformed line {line!r}")
            continue
        digest, path = m.groups()
        n += 1
        if not os.path.exists(path):
            if label == "tracked baseline":
                fail("protected-files", f"{path}: missing")
            else:
                notes.append(f"{label}: {path} not present in this checkout (not tracked); skipped")
        elif sha256(path) != digest:
            fail("protected-files", f"{path}: hash differs from {label} (hash-locked file changed)")
    return n


def check_protected():
    n = verify_manifest("tools/ci/protected_tracked_files.sha256", "tracked baseline")
    if n == 0:
        fail("protected-files", "tracked baseline lists no files")
    full = "docs/proposals/G3_CHURN_F_HASHES.txt"
    if os.path.exists(full):
        verify_manifest(full, "full manifest")
    else:
        notes.append("full 22-file manifest is not part of the repository: only the tracked subset was verified")


# ---------------------------------------------------------------- packaging / scripts consistency
def check_scripts_and_versions():
    for p in tracked:
        if p.startswith("tools/scripts/") and p.endswith(".sh"):
            r = subprocess.run(["bash", "-n", p], capture_output=True, text=True)
            if r.returncode:
                fail("shell-syntax", f"{p}: {r.stderr.strip()}")
        if p.endswith(".py") and (p.startswith("tools/") or p.startswith("tests/")):
            try:
                compile(open(p, encoding="utf-8").read(), p, "exec")
            except SyntaxError as e:
                fail("python-syntax", f"{p}: {e}")
    try:
        plist = plistlib.load(open("app/Resources/Info.plist", "rb"))
        version = plist.get("CFBundleShortVersionString", "")
        notes_text = open("docs/RELEASE_NOTES.md", encoding="utf-8").read()
        m = re.match(r"#\s+TorrentPlayer\s+(\S+)", notes_text)
        if not version or not m or m.group(1) != version:
            fail("versions", f"Info.plist version {version!r} does not match the docs/RELEASE_NOTES.md heading {m.group(1) if m else None!r}")
        if plist.get("LSMinimumSystemVersion") != "27.0":
            fail("versions", f"LSMinimumSystemVersion is {plist.get('LSMinimumSystemVersion')!r}, expected 27.0")
        if "GPL-3.0-or-later" not in open("tools/scripts/package_release.sh", encoding="utf-8").read():
            fail("packaging", "package_release.sh does not record the GPL-3.0-or-later licence")
    except (OSError, plistlib.InvalidFileException) as e:
        fail("versions", f"cannot read version information: {e}")
    r = subprocess.run([sys.executable, "tests/policy/test_hash_policy.py"], capture_output=True, text=True)
    if r.returncode:
        fail("policy-test", "tests/policy/test_hash_policy.py failed:\n" + r.stdout[-1500:] + r.stderr[-500:])


# ---------------------------------------------------------------- workflow security
def check_workflows():
    wf = [p for p in tracked if p.startswith(".github/workflows/") and p.endswith((".yml", ".yaml"))]
    if not wf:
        fail("workflows", "no workflow files found")
    for p in wf:
        t = open(p, encoding="utf-8").read()
        if re.search(r"^\s*pull_request_target\s*:", t, re.M) or "pull_request_target" in re.sub(r"#.*", "", t):
            fail("workflows", f"{p}: uses pull_request_target")
        if not re.search(r"^permissions:\s*$", t, re.M) and not re.search(r"^permissions:\s*\S", t, re.M):
            fail("workflows", f"{p}: no top-level permissions block")
        if re.search(r"^\s*permissions:\s*write-all", t, re.M):
            fail("workflows", f"{p}: write-all permissions")
        if "timeout-minutes" not in t:
            fail("workflows", f"{p}: jobs have no timeout-minutes")
        for m in re.finditer(r"^\s*-?\s*uses:\s*(\S+)", t, re.M):
            ref = m.group(1)
            if ref.startswith("./"):
                continue
            if not re.search(r"@[0-9a-f]{40}$", ref):
                fail("workflows", f"{p}: action not pinned to a full commit SHA: {ref}")
        if "${{ github.event.pull_request.title" in t or "${{ github.head_ref" in t:
            fail("workflows", f"{p}: untrusted pull request text interpolated into a workflow")


# ---------------------------------------------------------------- commit messages
def check_commits(commit_range):
    if not commit_range:
        notes.append("no commit range given: commit messages not checked")
        return
    msgs = git("log", "--format=%H%x1f%B%x1e", commit_range)
    for entry in msgs.split("\x1e"):
        if not entry.strip():
            continue
        sha, _, body = entry.strip().partition("\x1f")
        for line in body.splitlines():
            if re.match(r"(?i)co-authored-by:", line) and TOOLWORDS.search(line):
                fail("commits", f"{sha[:10]}: co-author trailer naming an assistant tool")
            elif TOOLWORDS.search(line):
                fail("commits", f"{sha[:10]}: commit message names an assistant tool or vendor")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--commit-range", default="")
    a = ap.parse_args()
    for fn in (check_required, check_tracked_files, check_private_content, check_protected, check_scripts_and_versions, check_workflows):
        fn()
    check_commits(a.commit_range)
    for n in notes:
        print("note:", n)
    if failures:
        print(f"\n{len(failures)} check(s) FAILED:")
        for f in failures:
            print(" -", f)
        return 1
    print(f"all repository checks passed ({len(tracked)} tracked files)")
    return 0


sys.exit(main())
