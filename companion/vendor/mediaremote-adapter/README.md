# mediaremote-adapter (vendored)

Copy of [ungive/mediaremote-adapter](https://github.com/ungive/mediaremote-adapter) at commit `0e82bf1`,
BSD 3-Clause (see `LICENSE`). Only the framework sources and the Perl script are kept.

It reads the macOS "Now Playing" information (any app: Music, Spotify, Chrome, Safari…) through the
private MediaRemote framework, which macOS 15.4+ only lets entitled system binaries use: the Perl
script runs in `/usr/bin/perl` and loads the framework.

`companion/src-tauri/build.rs` compiles the framework with clang into `src-tauri/resources/mediaremote/`,
which the app bundles. To update, copy the same folders from a newer commit and check the stream format.
