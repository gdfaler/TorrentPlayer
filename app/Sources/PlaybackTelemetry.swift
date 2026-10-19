// PlaybackTelemetry: pure mapping of mpv playback/audio properties into telemetry fields (no AppKit, no mpv types; unit-tested).
// Three different things are reported and must not be confused:
//   * `ahead_s` (existing)  : torrent reader / download FRONTIER. Contiguous cached pieces counted from the piece the mpv reader is at; 0 whenever the reader waits for the next piece.
//   * `mpv_buffer_s` (new)  : mpv's own DEMUXER buffer in seconds (`demuxer-cache-duration`); null when mpv does not report it.
//   * `playing` (new)       : mpv's playback state: started, not paused, not paused for cache, not seeking, core not idle.
import Foundation

enum PlaybackTelemetry {
    /// Seconds of media buffered by mpv's demuxer. A missing, non-finite or negative value is reported as JSON null, never as 0 (0 is a real, different answer).
    static func bufferSeconds(_ raw: Double?) -> Any {
        guard let v = raw, v.isFinite, v >= 0 else { return NSNull() }
        return (v * 1000).rounded() / 1000
    }

    /// True only while mpv is actually advancing playback.
    static func isPlaying(started: Bool, paused: Bool, pausedForCache: Bool, seeking: Bool, coreIdle: Bool) -> Bool {
        started && !paused && !pausedForCache && !seeking && !coreIdle
    }

    /// The four mpv properties captured once per playback initialization, in the order they are requested.
    static let audioProperties = ["current-ao", "audio-device", "audio-params", "audio-out-params"]
    /// Only these keys of `audio-params` / `audio-out-params` are kept (format and channel facts); anything else mpv adds is dropped.
    static let audioParamKeys = ["format", "samplerate", "channel-count", "channels", "hr-channels"]

    /// `audio-device` can carry a hardware identifier (for example a Bluetooth address inside a device UID), so only the kind is recorded:
    /// "auto" (system default), "<backend>/<redacted>" for an explicit device, "<redacted>" otherwise.
    static func redactedDevice(_ raw: String?) -> String? {
        guard let s = raw, !s.isEmpty else { return nil }
        if s == "auto" { return "auto" }
        if let slash = s.firstIndex(of: "/") {
            let backend = String(s[s.startIndex..<slash])
            let safe = backend.range(of: "^[A-Za-z0-9_-]{1,32}$", options: .regularExpression) != nil
            return (safe ? backend : "?") + "/<redacted>"
        }
        return "<redacted>"
    }

    static func sanitizedParams(_ p: [String: Any]?) -> Any {
        guard let p = p else { return NSNull() }
        var out: [String: Any] = [:]
        for k in audioParamKeys {
            switch p[k] {
            case let s as String: out[k] = String(s.prefix(48))
            case let b as Bool: out[k] = b
            case let i as Int: out[k] = i
            case let i as Int64: out[k] = Int(i)
            case let d as Double where d.isFinite: out[k] = d
            default: break                                   // absent, NaN or an unsupported type: left out, not invented
            }
        }
        return out
    }

    /// Fields of the single `audio_info` event. A property that mpv could not provide is null AND named in `unavailable`.
    static func audioInfo(currentAO: String?, audioDevice: String?, audioParams: [String: Any]?, audioOutParams: [String: Any]?) -> [String: Any] {
        var unavailable: [String] = []
        if currentAO == nil || currentAO!.isEmpty { unavailable.append("current-ao") }
        if audioDevice == nil || audioDevice!.isEmpty { unavailable.append("audio-device") }
        if audioParams == nil { unavailable.append("audio-params") }
        if audioOutParams == nil { unavailable.append("audio-out-params") }
        let ao: Any = (currentAO?.isEmpty == false) ? String(currentAO!.prefix(32)) : NSNull()
        let dev: Any = redactedDevice(audioDevice) ?? NSNull()
        let isDefault: Any = (audioDevice?.isEmpty == false) ? (audioDevice == "auto") : NSNull()
        return ["current_ao": ao, "audio_device": dev, "audio_device_is_default": isDefault,
                "audio_params": sanitizedParams(audioParams), "audio_out_params": sanitizedParams(audioOutParams),
                "unavailable": unavailable]
    }
}
