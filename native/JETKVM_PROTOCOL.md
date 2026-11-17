# Local JetKVM protocol

The C++ server implements the console protocol used by the pinned JetKVM
0.5.9 frontend. It uses the existing Nyx H.264 capture and TV uinput backend;
JetKVM's Go firmware and hardware drivers are not required.

## Signaling

The browser opens `ws://<TV>:47990/webrtc/signaling/client`. The HTTP gateway
requires the saved browser password and an exact same-origin `Origin` header
before upgrading the connection. Plain HTTP is supported. The server sends:

```json
{ "type": "device-metadata", "data": { "deviceVersion": "0.5.9" } }
```

The version identifies frontend protocol compatibility, not the TV firmware.
The browser creates the offer and sends `{"type":"offer","data":{"sd":"…"}}`.
`sd` is base64-encoded JSON containing the SDP `type` and `sdp`, as in upstream.
The answer is `{"type":"answer","data":"…"}` with the same base64 JSON encoding.
Both sides exchange `new-ice-candidate` messages containing standard candidate
objects. Text `ping` gets text `pong`. The server accepts the offered H.264
payload number, sends video, and leaves the offered audio m-line inactive.
Only one viewer owns capture and input at a time; rejected connections do not
release the existing viewer's keys. Closing signaling ends the session.

## Commands and input

The browser creates these WebRTC data channels:

- `rpc`: JSON-RPC 2.0 commands, responses, and `videoInputState` notifications.
- `hidrpc`: reliable HID RPC v1 handshake, keyboard reports, button transitions,
  macro execution/cancellation, keepalives, and state acknowledgements.
- `hidrpc-unreliable-ordered` and `hidrpc-unreliable-nonordered`: pointer movement.
  These channels cannot change button state; reliable reports own transitions.

HID layouts follow the pinned `ui/src/hooks/hidRpc.ts` and upstream HID parser.
Multibyte fields use big-endian encoding. Macros contain nine bytes per step
(modifier, six key usages, and a two-byte delay), execute asynchronously, and
report start/completion. Cancelling, disconnecting, or stopping releases input.
Held keys auto-release after 500 ms without a keyboard report or HID keepalive.
SCTP channels close before the transport to avoid reload-time channel aborts.

The supported JSON-RPC console methods are `ping`, `getVideoState`,
`getKeyDownState`, `getKeyboardLedState`, `getAudioConfig`, `getUsbDevices`,
`getUSBState`, `keyboardReport`, `keypressReport`, `absMouseReport`,
`relMouseReport`, `wheelReport`, `getKeyboardLayout`, `setKeyboardLayout`,
`getKeyboardMacros`, and `setKeyboardMacros`. Audio and USB gadget features are
reported unavailable; keyboard LEDs have no TV feedback and report false.
Unsupported device/cloud/encoder commands return method-not-found (`-32601`).
Malformed JSON or requests return standard JSON-RPC errors. Notifications do
not receive responses. Encoder, password, and autostart settings cannot be changed through console RPC.

Three TV extensions use the same JSON-RPC envelope: `tvRemote` taps a whitelisted
TV remote key, `releaseMouseButtons` releases held buttons without injecting
coordinates, and `importBrowserPreferences` imports legacy browser layout and
macros only when the corresponding TV preference is absent. Console preferences
are stored under `console` in the existing root-only config, using its atomic
save and lock. Writes preserve the password, resolution, and autostart.

Duplicate coordinates do not generate native mouse events, including button-only
reports. Browser blur and pointer exit release buttons without a position reset,
allowing webOS's existing pointer idle timeout to run. Keyboard key-down hides
the cursor through `wl_webos_input_manager` before injecting the key; movement
restores pointer mode through the normal input path. No additional idle timer or
synthetic navigation keys are used.
The frontend rejects coordinates outside the displayed image or browser viewport,
including events retargeted by pointer capture. An outside release uses
`releaseMouseButtons` instead of clamping coordinates to the video edges.

## Verification

`ctest --test-dir build/host --output-on-failure` covers shared protocol parsing,
input-event mapping, button authority, key release, macro completion/cancellation
and preference persistence. The event recorder is compiled only into unit tests.
The production runtime is built only for webOS and has no synthetic capture or
simulated input modes.

With the server running on the TV and no active viewer:

```sh
WEBOS_SMOKE_SSH=root@<TV-IP> npm run test:browser -- http://<TV-IP>:47990 http ui protocol smoke
```

The independent browser peer exercises the stock wire formats, including trickle
ICE, real decoding, RPC, binary input, busy-viewer rejection, idle operation and
reconnection. The UI suite exercises the upstream console components. The HTTP
suite starts separate real webOS processes over SSH to verify authentication,
origin enforcement, duplicate ports, CLI validation and signal-driven shutdown.
An explicit TV URL and credentials are required; no local server is launched.
