# Distribution and licensing notes (facts as checked on 2026-10-24; not legal advice)

TorrentPlayer is distributed under GPL-3.0-or-later (see LICENSE). The reason is the bundled combination, verified on the binaries:
- FFmpeg 9.0.2 is configured with `--enable-gpl --enable-version3` (libx264, libx265, lame, opus, dav1d, libvpx, SVT-AV1, libvmaf, OpenSSL), i.e. GPL-3.0-or-later.
- libmpv 0.41.0 is built with its `gpl` feature (GPL-2.0-or-later) and is loaded dynamically by the app.
- x264, x265 and rubberband are GPL-2.0-or-later; several LGPL libraries (libplacebo, lame, mpg123 [LGPL-2.1-only], libbluray, libudfread, glib, libintl, fribidi) are separate, replaceable dylibs.
- OpenSSL 3/4, shaderc, the Vulkan loader and MoltenVK are Apache-2.0 (compatible with GPLv3).

Per-release material is generated from the exact bundle by `tools/scripts/gen_third_party.py` (called by `build_release.sh`) and assembled by `tools/scripts/package_release.sh`:
THIRD_PARTY_NOTICES.md, COMPLIANCE_STATUS.md (what is resolved and what is not), SOURCE_OFFER.txt, source-manifest.tsv (upstream URL + sha256 or git revision + patches per library),
fetch_third_party_sources.sh (downloads and verifies the third-party source archives; needs network), the Homebrew formulae / receipts / SBOMs used for the build, and the TorrentPlayer source archive.

A binary release is complete only when the third-party source archives listed in source-manifest.tsv have been fetched, checksum-verified and attached to the same GitHub Release.
The app is ad-hoc signed and not notarized; Gatekeeper requires the user to approve it once. Codec patent licences are not provided by any of these licences.
