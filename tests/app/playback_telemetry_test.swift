// Offline test for PlaybackTelemetry (buffer/playing fields and audio_info mapping). No mpv, no UI, no network.
// Build and run:  swiftc -parse-as-library -swift-version 5 -target arm64-apple-macosx27.0 app/Sources/PlaybackTelemetry.swift tests/app/playback_telemetry_test.swift -o dist/<dir>/playback_telemetry_test && dist/<dir>/playback_telemetry_test
import Foundation

@main struct PlaybackTelemetryTest {
    static var failures = 0, checks = 0
    static func check(_ c: Bool, _ what: String, line: Int = #line) { checks += 1; if !c { failures += 1; print("FAILED (line \(line)): \(what)") } }
    static func isNull(_ v: Any?) -> Bool { v is NSNull }

    static func main() {
        // mpv_buffer_s: null when unavailable, never 0; a real 0 stays 0
        check(isNull(PlaybackTelemetry.bufferSeconds(nil)), "nil -> null")
        check(isNull(PlaybackTelemetry.bufferSeconds(.nan)), "NaN -> null")
        check(isNull(PlaybackTelemetry.bufferSeconds(.infinity)), "inf -> null")
        check(isNull(PlaybackTelemetry.bufferSeconds(-1)), "negative -> null")
        check((PlaybackTelemetry.bufferSeconds(0) as? Double) == 0, "a real 0 is reported as 0, not null")
        check((PlaybackTelemetry.bufferSeconds(12.3456) as? Double) == 12.346, "rounded to milliseconds")

        // playing: every condition must hold
        check(PlaybackTelemetry.isPlaying(started: true, paused: false, pausedForCache: false, seeking: false, coreIdle: false), "advancing playback")
        check(!PlaybackTelemetry.isPlaying(started: false, paused: false, pausedForCache: false, seeking: false, coreIdle: false), "not started")
        check(!PlaybackTelemetry.isPlaying(started: true, paused: true, pausedForCache: false, seeking: false, coreIdle: false), "user pause")
        check(!PlaybackTelemetry.isPlaying(started: true, paused: false, pausedForCache: true, seeking: false, coreIdle: false), "paused for cache")
        check(!PlaybackTelemetry.isPlaying(started: true, paused: false, pausedForCache: false, seeking: true, coreIdle: false), "seeking")
        check(!PlaybackTelemetry.isPlaying(started: true, paused: false, pausedForCache: false, seeking: false, coreIdle: true), "core idle")

        // audio_info with everything available
        let params: [String: Any] = ["format": "floatp", "samplerate": 48000, "channel-count": 2, "channels": "stereo", "hr-channels": "stereo",
                                     "device-name": "SECRET-DEVICE", "chmap": "FL-FR"]
        var a = PlaybackTelemetry.audioInfo(currentAO: "coreaudio", audioDevice: "auto", audioParams: params, audioOutParams: params)
        check(a["current_ao"] as? String == "coreaudio", "current_ao")
        check(a["audio_device"] as? String == "auto" && a["audio_device_is_default"] as? Bool == true, "default device")
        check((a["unavailable"] as? [String])?.isEmpty == true, "nothing unavailable")
        let p = a["audio_params"] as? [String: Any]
        check(p?["samplerate"] as? Int == 48000 && p?["channels"] as? String == "stereo" && p?["format"] as? String == "floatp", "whitelisted params kept")
        check(p?["device-name"] == nil && p?["chmap"] == nil, "keys outside the whitelist are dropped")
        check(JSONSerialization.isValidJSONObject(a), "result is valid JSON")

        // explicit device: identifier is never recorded
        a = PlaybackTelemetry.audioInfo(currentAO: "avfoundation", audioDevice: "coreaudio/AppleUSBAudioEngine:Vendor:Dev:AA-BB-CC-DD-EE-FF:1,2", audioParams: params, audioOutParams: nil)
        check(a["audio_device"] as? String == "coreaudio/<redacted>", "device identifier redacted: \(String(describing: a["audio_device"]))")
        check(a["audio_device_is_default"] as? Bool == false, "explicit device is not the default")
        check(!String(describing: a).contains("AA-BB"), "no identifier anywhere in the event")
        check(PlaybackTelemetry.redactedDevice("some device without a slash") == "<redacted>", "unknown form redacted")
        check(PlaybackTelemetry.redactedDevice("we ird/x") == "?/<redacted>", "odd backend name not echoed")

        // unavailable properties are explicit, never invented
        a = PlaybackTelemetry.audioInfo(currentAO: nil, audioDevice: nil, audioParams: nil, audioOutParams: nil)
        check(isNull(a["current_ao"]) && isNull(a["audio_device"]) && isNull(a["audio_device_is_default"]) && isNull(a["audio_params"]) && isNull(a["audio_out_params"]), "all null")
        check((a["unavailable"] as? [String]) == ["current-ao", "audio-device", "audio-params", "audio-out-params"], "all four named: \(String(describing: a["unavailable"]))")
        check(JSONSerialization.isValidJSONObject(a), "valid JSON with nulls")
        a = PlaybackTelemetry.audioInfo(currentAO: "", audioDevice: "", audioParams: [:], audioOutParams: nil)
        check((a["unavailable"] as? [String]) == ["current-ao", "audio-device", "audio-out-params"], "empty strings count as unavailable, an empty params map as available-but-empty")
        check((a["audio_params"] as? [String: Any])?.isEmpty == true, "empty params map stays an empty object")

        // non-finite numbers cannot break the JSON line
        a = PlaybackTelemetry.audioInfo(currentAO: "x", audioDevice: "auto", audioParams: ["samplerate": Double.nan, "channels": "stereo"], audioOutParams: nil)
        check(JSONSerialization.isValidJSONObject(a) && (a["audio_params"] as? [String: Any])?["samplerate"] == nil, "NaN dropped")
        check(PlaybackTelemetry.audioProperties == ["current-ao", "audio-device", "audio-params", "audio-out-params"], "property order")

        print("playback telemetry: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
