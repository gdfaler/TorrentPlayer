// Telemetry.swift: one JSONL file per playback session (startup, seeks, stalls, peers, buffer coverage). Local only; nothing is sent anywhere.
import Foundation

final class Telemetry {
    let url: URL
    private let q = DispatchQueue(label: "tp.telemetry")
    private var fh: FileHandle?
    private let t0 = DispatchTime.now().uptimeNanoseconds
    private let iso = ISO8601DateFormatter()

    static func logDirectory() -> URL {
        if let d = ProcessInfo.processInfo.environment["TP_LOG_DIR"] { return URL(fileURLWithPath: d, isDirectory: true) }
        return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/TorrentPlayer", isDirectory: true)
    }

    init() {
        let dir = Telemetry.logDirectory()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
        url = dir.appendingPathComponent("telemetry-\(f.string(from: Date()))-\(ProcessInfo.processInfo.processIdentifier).jsonl")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        fh = try? FileHandle(forWritingTo: url)
    }

    /// seconds since this telemetry object was created (monotonic)
    var now: Double { Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9 }

    func emit(_ type: String, _ fields: [String: Any] = [:]) {
        var d = fields
        d["event"] = type
        d["t"] = (now * 1000).rounded() / 1000
        d["wall"] = iso.string(from: Date())
        q.async { [weak self] in
            guard let self = self, let fh = self.fh, JSONSerialization.isValidJSONObject(d),
                  var data = try? JSONSerialization.data(withJSONObject: d, options: [.sortedKeys]) else { return }
            data.append(0x0A)
            fh.write(data)
        }
    }

    func close() { q.sync { try? fh?.close(); fh = nil } }
}
