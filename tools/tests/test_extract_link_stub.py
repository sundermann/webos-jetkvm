"""Exercise ELF export validation without requiring proprietary TV libraries."""

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "extract_link_stub", Path(__file__).resolve().parents[1] / "extract_link_stub.py"
)
stub = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = stub
spec.loader.exec_module(stub)


def library_bytes(*, symbol="example", kind=2, version=1):
    strings = b"\0libexample.so.1\0" + symbol.encode() + b"\0"
    contents = [
        b"",
        strings,
        bytes(16) + struct.pack("<IIIBBH", 17, 0, 0, 16 | kind, 0, 1),
        struct.pack("<IIII", 14, 1, 0, 0),
        struct.pack("<HH", 0, version),
    ]
    offset = 52 + 40 * len(contents)
    sections = []
    for index, data in enumerate(contents):
        section_type = [0, 3, 11, 6, 0x6FFFFFFF][index]
        sections.append(
            struct.pack(
                "<IIIIIIIIII",
                0,
                section_type,
                0,
                0,
                offset,
                len(data),
                1,
                0,
                1,
                16 if index == 2 else 0,
            )
        )
        offset += len(data)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + bytes(9),
        3,
        40,
        1,
        0,
        0,
        52,
        0,
        52,
        0,
        0,
        40,
        len(contents),
        0,
    )
    return header + b"".join(sections) + b"".join(contents)


class ExportTests(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "library.so"
            path.write_bytes(data)
            return stub.read_library(path)

    def test_function_and_data_aliases(self):
        function = self.read(library_bytes())
        self.assertEqual(function.soname, "libexample.so.1")
        self.assertEqual(function.symbols, {"example": 2})
        self.assertIn('"example" = "dummy";', stub.export_source(function))
        data = self.read(library_bytes(kind=1))
        self.assertIn('"example" = "dummy_data";', stub.export_source(data))
        with self.assertRaisesRegex(ValueError, "Export mismatch"):
            stub.verify(function, data)

    def test_rejects_unsupported_exports(self):
        for options in [{"version": 2}, {"kind": 6}, {"symbol": "injected;code"}]:
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.read(library_bytes(**options))

    def test_rejects_malformed_libraries(self):
        for data in [b"", library_bytes()[:70], library_bytes()[:-1]]:
            with self.subTest(length=len(data)), self.assertRaises((ValueError, struct.error)):
                self.read(data)

    def test_verifies_complete_exports_and_soname(self):
        reference = self.read(library_bytes())
        stub.verify(reference, reference)
        for candidate in [
            stub.Library("other.so", reference.symbols, ""),
            stub.Library(reference.soname, {}, ""),
            stub.Library(reference.soname, {**reference.symbols, "extra": 2}, ""),
        ]:
            with self.assertRaises(ValueError):
                stub.verify(reference, candidate)


if __name__ == "__main__":
    unittest.main()
