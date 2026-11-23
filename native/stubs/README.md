# webOS link stubs

These build-only libraries use webos-userland's linker-script approach: each
`.lds` file aliases the library's functions to `dummy` and data to `dummy_data`
in a shared `dummy.c`. The TV supplies the actual library at runtime.
The committed export lists come from the running LG C2 (O22), webOS
11.2.0, rather than the Chromium development sysroot.

| Library       | Export list | Exported symbols |
| ------------- | ----------- | ---------------- |
| `libnyx.so.7` | `nyx.lds`   | 2,733            |
| `libim.so.1`  | `im.lds`    | 312              |

The source library SHA-256 is recorded in each generated file. Every externally
visible definition has an alias, including names originally exported as weak or
GNU-unique symbols. Aliases preserve the function/data distinction; binding
strength, object sizes, and data contents are placeholders rather than replicas
of the runtime ABI. The current application bindings call functions only.
Executing a dummy function traps. No vendor implementation or library binary is
included here.

`native/nyx.hpp` declares the ARM32 types and proxy methods used by the optional
`libwebos-jetkvm-nyx.so` capture module. C++ namespaces and parameter type names match the
firmware ABI so the compiler produces the symbol references directly. Proxy
objects remain owned by the TV library; they must not be constructed, copied,
or deleted by this application. The input-manager call retains its existing
`extern "C" int IM_RequestToCheckInput()` signature.

## Regenerate and verify

Copy reference libraries from the target TV into the ignored local research directory:

```sh
mkdir -p research/link-stub-reference
scp -O root@<TV-IP>:/usr/lib/libnyx.so.7 research/link-stub-reference/
scp -O root@<TV-IP>:/usr/lib/libim.so.1 research/link-stub-reference/
python3 tools/extract_link_stub.py research/link-stub-reference/libnyx.so.7 native/stubs/nyx.lds
python3 tools/extract_link_stub.py research/link-stub-reference/libim.so.1 native/stubs/im.lds
cmake --build build/release --target webos-jetkvm --parallel
python3 tools/extract_link_stub.py research/link-stub-reference/libnyx.so.7 \
    --verify build/release/native/stubs/libnyx.so.7
python3 tools/extract_link_stub.py research/link-stub-reference/libim.so.1 \
    --verify build/release/native/stubs/libim.so.1
```

The extractor reads ELF32 ARM metadata using only the Python standard library.
Add `--check` to a generation command to check the committed file without
rewriting it. `--verify` checks the complete built export-name list, function/data
types, and SONAME. It does not compare placeholder sizes or binding strengths.
Unsupported export types and versioned definitions are rejected instead of
silently omitted.

Cross builds produce the stubs in `build/release/native/stubs/`. They are not
installed or copied into `service/lib` or the IPK. The server loads the capture
module from its bundled runtime library search path. The module and the server
resolve Nyx and input-manager, respectively, from the TV's library search path.
Do not add the stub directory to `LD_LIBRARY_PATH`
or distribute these libraries as runtime implementations.
