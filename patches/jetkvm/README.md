# JetKVM upstream and patches

`third_party/jetkvm` is the unmodified https://github.com/jetkvm/kvm.git submodule,
pinned to release/0.5.9 (`36c008fb5cc209f9f9f149ab4e9a864ec7d72cad`).
Only its frontend is built; its Go firmware is not used.

`series` lists patches in application order:

1. `0001-webos-frontend.patch`: the webOS console entry point, upstream WebSocket/WebRTC
   connection integration, TV remote, GPL notices, and build configuration.
   Upstream console components are retained. Encoder configuration stays in
   the TV setup app. Fonts, icons, title, theme behavior, and HTML presentation
   remain upstream. The HTML changes only bundle the
   existing theme initializer for the server's content security policy.

The native server implements JetKVM's local signaling, JSON-RPC 2.0, and HID RPC
v1. The upstream JSON-RPC, HID, keyboard, and mouse hooks are retained unchanged;
the signaling hook carries a type-only CommonJS interop annotation. Protocol and
verification details are in [native/JETKVM_PROTOCOL.md](../../native/JETKVM_PROTOCOL.md).

## Build and edit

```sh
git submodule update --init --recursive
npm run prepare:ui
# Edit build/jetkvm/ui, never third_party/jetkvm/ui.
npm run refresh:ui-patch
npm run build:ui
```

`prepare:ui` exports upstream UI source into an ignored, standalone Git working
tree at `build/jetkvm`. It commits the unmodified base locally, then applies
the webOS patch as working-tree changes. The export includes the unmodified
`internal/logging/sse.html` target so the restored upstream public symlink works
when Vite copies the public directory. `refresh:ui-patch` includes additions,
deletions, and binary changes in the final patch; review that patch before
committing it in this repository. Generated localization, dependencies, and
build output are excluded. Earlier patches are maintained separately.

An unchanged input reuses the prepared source, including local edits. Changed
patch inputs regenerate the tree and preserve its dependency directory, unless
there are unsaved source edits; in that case preparation stops to protect them.
To discard a prepared tree deliberately, remove `build/jetkvm` and prepare again.

To update upstream, check out the desired commit in the submodule, adjust the
patch series until it applies, and stage the new gitlink together with the patch
changes. Review upstream changes and run the frontend build and browser checks.

The TV serves the complete patched frontend source, this patch series, the
upstream revision, GPL license, and third-party notices. Its source archive can
also be built directly with `npm ci --ignore-scripts && npm run build:device`
inside the archived `ui` directory.
