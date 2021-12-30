# macOS Build

Tested on macOS 10.15.7, Xcode 12.4, x86_64.

1. Run `setup-vcpkg.sh` (fetches + builds vorbis, opus, zlib from vcpkg, as static libraries.)

   The Opus .dylibs included with QuakeSpasm lack the encoder, which is needed by `snd_voip.c`, necessitating building it from source.

2. For a release build, run `xcodebuild -project QuakeSpasm.xcodeproj -target QuakeSpasm-Spiked-SDL2`


## Limitations

- SDL 1.2 is no longer supported
    - Some code assumes SDL 2.0+ (e.g. `VID_UpdateCursor`)
- Only x86_64 is currently supported
    - arm64 should be possible in theory, just needs someone with an M1 mac to implement and test it.
- Minimum macOS version raised to 10.9; PowerPC and i386 dropped
    - This is the lowest version I can target with macOS 10.15.7, Xcode 12.4
