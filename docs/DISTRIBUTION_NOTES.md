# Dependency and licensing notes (facts as checked on 2026-10-15; not legal advice)

A successful local build does NOT make the app distributable. Facts:

| Component | Version here | License (Homebrew metadata) | Linked how |
|---|---|---|---|
| mpv / libmpv | 0.41.0_10 | GPL-2.0-or-later AND LGPL-2.1-or-later (this Homebrew build is the GPL one) | dynamic, absolute path `/opt/homebrew/opt/mpv/lib` |
| FFmpeg (via mpv) | 9.0.2 | GPL-3.0-or-later (this Homebrew build) | dynamic, through libmpv |
| libplacebo | 7.360.1 | LGPL-2.1-or-later | dynamic, through libmpv |
| libtorrent-rasterbar | 2.1.2 | BSD-3-Clause | dynamic |
| Boost | 1.92.0 | BSL-1.0 (header use) | headers |
| OpenSSL | 3.6.5 | Apache-2.0 | dynamic |

Implications recorded for later decisions (to be confirmed by someone qualified before any distribution):
- The app links a GPL build of libmpv (which itself pulls a GPL-3.0-or-later FFmpeg build). Distributing a binary of a combined work like that would normally require offering the app's complete source under GPL-compatible terms. Own-Mac use is outside this question; sharing the binary is inside it.
- An LGPL-only mpv/FFmpeg build (different configure options) would change the picture; Homebrew does not provide one.
- The bundle links Homebrew libraries by absolute path (`/opt/homebrew/...`): it only runs on a Mac with the same Homebrew packages installed, and a Homebrew upgrade can break it (ABI drift). Record the versions above in any bug report. The bundle is ad-hoc signed and not notarised, so Gatekeeper would refuse it on another Mac.
- The BitTorrent protocol use is the user's responsibility; the app ships no content and no tracker list.
Conclusion for M2: **own-Mac use only**. No distribution claim is made.
