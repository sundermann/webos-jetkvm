webos-jetkvm uses these libraries without source modifications. The pinned
source and its license are available at the links below; license texts are
included alongside the binary in licenses/.

- libdatachannel 0.24.6, MPL-2.0:
  https://github.com/paullouisageneau/libdatachannel/tree/2afeedc70878043bcd59d2d726e1476824f687c3
  Its pinned submodules include libjuice (MPL-2.0), libsrtp (BSD-3-Clause),
  usrsctp (BSD-3-Clause), plog (MIT), and nlohmann/json (MIT).
- cpp-httplib, MIT:
  https://github.com/yhirose/cpp-httplib/tree/546e821c65a8bd8426c351d7e3171a99231b6456
- OpenSSL 3 from the Buildroot NC4 SDK, Apache-2.0:
  https://github.com/openssl/openssl/tree/openssl-3.6.4
- GCC runtime libraries from Buildroot NC4 (GCC Runtime Library Exception):
  https://github.com/openlgtv/buildroot-nc4/releases/tag/2026.08-webos

The separately served JetKVM frontend is GPL-2.0. Its upstream attribution,
license, third-party notices, and complete modified frontend source archive are
included under web/assets/; see ui/UPSTREAM.md in this repository.
