#!/usr/bin/env python3
"""Read a local message archive without changing game files.

The default output contains counts. Use --label for one message label.
Use --reference file:label to resolve one resource.
Use --messages --limit 0 --output private/dialogue.json to export a local catalog.
Archive and member names identify resources. They do not establish the speaker or event type.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

MAX_BYTES = 256 * 1024 * 1024


class CatalogError(ValueError):
    """The input does not contain a supported, complete message resource."""


def _range(data, offset, size, what):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise CatalogError(f"{what}: range outside the resource")
    return data[offset:offset + size]


def _unpack(fmt, data, offset, what):
    _range(data, offset, struct.calcsize(fmt), what)
    return struct.unpack_from(fmt, data, offset)


def _endian(data, offset, what):
    bom = _range(data, offset, 2, what)
    if bom == b"\xff\xfe":
        return "<"
    if bom == b"\xfe\xff":
        return ">"
    raise CatalogError(f"{what}: invalid byte order marker")


def _utf8(data, what):
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise CatalogError(f"{what}: invalid UTF-8") from exc


def parse_sarc(data):
    """Return named member ranges. No member is extracted to disk."""
    if _range(data, 0, 4, "SARC magic") != b"SARC":
        raise CatalogError("SARC magic is missing")
    endian = _endian(data, 6, "SARC")
    header_size, = _unpack(endian + "H", data, 4, "SARC header size")
    file_size, data_offset = _unpack(endian + "II", data, 8, "SARC file range")
    if header_size < 20 or file_size < header_size or file_size > len(data):
        raise CatalogError("SARC header or file size is invalid")
    data = data[:file_size]
    if _range(data, header_size, 4, "SFAT magic") != b"SFAT":
        raise CatalogError("SFAT magic is missing")
    sfat_size, count = _unpack(endian + "HH", data, header_size + 4, "SFAT header")
    if sfat_size < 12:
        raise CatalogError("SFAT header is too small")
    nodes_at = header_size + sfat_size
    _range(data, nodes_at, count * 16, "SFAT nodes")
    sfnt_at = nodes_at + count * 16
    if _range(data, sfnt_at, 4, "SFNT magic") != b"SFNT":
        raise CatalogError("SFNT magic is missing")
    sfnt_size, = _unpack(endian + "H", data, sfnt_at + 4, "SFNT header")
    names_at = sfnt_at + sfnt_size
    if sfnt_size < 8 or not names_at <= data_offset <= file_size:
        raise CatalogError("SARC names or data offset is invalid")
    names = set()
    members = []
    for index in range(count):
        hash_value, attr, start, end = _unpack(endian + "IIII", data, nodes_at + index * 16, "SFAT node")
        if attr >> 24 not in (0, 1):
            raise CatalogError("SARC member name flag is invalid")
        if attr >> 24:
            name_at = names_at + (attr & 0xFFFFFF) * 4
            if not names_at <= name_at < data_offset:
                raise CatalogError("SARC member name offset is invalid")
            name_end = data.find(b"\0", name_at, data_offset)
            if name_end < 0:
                raise CatalogError("SARC member name is not terminated")
            name = _utf8(data[name_at:name_end], "SARC member name")
            if not name or name in names:
                raise CatalogError("SARC member name is empty or duplicated")
            names.add(name)
        else:
            name = f"@{hash_value:08x}:{index}"
        if not 0 <= start <= end <= file_size - data_offset:
            raise CatalogError("SARC member data range is invalid")
        members.append({"name": name, "start": data_offset + start, "end": data_offset + end})
    return members


def decode_text(raw, endian):
    """Decode one terminated UTF-16 message and retain exact control payloads."""
    codec = "utf-16-le" if endian == "<" else "utf-16-be"
    parts, text, run = [], [], bytearray()
    run_at = 0
    offset = 0

    def flush():
        if not run:
            return
        try:
            value = bytes(run).decode(codec)
        except UnicodeDecodeError as exc:
            raise CatalogError("TXT2 contains invalid UTF-16") from exc
        parts.append({"kind": "text", "offset": run_at, "text": value})
        text.append(value)
        run.clear()

    while offset < len(raw):
        unit, = _unpack(endian + "H", raw, offset, "TXT2 code unit")
        if unit == 0:
            flush()
            return {"text": "".join(text), "parts": parts, "byte_length": offset + 2}
        if unit in (0x0E, 0x0F):
            flush()
            group, kind = _unpack(endian + "HH", raw, offset + 2, "TXT2 control header")
            if unit == 0x0E:
                parameter_size, = _unpack(endian + "H", raw, offset + 6, "TXT2 control size")
                payload = _range(raw, offset + 8, parameter_size, "TXT2 control payload")
                size = 8 + parameter_size
            else:
                payload, size = b"", 6
            parts.append({"kind": "control", "offset": offset, "marker": unit,
                          "group": group, "type": kind, "parameters_hex": payload.hex(), "byte_length": size})
            offset += size
        else:
            if not run:
                run_at = offset
            run.extend(raw[offset:offset + 2])
            offset += 2
    raise CatalogError("TXT2 message is not terminated")


def _labels(data, endian, text_count):
    count, = _unpack(endian + "I", data, 0, "LBL1 bucket count")
    table_size = 4 + count * 8
    _range(data, 0, table_size, "LBL1 bucket table")
    labels, used, spans = {}, set(), []
    for bucket in range(count):
        entries, offset = _unpack(endian + "II", data, 4 + bucket * 8, "LBL1 bucket")
        if not entries:
            continue
        if offset < table_size:
            raise CatalogError("LBL1 entries overlap the bucket table")
        for _ in range(entries):
            start = offset
            length, = _unpack("B", data, offset, "LBL1 label length")
            offset += 1
            label = _utf8(_range(data, offset, length, "LBL1 label"), "LBL1 label")
            offset += length
            index, = _unpack(endian + "I", data, offset, "LBL1 text index")
            offset += 4
            if not label or label in used:
                raise CatalogError("LBL1 label is empty or duplicated")
            if index >= text_count:
                raise CatalogError("LBL1 text index is outside TXT2")
            used.add(label)
            labels.setdefault(index, []).append(label)
            spans.append((start, offset))
    spans.sort()
    if any(right[0] < left[1] for left, right in zip(spans, spans[1:])):
        raise CatalogError("LBL1 entries overlap")
    return labels


def parse_msbt(data):
    """Return labels, TXT2 indices, visible text, and controls for one MSBT."""
    if _range(data, 0, 8, "MSBT magic") != b"MsgStdBn":
        raise CatalogError("MSBT magic is missing")
    endian = _endian(data, 8, "MSBT")
    section_count, = _unpack(endian + "H", data, 14, "MSBT section count")
    file_size, = _unpack(endian + "I", data, 18, "MSBT file size")
    _range(data, 0, 32, "MSBT header")
    if data[12] != 1:
        raise CatalogError("MSBT encoding is not UTF-16")
    if file_size < 32 or file_size > len(data):
        raise CatalogError("MSBT file size is invalid")
    data = data[:file_size]
    sections, section_offsets = {}, {}
    offset = 32
    for _ in range(section_count):
        tag = _range(data, offset, 4, "MSBT section magic")
        size, = _unpack(endian + "I", data, offset + 4, "MSBT section size")
        _range(data, offset, 16, "MSBT section header")
        payload = _range(data, offset + 16, size, "MSBT section payload")
        if tag in (b"TXT2", b"LBL1"):
            if tag in sections:
                raise CatalogError("MSBT contains a duplicate text or label section")
            sections[tag] = payload
            section_offsets[tag] = offset + 16
        offset = (offset + 16 + size + 15) & ~15
    if b"TXT2" not in sections:
        raise CatalogError("MSBT TXT2 section is missing")
    txt = sections[b"TXT2"]
    count, = _unpack(endian + "I", txt, 0, "TXT2 string count")
    table_size = 4 + count * 4
    _range(txt, 0, table_size, "TXT2 offset table")
    offsets = list(_unpack(endian + f"{count}I", txt, 4, "TXT2 offsets"))
    if any(at < table_size or at >= len(txt) for at in offsets):
        raise CatalogError("TXT2 string offset is invalid")
    unique = sorted(set(offsets))
    ends = dict(zip(unique, unique[1:] + [len(txt)]))
    decoded = {}
    for at in unique:
        value = decode_text(txt[at:ends[at]], endian)
        value["text_offset"] = section_offsets[b"TXT2"] + at
        value["utf16_sha256"] = hashlib.sha256(txt[at:at + value["byte_length"]]).hexdigest()
        decoded[at] = value
    labels = _labels(sections[b"LBL1"], endian, count) if b"LBL1" in sections else {}
    messages = [{"index": index, "labels": labels.get(index, []), **decoded[at]} for index, at in enumerate(offsets)]
    return {"endian": "little" if endian == "<" else "big", "message_count": count,
            "label_count": sum(map(len, labels.values())), "messages": messages}


def read_resource(path, max_bytes=MAX_BYTES):
    """Read raw bytes or one zstd stream with a bounded decompressed size."""
    path = Path(path)
    if path.suffix != ".zs":
        with path.open("rb") as stream:
            data = stream.read(max_bytes + 1)
    else:
        executable = shutil.which("zstd")
        if executable:
            with subprocess.Popen([executable, "-d", "-q", "-c", str(path)], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE) as process:
                data = process.stdout.read(max_bytes + 1)
                if len(data) > max_bytes:
                    process.kill()
                _, errors = process.communicate()
                if len(data) <= max_bytes and process.returncode:
                    raise CatalogError("zstd cannot decode the resource: " + errors.decode("utf-8", "replace").strip())
        else:
            try:
                import zstandard
            except ImportError as exc:
                raise CatalogError("Install zstd, or use an existing Python environment with zstandard") from exc
            with path.open("rb") as stream, zstandard.ZstdDecompressor().stream_reader(stream) as decoded:
                data = decoded.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise CatalogError(f"Resource exceeds the {max_bytes}-byte read limit")
    return data


def catalog_archive(path, member=None, label=None, include_messages=False, limit=20):
    if limit < 0:
        raise CatalogError("The record limit must be nonnegative")
    data = read_resource(path)
    source = {"archive": Path(path).name, "source_path": str(Path(path).resolve()),
              "decoded_archive_sha256": hashlib.sha256(data).hexdigest()}
    members = parse_sarc(data) if data[:4] == b"SARC" else [{"name": Path(path).name, "start": 0, "end": len(data)}]
    files, records, record_count = [], [], 0
    for entry in members:
        if member is not None and entry["name"] != member:
            continue
        raw = data[entry["start"]:entry["end"]]
        if raw[:8] != b"MsgStdBn":
            continue
        parsed = parse_msbt(raw)
        files.append({"member": entry["name"], "message_count": parsed["message_count"],
                      "label_count": parsed["label_count"], "endian": parsed["endian"],
                      "member_sha256": hashlib.sha256(raw).hexdigest(), "member_offset": entry["start"]})
        if not (include_messages or label is not None):
            continue
        for message in parsed["messages"]:
            identities = message["labels"] or [None]
            for identity in identities:
                if label is not None and identity != label:
                    continue
                record_count += 1
                if limit and len(records) >= limit:
                    continue
                records.append({"archive": source["archive"], "member": entry["name"], "message_id": identity,
                                "index": message["index"], "text": message["text"], "parts": message["parts"],
                                "byte_length": message["byte_length"], "text_offset": message["text_offset"],
                                "utf16_sha256": message["utf16_sha256"]})
    if member is not None and not files:
        raise CatalogError("The selected MSBT member is absent")
    return {**source, "member_count": len(files), "message_count": sum(f["message_count"] for f in files),
            "label_count": sum(f["label_count"] for f in files), "members": files,
            "selected_record_count": record_count, "records": records, "records_truncated": len(records) < record_count,
            "selection": {"member": member, "label": label}}


def _reference_parts(reference):
    if not isinstance(reference, str) or reference.count(":") != 1 or reference.strip() != reference:
        raise CatalogError("A message reference must contain one file:label pair")
    if any(ord(char) < 32 or ord(char) == 127 for char in reference) or "\\" in reference:
        raise CatalogError("A message reference contains a control character or backslash")
    file_id, label = reference.split(":")
    if not file_id or not label or any(part in ("", ".", "..") for part in file_id.split("/")):
        raise CatalogError("A message reference has an empty or invalid file or label")
    return file_id, label


def _member_id(name):
    return name[:-5] if name.endswith(".msbt") else name


def _archive_group(name):
    # The observed reference prefix omits the archive's region/language suffix.
    for suffix in (".sarc.zs", ".sarc", ".msbt"):
        if name.endswith(suffix):
            name = name[:-len(suffix)]
            break
    return re.sub(r"_[A-Z]{2}[a-z]{2}$", "", name)


def _reference_catalog(source):
    if isinstance(source, dict):
        result = source
    else:
        path = Path(source)
        if path.is_dir():
            raise CatalogError("Select one archive or catalog file, not a directory")
        if path.suffix == ".json":
            try:
                result = json.loads(read_resource(path))
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                raise CatalogError("The catalog file is not valid JSON") from exc
        else:
            result = catalog_archive(path, include_messages=True, limit=0)
    if not isinstance(result, dict) or not isinstance(result.get("archive"), str) or not result["archive"]:
        raise CatalogError("The catalog archive identity is missing")
    if result.get("records_truncated") is not False:
        raise CatalogError("The catalog is truncated; export all selected messages with --limit 0")
    members, records = result.get("members"), result.get("records")
    if not isinstance(result.get("selection", {}), dict):
        raise CatalogError("The catalog selection metadata is invalid")
    if not isinstance(members, list) or not isinstance(records, list):
        raise CatalogError("The catalog members or records are missing")
    counts, indices, labels = {}, {}, {}
    for member in members:
        if not isinstance(member, dict) or not isinstance(member.get("member"), str):
            raise CatalogError("The catalog contains an invalid member")
        name, count, label_count = member["member"], member.get("message_count"), member.get("label_count")
        if (not name or name in counts or type(count) is not int or count < 0 or
                type(label_count) is not int or label_count < 0):
            raise CatalogError("The catalog member identity or counts are invalid")
        counts[name] = (count, label_count)
        indices[name], labels[name] = set(), set()
    for record in records:
        if not isinstance(record, dict) or record.get("archive") != result["archive"]:
            raise CatalogError("The catalog record archive identity is invalid")
        name, index, label = record.get("member"), record.get("index"), record.get("message_id")
        if (not isinstance(name, str) or name not in counts or type(index) is not int or
                not 0 <= index < counts[name][0] or not (label is None or isinstance(label, str) and label)):
            raise CatalogError("The catalog record identity is invalid")
        indices[name].add(index)
        if label is not None:
            if label in labels[name]:
                raise CatalogError("The catalog contains a duplicate label")
            labels[name].add(label)
    if any(len(indices[name]) != count or len(labels[name]) != label_count
           for name, (count, label_count) in counts.items()):
        raise CatalogError("The catalog does not contain every member's message records")
    return result


def resolve_reference(source, reference):
    """Resolve one file:label reference in an archive, complete catalog, or list of those sources.

    Prefixes match the selected archive group exactly. An unqualified basename must identify one member before
    label lookup. A matching resource establishes no speaker, event type, or permission to replace dialogue.
    """
    file_id, label = _reference_parts(reference)
    sources = source if isinstance(source, (list, tuple)) else [source]
    candidates = []
    for item in sources:
        archive = _reference_catalog(item)
        requested = file_id
        prefix = _archive_group(archive["archive"]) + "/"
        qualified = requested.startswith(prefix)
        if qualified:
            requested = requested[len(prefix):]
        requested = _member_id(requested)
        if "/" not in file_id:
            if archive.get("selection", {}).get("member") is not None:
                raise CatalogError("A member-filtered catalog cannot establish a unique basename")
            exact = [m["member"] for m in archive["members"]
                     if _member_id(m["member"].rsplit("/", 1)[-1]) == requested]
            resolution = "unique_basename"
        else:
            exact = [m["member"] for m in archive["members"] if _member_id(m["member"]) == requested]
            resolution = "exact_member"
        candidates.extend((archive, name, resolution) for name in exact)
    if len(candidates) != 1:
        if candidates:
            raise CatalogError("The message reference is ambiguous across members or archives")
        raise CatalogError("The message reference file is absent; unknown prefixes are not stripped")
    archive, member, resolution = candidates[0]
    matches = [record for record in archive["records"] if record["member"] == member and record["message_id"] == label]
    if len(matches) != 1:
        raise CatalogError("The message reference label is absent or ambiguous")
    fields = ("archive", "member", "message_id", "index", "text", "parts", "byte_length", "text_offset", "utf16_sha256")
    if any(key not in matches[0] for key in fields):
        raise CatalogError("The catalog record lacks text or source metadata")
    return {**{key: matches[0][key] for key in fields}, "message_reference": reference,
            "reference_file_id": file_id, "message_label": label, "resolution": resolution}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="RomFS directory, message directory, SARC archive, or MSBT file")
    parser.add_argument("--archive", default="TalkNNpc_EUen.sarc.zs", help="Archive filename when path is a directory")
    parser.add_argument("--member", help="Exact MSBT member name")
    parser.add_argument("--label", help="Exact message label; includes the selected text")
    parser.add_argument("--reference", help="Resolve one file:label reference and return its full message identity")
    parser.add_argument("--messages", action="store_true", help="Include message text and control metadata")
    parser.add_argument("--limit", type=int, default=20, help="Maximum returned records; 0 returns all selected records")
    parser.add_argument("--output", type=Path, help="Write JSON to a new file instead of standard output")
    args = parser.parse_args(argv)
    if args.limit < 0:
        parser.error("--limit must be nonnegative")
    if args.reference is not None and (args.member is not None or args.label is not None or args.messages):
        parser.error("--reference cannot be combined with --member, --label, or --messages")
    path = args.path
    if path.is_dir():
        path = (path / "Message" if (path / "Message").is_dir() else path) / args.archive
    if args.output is not None and args.output.resolve() == path.resolve():
        parser.error("The output path must differ from the input resource")
    try:
        result = (resolve_reference(path, args.reference) if args.reference is not None else
                  catalog_archive(path, args.member, args.label, args.messages, args.limit))
        if args.label is not None and not result["selected_record_count"]:
            raise CatalogError("The selected message label is absent")
        output = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(output)
        else:
            sys.stdout.write(output)
    except (CatalogError, OSError, struct.error) as exc:
        print(f"dialogue_catalog: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
