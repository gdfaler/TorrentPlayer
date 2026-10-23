// Offline tests for PlaybackKeys. Build: swiftc -parse-as-library app/Sources/PlaybackKeys.swift tests/app/playback_keys_test.swift
import Foundation
var failures = 0, checks = 0
func check(_ ok: Bool, _ what: @autoclosure () -> String) { checks += 1; if !ok { failures += 1; print("FAIL: \(what())") } }

@main struct PlaybackKeysTest {
    static func main() {
        // English (QWERTY): the documented letters, either case
        for (code, ch, want) in [(UInt16(3), "f", PlaybackKey.fullscreen), (46, "m", .mute), (14, "e", .expandCollapse), (15, "r", .retry), (3, "F", .fullscreen), (46, "M", .mute)] {
            check(PlaybackKey.resolve(keyCode: code, characters: ch) == want, "english \(ch)")
        }
        // Russian (ЙЦУКЕН): the same physical keys, Cyrillic characters, upper and lower case
        for (code, ch, want) in [(UInt16(3), "а", PlaybackKey.fullscreen), (46, "ь", .mute), (14, "у", .expandCollapse), (15, "к", .retry), (3, "А", .fullscreen), (46, "Ь", .mute), (14, "У", .expandCollapse)] {
            check(PlaybackKey.resolve(keyCode: code, characters: ch) == want, "russian \(ch) code \(code)")
        }
        // other Cyrillic / non-letter keys never trigger
        for (code, ch) in [(UInt16(7), "ч"), (0, "ф"), (13, "ц"), (49, " "), (123, "\u{F702}"), (36, "\r"), (18, "1")] { check(PlaybackKey.resolve(keyCode: code, characters: ch) == nil, "no shortcut for \(ch) code \(code)") }
        // other Latin letters never trigger
        for (i, ch) in "abcdghijklnopqstuvwxyz".enumerated() { check(PlaybackKey.resolve(keyCode: UInt16(i), characters: String(ch)) == nil, "latin \(ch)") }
        // Latin layouts that move letters keep the LETTER (Dvorak: physical F position types "u"; physical position of "f" holds another key)
        check(PlaybackKey.resolve(keyCode: 3, characters: "u") == nil, "dvorak: F position types u")
        check(PlaybackKey.resolve(keyCode: 17, characters: "m") == .mute, "dvorak: letter m wherever it sits")
        check(PlaybackKey.resolve(keyCode: 17, characters: "e") == .expandCollapse, "letter e wherever it sits")
        // no character at all (dead key, unknown): physical position
        check(PlaybackKey.resolve(keyCode: 46, characters: nil) == .mute && PlaybackKey.resolve(keyCode: 46, characters: "") == .mute, "no characters falls back to the key code")
        check(PlaybackKey.resolve(keyCode: 99, characters: nil) == nil, "unknown key code")
        // Greek / Hebrew style layouts also use the physical key
        check(PlaybackKey.resolve(keyCode: 46, characters: "μ") == .mute, "greek mu on the M key")
        // modifiers
        check(PlaybackKey.modifiersAllowShortcut(command: false, control: false, option: false), "plain")
        check(!PlaybackKey.modifiersAllowShortcut(command: true, control: false, option: false) && !PlaybackKey.modifiersAllowShortcut(command: false, control: true, option: false) && !PlaybackKey.modifiersAllowShortcut(command: false, control: false, option: true), "cmd / ctrl / option")
        print("playback_keys_test: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
