# webos-jetkvm

View and control a rooted LG webOS TV from Firefox using JetKVM’s browser interface.
Video uses hardware H.264 encoding and WebRTC, with keyboard, mouse, scrolling,
paste, macros, and TV remote controls.

Requires root access, Homebrew Channel, and compatible webOS capture interfaces.
The server uses Nyx capture where the firmware exports the required interfaces.
On LG O22 TVs such as the C2, it can also use the older LG hardware encoder and
display feedback controls. One viewer can connect at a time. Audio streaming is
not supported.

## Install and use

1. Install the IPK from `dist/` on the TV.
2. Open **webOS JetKVM** and grant root access through Homebrew Channel.
3. Save a browser password and select **Start server**.
4. Open `http://<TV-IP>:47990/` in Firefox. Log in as **jetkvm** with your password.

The TV app controls resolution, start/stop, and autostart. Available video presets
are 360p, 540p, 720p, and 1080p; the default is 1920 × 1080 at 30 fps.

For mouse control, turn **Audio Guidance** off in the TV’s accessibility settings.
Right-click and Escape act as **Back**. Keyboard layout and macros are saved on
the TV; appearance and mouse preferences are saved in the browser.

## Build

Use Node.js **22.21.1 or newer in the Node 22 series**, CMake 3.24+, C++20, and the
[Buildroot NC4 webOS SDK](https://github.com/openlgtv/buildroot-nc4/releases/tag/2026.08-webos).
The native server targets ARM32 webOS.

```sh
git submodule update --init --recursive
npm ci --ignore-scripts
npm run build
```

On Linux, extract the SDK, run its `relocate-sdk.sh`, then build and package:

```sh
export PATH=/path/to/sdk/bin:$PATH
export CMAKE_TOOLCHAIN_FILE=/path/to/sdk/share/buildroot/toolchainfile.cmake
npm run build:native
npm run package
npm run manifest
```

On macOS, use Homebrew LLVM/LLD with the supplied toolchain:

```sh
brew install llvm lld cmake
WEBOS_SDK_ROOT=/path/to/sdk cmake -S . -B build/release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/macos-webos.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target stage-service --parallel
npm run package
npm run manifest
```

Alternatively, build with Docker:

```sh
docker build -t webos-jetkvm-builder .
docker run --rm -v "$PWD:/work" webos-jetkvm-builder
```

Packages are written to `dist/`. Use a fresh CMake build directory when switching
toolchains.

## Development

The JetKVM UI is a pinned submodule with a webOS patch series. See the
[patch workflow](patches/jetkvm/README.md) and [protocol documentation](native/JETKVM_PROTOCOL.md).

Portable unit tests run on the development machine:

```sh
npm run quality:setup
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_FLAGS=-Werror -DCMAKE_CXX_FLAGS=-Werror
cmake --build build/host --target settings-test annexb-test jetkvm-test lifecycle-test control-test --parallel
npm run check
```

Run browser integration tests against a TV with the server running and no active viewer:

```sh
npx playwright install firefox
WEBOS_SMOKE_SSH=root@<TV-IP> npm run test:browser -- http://<TV-IP>:47990
```

SSH reads the saved password privately. Alternatively, set `WEBOS_STREAM_PASSWORD`.
Use `npm run format` to format project files. Server logs are in
`/tmp/webos-jetkvm.log`.

## License

[GPL-2.0-only](LICENSE). The browser interface is based on
[JetKVM](https://github.com/jetkvm/kvm), with its original notices preserved.
The TV app/package structure originated in
[webos-vncserver](https://github.com/Informatic/webos-vncserver).
See [third-party licenses](native/THIRD_PARTY.md) for native dependencies.
