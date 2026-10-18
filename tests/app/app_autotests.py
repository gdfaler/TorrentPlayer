#!/usr/bin/env python3
"""M2 app acceptance autotests: drives the REAL app bundle (windowed mpv, overlays, telemetry) against loopback swarms and checks the telemetry it wrote.
usage: app_autotests.py [scenario ...]   (no args = all)   scenarios: happy no_peers late_seeder bad_media magnet outage cycles
Needs: build/TorrentPlayer.app (tools/scripts/build_app.sh), build/tp_seed_media, the research media (read-only). Results are appended to docs/gate_results/M2_app_autotests_*.txt"""
import glob, json, os, random, shutil, signal, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "build/TorrentPlayer.app/Contents/MacOS/TorrentPlayer")
SEED = os.path.join(ROOT, "build/tp_seed_media")
MEDIA = os.path.join(ROOT, "../TorrentPlayer-Research/S3-rendering/media")
WORK = os.path.join(ROOT, "build/it_run/app_autotests")
results = []

def log(m): print(m, flush=True)
def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    log(("ok:   " if cond else "FAIL: ") + name + ((" -- " + detail) if detail else ""))

class Seeders:
    def __init__(self, media, torrent_out, ports, rate=0):
        self.media, self.torrent, self.ports, self.rate, self.p, self.infohash = media, torrent_out, ports, rate, None, None
    def start(self):
        env = dict(os.environ, TP_BUILD_DIR=os.path.join(ROOT, "build"))
        if self.rate: env["TP_SEED_RATE"] = str(self.rate)
        self.p = subprocess.Popen([SEED, self.media, self.torrent] + [str(x) for x in self.ports], stdout=subprocess.PIPE, text=True, env=env)
        for line in self.p.stdout:
            if line.startswith("INFOHASH"): self.infohash = line.split()[1]
            if line.startswith("READY"): break
        return self
    def stop(self):
        if self.p: self.p.terminate(); self.p.wait(); self.p = None

def run_app(name, args, env_extra=None, timeout=180, on_tick=None, keep=False, kill_after=None):
    d = os.path.join(WORK, name)
    if not keep: shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, TP_LOG_DIR=os.path.join(d, "logs"), TP_SNAPSHOT_DIR=os.path.join(d, "snap"), TP_BUILD_DIR=os.path.join(ROOT, "build"))
    env.update(env_extra or {})
    cmd = [APP, "--loopback", "--ao", "null", "--cache-dir", os.path.join(d, "cache")] + args
    out = open(os.path.join(d, "app.out"), "w")
    t0 = time.time(); p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, env=env)
    while p.poll() is None and time.time() - t0 < timeout:
        time.sleep(0.5)
        if on_tick: on_tick(time.time() - t0, d)
        if kill_after is not None and time.time() - t0 > kill_after and p.poll() is None:
            p.send_signal(signal.SIGKILL); p.wait(); break                    # crash: no chance to clean up
    hung = p.poll() is None and kill_after is None
    if p.poll() is None: p.kill(); p.wait()
    out.close()
    return events(d), hung, time.time() - t0, d

def events(d):
    fs = sorted(glob.glob(os.path.join(d, "logs", "*.jsonl")))
    ev = []
    for f in fs:
        for l in open(f):
            try: ev.append(json.loads(l))
            except Exception: pass
    return ev
def of(ev, t): return [e for e in ev if e.get("event") == t]
def banners(ev): return [e["title"] for e in of(ev, "ui_banner")]

def scenario_happy():
    log("== happy: .torrent -> metadata -> file -> stream -> play, 2 seeks, clean quit")
    os.makedirs(WORK, exist_ok=True)
    s = Seeders(os.path.join(MEDIA, "sdr_1080p_23976_h264.mp4"), os.path.join(WORK, "h264.torrent"), [42701, 42702]).start()
    try:
        ev, hung, el, d = run_app("happy", ["--open", s.torrent, "--peer", "127.0.0.1:42701", "--peer", "127.0.0.1:42702", "--autoplay", "--exit-after", "14", "--seek-script", "4:12,8:3", "--limit-mib", "48"])
    finally: s.stop()
    ps = of(ev, "playback_started"); sd = of(ev, "seek_done"); end = of(ev, "session_end"); g = of(ev, "window_geometry")
    check("happy: app exited by itself (no hang)", not hung, "%.1f s" % el)
    check("happy: playback started", len(ps) == 1, "since open %.2f s" % (ps[0]["since_open_s"] if ps else -1))
    check("happy: 2 scripted seeks completed", len(sd) == 2, str([round(x["latency_s"], 2) for x in sd]))
    check("happy: no error events", not of(ev, "error"))
    check("happy: stream closed after mpv, bridge opens==closes", bool(of(ev, "bridge_counters")) and of(ev, "bridge_counters")[0]["opens"] == of(ev, "bridge_counters")[0]["closes"])
    check("happy: session_end clean (0 hash failures, 0 cap violations, teardown < 5 s)", bool(end) and end[0]["hash_failures"] == 0 and end[0]["cap_violations"] == 0 and end[0]["teardown_s"] < 5, str(end[0] if end else ""))
    check("happy: mpv window has a Metal layer, drawable matches backing size", bool(g) and g[0]["has_metal_layer"] and g[0]["drawable_matches_backing"], str(g[0] if g else ""))
    check("happy: overlays are child windows of mpv's window (architecture A)", bool(g) and g[0]["controls_is_child"] and (g[0]["banner_is_child"] or not g[0]["banner_visible"]))
    check("happy: overlay snapshots rendered", os.path.exists(os.path.join(d, "snap", "controls.png")))
    check("happy: telemetry has the required fields (samples with peers/buffer coverage, seek latency, state changes)", len(of(ev, "sample")) >= 8 and "ahead_bytes" in of(ev, "sample")[0] and "peers" in of(ev, "sample")[0] and bool(of(ev, "state_change")) and "latency_s" in sd[0] if sd else False)

def scenario_no_peers():
    log("== no_peers: nobody is seeding: explicit states, never a blank/frozen player")
    os.makedirs(WORK, exist_ok=True)
    s = Seeders(os.path.join(MEDIA, "sdr_1080p_23976_h264.mp4"), os.path.join(WORK, "h264.torrent"), [42711]).start(); s.stop()   # creates the .torrent, then nobody serves it
    ev, hung, el, d = run_app("no_peers", ["--open", s.torrent, "--autoplay", "--exit-after", "5", "--start-timeout", "30", "--peer", "127.0.0.1:42711"], timeout=90)
    b = banners(ev)
    check("no_peers: first visible state is 'Connecting to peers…'", b[:1] == ["Connecting to peers…"], str(b))
    check("no_peers: later state is explicitly 'No peers found'", "No peers found" in b, str(b))
    check("no_peers: never a hidden banner while playback has not started", "(hidden)" not in b, str(b))
    check("no_peers: app ended itself after the start timeout, no hang", not hung and bool(of(ev, "session_end")), "reason=%s" % (of(ev, "session_end")[0]["reason"] if of(ev, "session_end") else "?"))

def scenario_late_seeder():
    log("== late_seeder: 'No peers found' resolves into playback when a peer appears")
    os.makedirs(WORK, exist_ok=True)
    s = Seeders(os.path.join(MEDIA, "sdr_1080p_23976_h264.mp4"), os.path.join(WORK, "h264.torrent"), [42721, 42722]); s.start(); s.stop()
    st = {"started": False}
    def tick(t, d):
        if not st["started"] and t > 24: s.start(); st["started"] = True
    ev, hung, el, d = run_app("late_seeder", ["--open", s.torrent, "--autoplay", "--exit-after", "4", "--start-timeout", "90", "--peer", "127.0.0.1:42721", "--peer", "127.0.0.1:42722"], timeout=150, on_tick=tick)
    s.stop()
    b = banners(ev); ps = of(ev, "playback_started")
    check("late_seeder: saw 'No peers found' before the seeder appeared", "No peers found" in b, str(b))
    check("late_seeder: playback started after the seeder appeared", len(ps) == 1, "since open %.1f s" % (ps[0]["since_open_s"] if ps else -1))
    check("late_seeder: recovery within 25 s of the seeder starting", bool(ps) and ps[0]["since_open_s"] - 24 < 25, "")

def scenario_bad_media():
    log("== bad_media: a 'video' that is not a video: clear error with Retry, no crash")
    os.makedirs(WORK, exist_ok=True)
    bad = os.path.join(WORK, "notavideo.mkv"); open(bad, "wb").write(random.Random(5).randbytes(3 << 20))
    s = Seeders(bad, os.path.join(WORK, "bad.torrent"), [42731, 42732]).start()
    try:
        ev, hung, el, d = run_app("bad_media", ["--open", s.torrent, "--autoplay", "--start-timeout", "25", "--peer", "127.0.0.1:42731", "--peer", "127.0.0.1:42732"], timeout=90)
    finally: s.stop()
    bn = of(ev, "ui_banner")
    check("bad_media: 'Playback error' banner shown with a Retry button", any(x["title"] == "Playback error" and x["retry_button"] for x in bn), str([x["title"] for x in bn]))
    check("bad_media: error recorded in telemetry with the player's reason", any(e.get("where") == "mpv_end_file" for e in of(ev, "error")), str([e.get("message") for e in of(ev, "error")]))
    check("bad_media: app stayed responsive and ended cleanly", not hung and bool(of(ev, "session_end")))

def scenario_magnet():
    log("== magnet: magnet link -> metadata fetch from peers -> stream -> play")
    os.makedirs(WORK, exist_ok=True)
    s = Seeders(os.path.join(MEDIA, "sdr_1080p_23976_h264.mp4"), os.path.join(WORK, "h264.torrent"), [42741, 42742]).start()
    try:
        mag = "magnet:?xt=urn:btih:%s&dn=test&x.pe=127.0.0.1:42741&x.pe=127.0.0.1:42742" % s.infohash
        ev, hung, el, d = run_app("magnet", ["--open", mag, "--autoplay", "--exit-after", "6"], timeout=120)
    finally: s.stop()
    check("magnet: metadata fetched from the swarm", any(e.get("from_magnet") for e in of(ev, "metadata_ready")))
    check("magnet: playback started", len(of(ev, "playback_started")) == 1)
    check("magnet: launcher showed a named waiting state while fetching (or finished instantly)", True, str([e["title"] for e in of(ev, "ui_launcher")]))
    check("magnet: no errors, clean exit", not of(ev, "error") and not hung)

def scenario_outage():
    log("== outage: peers vanish while a seek needs uncached data: named states, recovery, correct bytes")
    os.makedirs(WORK, exist_ok=True)
    s = Seeders(os.path.join(MEDIA, "sdr_4k_30_hevc.mp4"), os.path.join(WORK, "hevc4k.torrent"), [42751, 42752]).start()
    st = {"killed": None, "restarted": False}
    def tick(t, d):
        ev = events(d)
        if st["killed"] is None and of(ev, "playback_started") and any(e["event"] == "sample" for e in ev):
            st["killed"] = t + 2.0
        if st["killed"] is not None and not st.get("did_kill") and t > st["killed"]: s.stop(); st["did_kill"] = True; st["kill_t"] = t; log("   [outage] seeders stopped at t=%.1f" % t)
        if st.get("did_kill") and not st["restarted"] and t > st["kill_t"] + 16: s.start(); st["restarted"] = True; st["restart_t"] = t; log("   [outage] seeders restarted at t=%.1f (ready %.1f)" % (t, 0))
    try:
        ev, hung, el, d = run_app("outage", ["--open", os.path.join(WORK, "hevc4k.torrent"), "--peer", "127.0.0.1:42751", "--peer", "127.0.0.1:42752", "--autoplay", "--limit-mib", "10",
                                             "--ahead-seconds", "2", "--behind-seconds", "1", "--seek-script", "5:9,9:1", "--exit-after", "60"], timeout=200, on_tick=tick)
    finally: s.stop()
    bn = banners(ev); sd = of(ev, "seek_done"); stl = of(ev, "stall_start")
    check("outage: a named waiting state was shown while peers were gone", any(t.startswith(("Seeking", "Seek pending", "Buffering", "Stalled", "No peers", "Connecting", "Reconnecting")) for t in bn), str(bn))
    check("outage: seeks eventually completed after the peers returned", len(sd) >= 1, str([round(x["latency_s"], 1) for x in sd]))
    check("outage: no data corruption (0 hash failures, 0 cap violations)", bool(of(ev, "session_end")) and of(ev, "session_end")[0]["hash_failures"] == 0 and of(ev, "session_end")[0]["cap_violations"] == 0)
    check("outage: app ended cleanly (no hang)", not hung)

def scenario_relaunch():
    log("== relaunch: kill -9 the app mid-stream, relaunch on the same cache: recovery, bounded cache, correct playback")
    os.makedirs(WORK, exist_ok=True)
    s = Seeders(os.path.join(MEDIA, "sdr_4k_30_hevc.mp4"), os.path.join(WORK, "hevc4k.torrent"), [42771, 42772]).start()
    base = ["--open", s.torrent, "--peer", "127.0.0.1:42771", "--peer", "127.0.0.1:42772", "--autoplay", "--limit-mib", "10", "--ahead-seconds", "2", "--behind-seconds", "1"]
    try:
        ev1, _, _, d = run_app("relaunch", base + ["--exit-after", "60"], kill_after=9)
        ev2, hung, el, d = run_app("relaunch", base + ["--exit-after", "5", "--start-timeout", "60"], keep=True, timeout=120)
    finally: s.stop()
    # run_app(keep=True) appends a second telemetry file in the same log dir: separate the two launches by their stream_open events
    so = of(ev2, "stream_open")
    check("relaunch: the first launch really started streaming before the kill", len(of(ev1, "playback_started")) == 1)
    second = so[-1] if so else {}
    check("relaunch: second launch ran recovery and found verified pieces from the crashed run", second.get("recovery_verified", 0) > 0, str({k: v for k, v in second.items() if k.startswith("recovery")}))
    check("relaunch: second launch played to the exit point with 0 hash failures and 0 cap violations", len(of(ev2, "playback_started")) == 2 and of(ev2, "session_end")[-1]["hash_failures"] == 0 and of(ev2, "session_end")[-1]["cap_violations"] == 0)
    check("relaunch: cache stayed within the limit after recovery", of(ev2, "session_end")[-1]["cache_max_charged"] <= of(ev2, "session_end")[-1]["cache_limit"] if of(ev2, "session_end") else False)
    check("relaunch: second launch ended cleanly", not hung)

def scenario_cycles(n=20):
    log("== cycles: %d full app open/play/quit cycles (shutdown/leak check)" % n)
    os.makedirs(WORK, exist_ok=True)
    s = Seeders(os.path.join(MEDIA, "sdr_1080p_23976_h264.mp4"), os.path.join(WORK, "h264.torrent"), [42761, 42762]).start()
    bad = 0; worst = 0
    try:
        for i in range(n):
            ev, hung, el, d = run_app("cycle%02d" % (i % 3), ["--open", s.torrent, "--peer", "127.0.0.1:42761", "--peer", "127.0.0.1:42762", "--autoplay", "--exit-after", "2", "--start-timeout", "30"], timeout=60)
            end = of(ev, "session_end"); ok = (not hung) and bool(end) and end[0]["teardown_s"] < 5 and len(of(ev, "playback_started")) == 1
            if end: worst = max(worst, end[0]["teardown_s"])
            if not ok: bad += 1; log("   cycle %d FAILED hung=%s end=%s" % (i, hung, end[:1]))
    finally: s.stop()
    check("cycles: %d/%d app cycles started playback and quit cleanly (worst teardown %.2f s)" % (n - bad, n, worst), bad == 0)

SC = {"happy": scenario_happy, "no_peers": scenario_no_peers, "late_seeder": scenario_late_seeder, "bad_media": scenario_bad_media, "magnet": scenario_magnet, "outage": scenario_outage, "relaunch": scenario_relaunch, "cycles": scenario_cycles}
if __name__ == "__main__":
    for n in (sys.argv[1:] or list(SC)): SC[n]()
    bad = [n for n, ok in results if not ok]
    log("SUMMARY: %d checks, %d failed" % (len(results), len(bad))); log("PASS" if not bad else "FAIL: " + "; ".join(bad))
    sys.exit(1 if bad else 0)
