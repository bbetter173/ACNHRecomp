"""Synthetic message resources for the read-only dialogue catalog."""
import contextlib
import io
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
import struct
import tempfile
import unittest

from tools import dialogue_catalog as catalog


def msbt(strings, labels=(), endian="<", offsets=None):
    table_size = 4 + 4 * len(strings)
    blob = b"".join(strings)
    if offsets is None:
        offsets, offset = [], table_size
        for string in strings:
            offsets.append(offset)
            offset += len(string)
    txt = struct.pack(endian + "I", len(strings)) + struct.pack(endian + f"{len(strings)}I", *offsets) + blob
    entries = b"".join(bytes([len(label.encode())]) + label.encode() + struct.pack(endian + "I", index)
                       for label, index in labels)
    lbl = struct.pack(endian + "III", 1, len(labels), 12) + entries
    sections = bytearray()
    for magic, payload in [(b"LBL1", lbl), (b"TXT2", txt)]:
        sections += magic + struct.pack(endian + "I", len(payload)) + bytes(8) + payload
        sections += b"\xab" * (-len(sections) % 16)
    header = bytearray(32)
    header[:8] = b"MsgStdBn"
    header[8:10] = b"\xff\xfe" if endian == "<" else b"\xfe\xff"
    header[12:14] = bytes([1, 3])
    struct.pack_into(endian + "H", header, 14, 2)
    struct.pack_into(endian + "I", header, 18, 32 + len(sections))
    return bytes(header + sections)


def text(value, endian="<", suffix=b""):
    return value.encode("utf-16-le" if endian == "<" else "utf-16-be") + b"\0\0" + suffix


def sarc(files, endian="<"):
    names, nodes, body = bytearray(), bytearray(), bytearray()
    for name, raw in files:
        names += bytes(-len(names) % 4)
        at = len(names) // 4
        names += name.encode() + b"\0"
        start = len(body)
        body += raw
        nodes += struct.pack(endian + "IIII", 0, 0x01000000 | at, start, len(body))
    sfat = b"SFAT" + struct.pack(endian + "HHI", 12, len(files), 0x65) + nodes
    sfnt = b"SFNT" + struct.pack(endian + "HH", 8, 0) + names
    data_at = (20 + len(sfat) + len(sfnt) + 31) & ~31
    header = b"SARC" + struct.pack(endian + "HHIIHH", 20, 0xFEFF, data_at + len(body), data_at, 0x100, 0)
    return header + sfat + sfnt + bytes(data_at - 20 - len(sfat) - len(sfnt)) + body


class MessageTests(unittest.TestCase):
    def test_labels_map_to_indices_in_both_byte_orders(self):
        for endian in ("<", ">"):
            with self.subTest(endian=endian):
                result = catalog.parse_msbt(msbt([text("first", endian), text("second", endian)],
                                                  [("later", 1), ("earlier", 0)], endian))
                self.assertEqual(result["message_count"], 2)
                self.assertEqual(result["messages"][0]["labels"], ["earlier"])
                self.assertEqual(result["messages"][1]["text"], "second")

    def test_controls_keep_exact_payload_and_offsets(self):
        for endian in ("<", ">"):
            raw = text("A", endian)[:-2]
            raw += struct.pack(endian + "HHHH", 0x0E, 7, 9, 3) + b"\x00\x00\xff"
            raw += text("B", endian)[:-2] + struct.pack(endian + "HHH", 0x0F, 7, 9) + b"\0\0"
            result = catalog.parse_msbt(msbt([raw], [("tagged", 0)], endian))["messages"][0]
            self.assertEqual(result["text"], "AB")
            controls = [part for part in result["parts"] if part["kind"] == "control"]
            self.assertEqual(controls[0], {"kind": "control", "offset": 2, "marker": 0x0E,
                                          "group": 7, "type": 9, "parameters_hex": "0000ff", "byte_length": 11})
            self.assertEqual(controls[1]["marker"], 0x0F)
            self.assertEqual(controls[1]["parameters_hex"], "")

    def test_empty_strings_surrogate_pairs_and_padding(self):
        result = catalog.parse_msbt(msbt([text(""), text("A\U0001f332Z", suffix=b"\xab\xab\xab\xab")]))
        self.assertEqual(result["messages"][0]["text"], "")
        self.assertEqual(result["messages"][1]["text"], "A\U0001f332Z")
        self.assertEqual(result["messages"][1]["byte_length"], 10)

    def test_duplicate_text_offsets_are_explicit_aliases(self):
        result = catalog.parse_msbt(msbt([text("shared"), b""], [("one", 0), ("two", 1)], offsets=[12, 12]))
        self.assertEqual([m["text"] for m in result["messages"]], ["shared", "shared"])
        self.assertEqual([m["labels"] for m in result["messages"]], [["one"], ["two"]])

    def test_unordered_offsets_preserve_the_indices(self):
        result = catalog.parse_msbt(msbt([text("A"), text("B")], offsets=[16, 12]))
        self.assertEqual([m["text"] for m in result["messages"]], ["B", "A"])

    def test_duplicate_label_is_rejected(self):
        with self.assertRaisesRegex(catalog.CatalogError, "duplicated"):
            catalog.parse_msbt(msbt([text("A")], [("same", 0), ("same", 0)]))

    def test_label_index_outside_txt2_is_rejected(self):
        with self.assertRaisesRegex(catalog.CatalogError, "index"):
            catalog.parse_msbt(msbt([text("A")], [("bad", 3)]))

    def test_out_of_bounds_string_offsets_are_rejected(self):
        for offset in (0, 4, 10000):
            with self.subTest(offset=offset), self.assertRaisesRegex(catalog.CatalogError, "offset"):
                catalog.parse_msbt(msbt([text("A")], offsets=[offset]))

    def test_truncated_control_headers_and_payloads_are_rejected(self):
        for raw in (struct.pack("<H", 0x0E), struct.pack("<HHH", 0x0E, 1, 2),
                    struct.pack("<HHHH", 0x0E, 1, 2, 8) + bytes(4), struct.pack("<HH", 0x0F, 1)):
            with self.subTest(raw=raw), self.assertRaises(catalog.CatalogError):
                catalog.parse_msbt(msbt([raw]))

    def test_missing_terminator_and_invalid_surrogates_are_rejected(self):
        for raw in ("missing".encode("utf-16-le"), b"\x00\xd8\0\0"):
            with self.subTest(raw=raw), self.assertRaises(catalog.CatalogError):
                catalog.parse_msbt(msbt([raw]))

    def test_section_bounds_and_encoding_are_checked(self):
        original = msbt([text("A")])
        for change in (lambda b: b.__setitem__(12, 0),
                       lambda b: struct.pack_into("<I", b, 36, len(b)),
                       lambda b: struct.pack_into("<I", b, 18, len(b) + 1)):
            raw = bytearray(original)
            change(raw)
            with self.assertRaises(catalog.CatalogError):
                catalog.parse_msbt(bytes(raw))

    def test_empty_txt2_is_supported(self):
        self.assertEqual(catalog.parse_msbt(msbt([]))["messages"], [])


class ArchiveTests(unittest.TestCase):
    def test_member_identity_and_contents_in_both_byte_orders(self):
        for endian in ("<", ">"):
            raw = msbt([text("hello", endian)], [("greeting", 0)], endian)
            archive = sarc([("chat/hello.msbt", raw), ("other.bin", b"other")], endian)
            members = catalog.parse_sarc(archive)
            self.assertEqual([m["name"] for m in members], ["chat/hello.msbt", "other.bin"])
            self.assertEqual(archive[members[0]["start"]:members[0]["end"]], raw)

    def test_duplicate_names_and_bad_member_ranges_are_rejected(self):
        with self.assertRaisesRegex(catalog.CatalogError, "duplicated"):
            catalog.parse_sarc(sarc([("a", b"a"), ("a", b"b")]))
        archive = bytearray(sarc([("a", b"a")]))
        struct.pack_into("<I", archive, 20 + 12 + 12, len(archive))
        with self.assertRaisesRegex(catalog.CatalogError, "range"):
            catalog.parse_sarc(bytes(archive))

    def test_name_offset_and_truncated_tables_are_rejected(self):
        archive = bytearray(sarc([("a", b"a")]))
        struct.pack_into("<I", archive, 20 + 12 + 4, 0x01FFFFFF)
        with self.assertRaisesRegex(catalog.CatalogError, "name offset"):
            catalog.parse_sarc(bytes(archive))
        with self.assertRaises(catalog.CatalogError):
            catalog.parse_sarc(sarc([("a", b"a")])[:33])

    def test_cli_defaults_to_inventory_and_exports_selected_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "messages.sarc"
            raw = msbt([text("first"), text("second")], [("one", 0), ("two", 1)])
            archive.write_bytes(sarc([("shared.msbt", raw)]))
            before = archive.read_bytes()
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(catalog.main([str(archive)]), 0)
            inventory = json.loads(output.getvalue())
            self.assertEqual(inventory["message_count"], 2)
            self.assertEqual(inventory["records"], [])
            destination = root / "catalog.json"
            self.assertEqual(catalog.main([str(archive), "--label", "two", "--output", str(destination)]), 0)
            record = json.loads(destination.read_text())["records"][0]
            self.assertEqual((record["archive"], record["member"], record["message_id"], record["index"]),
                             ("messages.sarc", "shared.msbt", "two", 1))
            self.assertEqual(record["text"], "second")
            self.assertEqual(record["utf16_sha256"], hashlib.sha256(text("second")).hexdigest())
            self.assertEqual(raw[record["text_offset"]:record["text_offset"] + record["byte_length"]], text("second"))
            self.assertEqual(archive.read_bytes(), before)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(catalog.main([str(archive), "--output", str(destination)]), 1)
            self.assertEqual(json.loads(destination.read_text())["records"][0]["text"], "second")

    def test_export_limit_and_shared_labels_keep_identities(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "messages.sarc"
            raw = msbt([text("A")], [("same", 0)])
            archive.write_bytes(sarc([("a.msbt", raw), ("b.msbt", raw)]))
            result = catalog.catalog_archive(archive, label="same", limit=1)
            self.assertEqual(result["selected_record_count"], 2)
            self.assertTrue(result["records_truncated"])
            self.assertEqual(len(catalog.catalog_archive(archive, label="same", limit=0)["records"]), 2)

    def test_cli_refuses_to_overwrite_the_input(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "original.msbt"
            raw = msbt([text("preserved")])
            path.write_bytes(raw)
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                catalog.main([str(path), "--output", str(path)])
            self.assertEqual(error.exception.code, 2)
            self.assertEqual(path.read_bytes(), raw)

    @unittest.skipUnless(shutil.which("zstd"), "zstd is not installed")
    def test_zstd_roundtrip_and_decompressed_limit(self):
        raw = sarc([("text.msbt", msbt([text("synthetic")], [("label", 0)]))])
        compressed = subprocess.run([shutil.which("zstd"), "-q", "-c"], input=raw,
                                    capture_output=True, check=True).stdout
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "archive.sarc.zs"
            path.write_bytes(compressed)
            self.assertEqual(catalog.read_resource(path), raw)
            with self.assertRaisesRegex(catalog.CatalogError, "read limit"):
                catalog.read_resource(path, max_bytes=4)

    def test_truncation_never_returns_partial_resources(self):
        raw = msbt([text("example")], [("label", 0)])
        archive = sarc([("dialogue.msbt", raw)])
        for data, parser in ((raw, catalog.parse_msbt), (archive, catalog.parse_sarc)):
            for size in range(len(data)):
                with self.subTest(parser=parser.__name__, size=size), self.assertRaises(catalog.CatalogError):
                    parser(data[:size])

    def test_raw_read_limit_is_enforced(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "raw"
            path.write_bytes(b"12345")
            with self.assertRaisesRegex(catalog.CatalogError, "read limit"):
                catalog.read_resource(path, max_bytes=4)


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def archive(self, members, filename="TalkNNpc_EUen.sarc"):
        path = self.root / filename
        files = [(name, msbt([text(value) for _, value in entries],
                            [(label, index) for index, (label, _) in enumerate(entries)]))
                 for name, entries in members]
        path.write_bytes(sarc(files))
        return path

    def test_exact_path_and_archive_prefix(self):
        path = self.archive([("B1_Bo/Free/BO_FreeC_RoomG.msbt", [("006", "example")])])
        for reference in ("TalkNNpc/B1_Bo/Free/BO_FreeC_RoomG:006", "B1_Bo/Free/BO_FreeC_RoomG:006"):
            with self.subTest(reference=reference):
                result = catalog.resolve_reference(path, reference)
                self.assertEqual((result["archive"], result["member"], result["message_id"], result["index"]),
                                 (path.name, "B1_Bo/Free/BO_FreeC_RoomG.msbt", "006", 0))
                self.assertEqual(result["message_reference"], reference)
                self.assertEqual(result["resolution"], "exact_member")

    def test_msbt_extension_normalizes_only_the_member_suffix(self):
        path = self.archive([("folder/entry.msbt", [("001", "example")])])
        for reference in ("TalkNNpc/folder/entry.msbt:001", "folder/entry.msbt:001", "entry.msbt:001"):
            self.assertEqual(catalog.resolve_reference(path, reference)["member"], "folder/entry.msbt")
        with self.assertRaises(catalog.CatalogError):
            catalog.resolve_reference(path, "entry.MSBT:001")

    def test_unique_basename_keeps_full_resource_identity(self):
        path = self.archive([("first/one.msbt", [("001", "first")]), ("second/two.msbt", [("001", "second")])])
        result = catalog.resolve_reference(path, "two:001")
        self.assertEqual(result["member"], "second/two.msbt")
        self.assertEqual(result["text"], "second")
        self.assertEqual(result["resolution"], "unique_basename")

    def test_ambiguous_basename_is_rejected_before_label_lookup(self):
        for names in (("first/shared.msbt", "second/shared.msbt"), ("shared.msbt", "second/shared.msbt")):
            path = self.archive([(names[0], [("001", "first")]), (names[1], [("002", "second")])])
            with self.subTest(names=names), self.assertRaisesRegex(catalog.CatalogError, "ambiguous"):
                catalog.resolve_reference(path, "shared:001")

    def test_duplicate_labels_across_members_need_the_file_identity(self):
        path = self.archive([("a/shared.msbt", [("same", "first")]), ("b/shared.msbt", [("same", "second")])])
        result = catalog.resolve_reference(path, "TalkNNpc/b/shared:same")
        self.assertEqual(result["member"], "b/shared.msbt")
        self.assertEqual(result["text"], "second")

    def test_unknown_prefix_does_not_fall_back_to_basename(self):
        path = self.archive([("folder/entry.msbt", [("001", "example")])])
        for reference in ("Unknown/folder/entry:001", "TalkSNpc/folder/entry:001", "Unknown/entry:001"):
            with self.subTest(reference=reference), self.assertRaisesRegex(catalog.CatalogError, "unknown prefixes"):
                catalog.resolve_reference(path, reference)

    def test_multiple_archive_matches_remain_ambiguous(self):
        members = [("folder/entry.msbt", [("001", "example")])]
        first = self.archive(members, "TalkNNpc_EUen.sarc")
        second = self.archive(members, "TalkNNpc_USen.sarc")
        with self.assertRaisesRegex(catalog.CatalogError, "ambiguous"):
            catalog.resolve_reference([first, second], "TalkNNpc/folder/entry:001")
        second = self.archive(members, "TalkSNpc_EUen.sarc")
        self.assertEqual(catalog.resolve_reference([first, second], "TalkNNpc/folder/entry:001")["archive"], first.name)

    def test_malformed_references_are_rejected(self):
        for reference in (None, "", "file", ":001", "file:", "file:one:two", " file:001", "file:001 ",
                          "/file:001", "folder//file:001", "folder/../file:001", "folder/./file:001",
                          "file\\part:001", "file:one\n", "file:\0"):
            with self.subTest(reference=reference), self.assertRaises(catalog.CatalogError):
                catalog.resolve_reference({}, reference)

    def test_complete_catalog_dictionary_and_json_file_are_supported(self):
        path = self.archive([("folder/entry.msbt", [("001", "example")])])
        exported = catalog.catalog_archive(path, include_messages=True, limit=0)
        # Extra fields cannot establish a speaker or eligibility through the resolver.
        exported["records"][0].update(npc_id="invented", eligible=True, flow_id="invented")
        result = catalog.resolve_reference(exported, "entry:001")
        self.assertNotIn("npc_id", result)
        self.assertNotIn("eligible", result)
        self.assertNotIn("flow_id", result)
        json_path = self.root / "catalog.json"
        json_path.write_text(json.dumps(exported))
        self.assertEqual(catalog.resolve_reference(json_path, "entry:001")["text"], "example")

    def test_truncated_or_inventory_only_catalog_is_rejected(self):
        path = self.archive([("folder/entry.msbt", [("001", "one"), ("002", "two")])])
        for exported in (catalog.catalog_archive(path), catalog.catalog_archive(path, include_messages=True, limit=1)):
            with self.subTest(exported=exported["records_truncated"]), self.assertRaises(catalog.CatalogError):
                catalog.resolve_reference(exported, "entry:001")
        selected = catalog.catalog_archive(path, member="folder/entry.msbt", include_messages=True, limit=0)
        with self.assertRaisesRegex(catalog.CatalogError, "member-filtered"):
            catalog.resolve_reference(selected, "entry:001")
        self.assertEqual(catalog.resolve_reference(selected, "folder/entry:001")["member"], "folder/entry.msbt")

    def test_cli_reference_returns_one_complete_record(self):
        path = self.archive([("folder/entry.msbt", [("001", "example")])])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(catalog.main([str(path), "--reference", "TalkNNpc/folder/entry:001"]), 0)
        self.assertEqual(json.loads(output.getvalue())["member"], "folder/entry.msbt")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(catalog.main([str(path), "--reference", "folder/entry:missing"]), 1)


if __name__ == "__main__":
    unittest.main()
