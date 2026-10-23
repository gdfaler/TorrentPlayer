// PlaybackKeys: which letter shortcut (full screen, mute, expand/collapse, retry) a key press means, independent of the active input source. Pure Foundation: unit-tested on its own.
// A Latin layout keeps its letters (so Dvorak / AZERTY users keep the documented F, M, E, R); a layout whose keys do not produce Latin letters (Russian, Greek, ...) falls back to the
// PHYSICAL key (the ANSI key code of the same position), so the shortcuts sit on the same keys as under English QWERTY.
import Foundation

enum PlaybackKey: Equatable {
    case fullscreen, mute, expandCollapse, retry

    static let ansiKeyCodes: [UInt16: PlaybackKey] = [3: .fullscreen, 46: .mute, 14: .expandCollapse, 15: .retry]       // kVK_ANSI_F, M, E, R
    static let letters: [Character: PlaybackKey] = ["f": .fullscreen, "m": .mute, "e": .expandCollapse, "r": .retry]

    /// `characters` = NSEvent.charactersIgnoringModifiers of a key-down.
    static func resolve(keyCode: UInt16, characters: String?) -> PlaybackKey? {
        if let c = characters?.lowercased().first, c.isASCII, c.isLetter { return letters[c] }       // a Latin layout decides by its own letter
        return ansiKeyCodes[keyCode]                                                                 // non-Latin (or no) character: the physical key
    }

    /// Plain shortcuts only: Command belongs to the menus, Control and Option never start a playback shortcut (Shift is allowed, as before).
    static func modifiersAllowShortcut(command: Bool, control: Bool, option: Bool) -> Bool { !command && !control && !option }
}
