"""Validate or replay captured dialogue events without modifying the game's files.

python3 -m tools.villager_chat validate private/dialogue/events.jsonl
python3 -m tools.villager_chat replay private/dialogue/events.jsonl --profile private/dialogue/profile.json

Generation requires explicit --endpoint and --model arguments. The endpoint is the complete chat-completions URL.
Without them, replay emits original dialogue as fallback. API keys come from VILLAGER_CHAT_API_KEY by default.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

from .backend import HttpBackend
from .core import DialogueEvent, DialogueService, MAX_EVENT_BYTES, MemoryStore, Profile, ValidationError, atomic_json, canonical_json


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("validate", "replay"))
    parser.add_argument("events", help="a JSONL file, or - for standard input")
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--memory-dir", type=Path)
    parser.add_argument("--endpoint", help="explicit chat-completions URL; no default server")
    parser.add_argument("--model")
    parser.add_argument("--api-key-env", default="VILLAGER_CHAT_API_KEY")
    args = parser.parse_args(argv)
    if bool(args.endpoint) != bool(args.model):
        parser.error("--endpoint and --model must be supplied together")
    if args.command == "validate" and (args.endpoint or args.memory_dir or args.output_dir):
        parser.error("validate does not call a model or write files")
    try:
        profile = None
        if args.profile:
            with args.profile.open("rb") as stream:
                raw = stream.read(MAX_EVENT_BYTES + 1)
            if len(raw) > MAX_EVENT_BYTES:
                raise ValidationError("operator profile exceeds its byte limit")
            profile = Profile.parse(json.loads(raw))
        if args.memory_dir and profile is None:
            parser.error("--memory-dir requires --profile")
        backend = HttpBackend(args.endpoint, args.model, os.environ.get(args.api_key_env)) if args.endpoint else None
        memory = MemoryStore(args.memory_dir, profile) if args.memory_dir else None
        service = DialogueService(profile, backend, memory)
    except (OSError, ValueError, UnicodeError):
        print("villager_chat: invalid profile or backend configuration", file=sys.stderr)
        return 2
    failed = False
    try:
        source = sys.stdin.buffer if args.events == "-" else open(args.events, "rb")
    except OSError:
        print("villager_chat: cannot open event input", file=sys.stderr)
        return 2
    try:
        line_number = 0
        while True:
            raw = source.readline(MAX_EVENT_BYTES + 1)
            if not raw:
                break
            line_number += 1
            try:
                if len(raw) > MAX_EVENT_BYTES:
                    raise ValidationError("event exceeds its byte limit")
                event = DialogueEvent.parse(json.loads(raw))
                if args.command == "validate":
                    result = {"schema_version": 1, "event_id": event.event_id, "context_fingerprint": event.fingerprint,
                              "valid": True, "allowed": profile is not None and profile.rejection(event) is None}
                else:
                    result = service.process(event)
                    if args.output_dir:
                        name = hashlib.sha256(event.event_id.encode("utf-8")).hexdigest() + ".json"
                        atomic_json(args.output_dir / name, result)
                sys.stdout.buffer.write(canonical_json(result) + b"\n")
                sys.stdout.buffer.flush()
            except (OSError, ValueError, UnicodeError):
                print(f"villager_chat: invalid event or output at line {line_number}", file=sys.stderr)
                failed = True
                if len(raw) > MAX_EVENT_BYTES:
                    # Consume this line without allocating an unbounded buffer.
                    while raw and not raw.endswith(b"\n"):
                        raw = source.readline(MAX_EVENT_BYTES + 1)
    finally:
        if source is not sys.stdin.buffer:
            source.close()
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
