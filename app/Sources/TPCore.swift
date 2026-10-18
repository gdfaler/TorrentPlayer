// TPCore.swift: Swift wrappers over the narrow C interface (tp_c.h). No C++ types are visible here.
import Foundation

struct TPError: Error, CustomStringConvertible { let description: String; init(_ s: String) { description = s } }

func cString(_ tuple: UnsafePointer<CChar>) -> String { String(cString: tuple) }

enum VideoFiles {
    static let exts: Set<String> = ["mkv", "mp4", "m4v", "avi", "webm", "mov", "ts", "m2ts", "mts", "wmv", "flv", "mpg", "mpeg", "ogv", "vob"]
}

final class TPEngine {
    let ptr: OpaquePointer
    let cacheRoot: String
    init(cacheRoot: String, evidenceDir: String?, listen: String?, loopback: Bool) throws {
        self.cacheRoot = cacheRoot
        var err = [CChar](repeating: 0, count: 256)
        var p: OpaquePointer?
        func make(_ listenPtr: UnsafePointer<CChar>?, _ evPtr: UnsafePointer<CChar>?) {
            cacheRoot.withCString { cr in
                var cfg = tp_engine_config(cache_root: cr, evidence_dir: evPtr, listen_interfaces: listenPtr, loopback_test_mode: loopback ? 1 : 0, enable_dht: loopback ? 0 : 1)
                p = tp_engine_create(&cfg, &err, err.count)
            }
        }
        func withOpt(_ s: String?, _ body: (UnsafePointer<CChar>?) -> Void) { if let s = s { s.withCString { body($0) } } else { body(nil) } }
        withOpt(listen) { l in withOpt(evidenceDir) { e in make(l, e) } }
        guard let engine = p else { throw TPError(String(cString: err)) }
        ptr = engine
    }
    deinit { tp_engine_destroy(ptr) }
}

final class TPMeta {
    struct FileEntry { let index: Int; let path: String; let size: Int64; let isVideo: Bool }
    enum State: Int { case fetching = 0, ready = 1, failed = 2 }
    struct Status { let state: State; let peers: Int; let elapsed: Double; let error: String }
    let ptr: OpaquePointer
    let fromMagnet: Bool
    let sourceDescription: String

    init(engine: TPEngine, file path: String) throws {
        var err = [CChar](repeating: 0, count: 256)
        guard let p = tp_meta_from_file(engine.ptr, path, &err, err.count) else { throw TPError(String(cString: err)) }
        ptr = p; fromMagnet = false; sourceDescription = (path as NSString).lastPathComponent
    }
    init(engine: TPEngine, magnet: String) throws {
        var err = [CChar](repeating: 0, count: 256)
        guard let p = tp_meta_from_magnet(engine.ptr, magnet, &err, err.count) else { throw TPError(String(cString: err)) }
        ptr = p; fromMagnet = true; sourceDescription = "magnet link"
    }
    deinit { tp_meta_free(ptr) }

    func status() -> Status {
        var st = tp_meta_status()
        tp_meta_status_get(ptr, &st)
        let err = withUnsafePointer(to: &st.error) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }
        return Status(state: State(rawValue: Int(st.state)) ?? .failed, peers: Int(st.peers), elapsed: st.elapsed_s, error: err)
    }
    func cancel() { tp_meta_cancel(ptr) }
    func addPeer(_ s: String) { tp_meta_add_peer(ptr, s) }
    var name: String { var b = [CChar](repeating: 0, count: 512); return tp_meta_name(ptr, &b, b.count) == 1 ? String(cString: b) : "" }
    var defaultVideo: Int { Int(tp_meta_default_video(ptr)) }
    func files() -> [FileEntry] {
        var out: [FileEntry] = []
        for i in 0..<Int(tp_meta_file_count(ptr)) {
            var path = [CChar](repeating: 0, count: 1024); var size: Int64 = 0; var isv: Int32 = 0
            if tp_meta_file_info(ptr, Int32(i), &path, path.count, &size, &isv) == 1 { out.append(FileEntry(index: i, path: String(cString: path), size: size, isVideo: isv != 0)) }
        }
        return out
    }
}

struct StreamStatus {
    var raw: tp_stream_status
    var state: Int { Int(raw.state) }
    var stateName: String { withUnsafePointer(to: raw.state_text) { $0.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) } } }
    var webSeedLastError: String { withUnsafePointer(to: raw.web_seed_last_error) { $0.withMemoryRebound(to: CChar.self, capacity: 96) { String(cString: $0) } } }
}

final class TPStream {
    let ptr: OpaquePointer
    private(set) var closed = false
    init(engine: TPEngine, meta: TPMeta, fileIndex: Int, cacheLimit: Int64, behindSeconds: Double, aheadSeconds: Double, webSeedMode: Int) throws {
        var err = [CChar](repeating: 0, count: 256)
        var o = tp_stream_options(cache_limit_bytes: cacheLimit, behind_seconds: behindSeconds, ahead_seconds: aheadSeconds, web_seed_mode: Int32(webSeedMode))
        guard let p = tp_stream_open(engine.ptr, meta.ptr, Int32(fileIndex), &o, &err, err.count) else { throw TPError(String(cString: err)) }
        ptr = p
    }
    var size: Int64 { tp_stream_size(ptr) }
    var fileName: String { var b = [CChar](repeating: 0, count: 1024); _ = tp_stream_file_name(ptr, &b, b.count); return String(cString: b) }
    func status() -> StreamStatus { var s = tp_stream_status(); tp_stream_status_get(ptr, &s); return StreamStatus(raw: s) }
    func coverage(_ n: Int) -> [UInt8] { var out = [UInt8](repeating: 0, count: n); tp_stream_coverage(ptr, Int32(n), &out); return out }
    func seekHint(_ off: Int64) { tp_stream_seek_hint(ptr, off) }
    func setBitrate(_ bps: Double) { tp_stream_set_bitrate(ptr, bps) }
    func events() -> String { var b = [CChar](repeating: 0, count: 16384); _ = tp_stream_events(ptr, &b, b.count); return String(cString: b) }
    func lastHashReport() -> String { var b = [CChar](repeating: 0, count: 2048); _ = tp_stream_last_hash_report(ptr, &b, b.count); return String(cString: b) }
    /// Must be called after the mpv handle has been destroyed.
    func close() { if !closed { closed = true; tp_stream_close(ptr) } }
    deinit { close() }
}
