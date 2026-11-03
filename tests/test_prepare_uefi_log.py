import tempfile
import unittest
from pathlib import Path

from scripts.prepare_uefi_log import (
    HEADER_SIZE,
    PAYLOAD_SIZE,
    READY,
    SECTOR_COUNT,
    SECTOR_SIZE,
    TEMPLATE_SIZE,
    USED,
    TemplateError,
    create_template,
    extract_used,
    sector_header,
)


class PrepareUefiLogTests(unittest.TestCase):
    def test_create_exact_size_headers_and_offsets(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "template.bin"
            create_template(path)
            data = path.read_bytes()
            self.assertEqual(len(data), 1024 * 1024)
            self.assertEqual(len(data), TEMPLATE_SIZE)
            self.assertEqual((SECTOR_SIZE, SECTOR_COUNT), (512, 2048))
            self.assertEqual(HEADER_SIZE, 32)
            self.assertEqual(PAYLOAD_SIZE, 480)
            for index in range(SECTOR_COUNT):
                offset = index * SECTOR_SIZE
                expected = sector_header(index, READY)
                self.assertEqual(len(expected), 32)
                self.assertEqual(data[offset : offset + HEADER_SIZE], expected)
                self.assertEqual(data[offset + HEADER_SIZE : offset + SECTOR_SIZE], b" " * 480)
            self.assertEqual(data[:32], b"J250FLOG2 00000000 READY       \n")
            self.assertEqual(data[19:24], b"READY")
            self.assertEqual(data[SECTOR_SIZE + 19 : SECTOR_SIZE + 24], b"READY")

    def test_creation_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "template.bin"
            path.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                create_template(path)
            self.assertEqual(path.read_bytes(), b"keep")

    def test_extract_used_records_and_validate(self):
        with tempfile.TemporaryDirectory() as temporary:
            template = Path(temporary) / "template.bin"
            output = Path(temporary) / "records.bin"
            create_template(template)
            with template.open("r+b") as file:
                file.seek(0)
                file.write(sector_header(0, USED, len(b"first record  "), b"first record  "))
                file.write(b"first record  " + b" " * (PAYLOAD_SIZE - len(b"first record  ")))
                file.seek(SECTOR_SIZE)
                file.write(sector_header(1, USED, len(b"second\x00record"), b"second\x00record"))
                file.write(b"second\x00record" + b" " * (PAYLOAD_SIZE - len(b"second\x00record")))
            self.assertEqual(extract_used(template, output), 2)
            self.assertEqual(output.read_bytes(), b"first record  second\x00record")

    def test_used_after_ready_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            template = Path(temporary) / "template.bin"
            create_template(template)
            with template.open("r+b") as file:
                file.seek(SECTOR_SIZE)
                file.write(sector_header(1, USED, 1, b" "))
            with self.assertRaises(TemplateError):
                extract_used(template, Path(temporary) / "out.bin")

    def test_corruption_is_rejected_without_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            template = Path(temporary) / "template.bin"
            output = Path(temporary) / "records.bin"
            create_template(template)
            with template.open("r+b") as file:
                file.seek(5 * SECTOR_SIZE)
                file.write(b"X")
            with self.assertRaises(TemplateError):
                extract_used(template, output)
            self.assertFalse(output.exists())

    def test_wrong_length_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            template = Path(temporary) / "short.bin"
            template.write_bytes(b"short")
            with self.assertRaises(TemplateError):
                extract_used(template, Path(temporary) / "out.bin")

    def test_torn_payload_and_verified_prefix_salvage(self):
        with tempfile.TemporaryDirectory() as temporary:
            template = Path(temporary) / "template.bin"
            create_template(template)
            with template.open("r+b") as file:
                for index, text in enumerate([b"intact\n", b"torn\n"]):
                    file.seek(index * SECTOR_SIZE)
                    file.write(sector_header(index, USED, len(text), text))
                    file.write(text)
                file.seek(SECTOR_SIZE + HEADER_SIZE)
                file.write(b"X")
            with self.assertRaises(TemplateError):
                extract_used(template, Path(temporary) / "refused.txt")
            output = Path(temporary) / "salvaged.txt"
            self.assertEqual(extract_used(template, output, salvage=True), 1)
            self.assertEqual(output.read_bytes(), b"intact\n")


if __name__ == "__main__":
    unittest.main()
