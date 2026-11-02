# Villager dialogue prototype

This branch contains a read-only native tracer and a separate Python service for dialogue experiments.
The tracer observes message references, execution flows, and raw text in ACNH 3.0.3.
The service can replay explicitly allowed events through a configured model server.
The game does not consume the service replies.

The game retains all story decisions, rewards, relationships, and save writes.
The native tracer never writes guest memory.
Tracing remains off unless `NX_DIALOGUE_TRACE=1`.
No model server, model download, or API credential is configured by default.

## Native observations

The hooks support main build ID `FF1D1C05670DB6021C85B624A710B963` only.
The runtime also compares instructions at the observed boundaries with expected values.
A mismatch disables capture.
The hook manifest contains these addresses only for version 3.0.3.

The following addresses are offsets from the main module:

| Address | Observed behavior | Captured data |
| --- | --- | --- |
| `0x9d8430` | Shared message-reference parser entry | Output objects, input reference, caller |
| `0x9d85bc` | Common successful parser return | Parsed file, label, optional actor candidates |
| `0x3c4ce3c` | Return from MSBT label lookup | Filename, label, UTF-16 span |
| `0x60f4f0` | Action dispatch entry | Executing flow and initial entrypoint |
| `0x60f7c4` | Common dispatch epilogue | End of the synchronous observation scope |

MSBT is the format of the game message files.
The parser receives string objects with data pointers at offset `+8`.
At the text boundary, the span pointer is at `SP+0x18` and its signed count is at `SP+0x20`.
The filename is at `SP+0x2c`, and `X20` identifies the label object.
The next routine copies UTF-16 units and processes message controls.

The tracer copies at most 128 UTF-16 units from each span.
It records those bytes as hexadecimal text and marks longer spans as truncated.
These bytes include control tags and unexpanded variables.
They do not represent the final text on the screen.

The tracer reads an actor candidate only when the parser arguments match the observed message action layout.
The action points to a context at `+8`, and that context points to an actor at `+0x48`.
A nearby action reads a string object at actor offset `+0x3e0` for an NPC-dependent message reference.
That evidence does not establish a unique villager identity.
Context string candidates also remain uninterpreted.

The dispatch receives an argument in `X2` with a context pointer at `+8` and signed thread index at `+0x10`.
The context holds its thread array at `+0x38` and signed slot count at `+0x40`.
Each slot occupies `0x20` bytes and points to a runtime flow record.
That record points to an EVFL resource, the compiled flowchart format.
The tracer requires the `EVFL` signature and copies the resource name through `resource+0x20`.
The name record contains a two-byte length followed by the name bytes.

The tracer requires a matching generation in the argument and thread slot.
A generation distinguishes reuse of the same execution slot.
It also compares the dispatched action pointer with the selected event in the resource.
The event index comes from the thread slot, and the resource supplies the event count and name.
The tracer rejects a pointer mismatch, an invalid index, or a node that is not an action.
It limits the slot count to 256 and copied names to 128 ASCII bytes.
It associates the copied flow with a message only inside the same synchronous dispatch and receptor.
The dispatch scope does not establish the lifetime of a visible conversation.
Nonlocal unwinds remain untested, and scope cleanup does not make delayed replies eligible.

The context also stores initial flow and entrypoint indices at `+0x88` and `+0x8c`.
The tracer bounds those indices against the resource array and entry dictionary.
It records `initial_flow_id` and `initial_entrypoint` separately from the currently executing `flow_id`.
A story event can call another flow, so the current flow alone cannot establish eligibility.
The entrypoint of the current subflow remains unresolved.

Both record kinds use one run identifier.
Sequence numbers are separate for each kind.
Each kind writes at most 10,000 records per run.
The runtime bounds string reads and rejects unreadable memory.
Every record contains `speaker_verified:false` and `eligible:false`.
A reference record contains a `flow_id` only when the scoped resource lookup succeeds.
An unbound record keeps that field null.

Rebuild the generated game code after changing `games/acnh/hooks-3.0.3.txt`.
The normal importer and setup process includes the hook manifest.
An executable built with an older manifest cannot capture these locations.

With a rebuilt executable and an isolated data directory, start tracing from a POSIX shell:

```sh
NX_DIALOGUE_TRACE=1 NX_LOG=private/dialogue.log /path/to/acnh /path/to/isolated-data
```

Use a separate data directory for automated input tests.
Do not point an automated test at your normal save directory.
Trace lines start with `dialogue_probe:` inside the runtime log.
Keep traces under `private/` because they contain game text and process addresses.

## Local evidence

An isolated native test on macOS exited with status zero after 30 seconds.
The test used a fresh data directory and automatic A presses.
It captured two tutorial message references and 379 text spans.
The actor candidate was `rcm`, and one context candidate was `EventFlowReceptor`.
The actor dispatch table was `main+0x495d200`.
Its method at `main+0x1d47f20` returns the class name `NpcActor`.
The localized special-character name resource resolves `rcm` to Timmy.
These observations do not establish a normal villager or prove that every word belongs to the owning actor.

The two references resolved to labels in the local `Tutorials_EUen.sarc.zs` archive.
The captured text prefixes matched the source bytes for those labels.
These observations establish resource lookup during the tutorial.
They do not establish a safe place to replace casual villager dialogue.

A second run reproduced those captures with a shared run identifier.
A run with tracing disabled also exited with status zero and emitted no probe records.
These startup tests do not establish full gameplay correctness or performance equivalence.

A later native run captured `Tutorials_Prologue1_Charactermaking` as both the executing and initial flow, with initial entrypoint `Root`.
The message references still pointed to airport text files.
This demonstrates why the message filename cannot identify the active flow.
Both records remained ineligible, and the process exited with status zero.
The final capture also identified action nodes `Event69` and `Event39` at event indices 15 and 32.
Their names and message parameters matched the local flowchart resource.

The native tests reused existing compiled game archives with three local generated files that included the new hooks.
Those generated files and game archives remain private.
The normal full importer rebuild and native behavior on other platforms remain untested for this branch.

## Character resources

The local `NmlNpcParam.bcsv` table contains 417 distinct normal-character labels.
The special-character table uses separate labels, but its numeric identifiers overlap the normal table in 73 cases.
Model resource names also overlap between distinct characters.
The prototype must retain the table family and textual label when it binds an observed character to a profile.

Each normal row contains a value that matches one of eight case-sensitive enum literals in the matching main image.
The literals include `Boy_normal`, `Boy_active`, and `Girl_big_sis`.
This exact resource join establishes candidate source values, but the native personality consumer remains unbound.
The prototype does not assign friendly personality descriptions from unknown fields.
Operator-supplied demeanor remains an explicit editorial choice.

Localized name and phrase resources use the same character labels.
Those phrases are authored defaults rather than proof of the current player-customized catchphrase.
Current resident membership and the native speaker-to-table binding remain unresolved.
The prototype must establish both before enabling a normal villager.

## Candidate conversation

Static analysis found one candidate in `NNPC_Free2_House`, under entrypoint `Free_HouseG`.
Event index 62, named `Event70`, opens `TalkNNpc/B1_Bo/Free/BO_FreeC_RoomG:006`.
The local path first sets an NPC name, then selects one of three messages through the original random-choice action.
Only this entrypoint reaches that event within the parsed resource.
The same message also occurs in another flow, so its reference alone cannot permit replacement.

The original path must retain its name setup, random-choice state, and window-close behavior.
The source text contains a dynamic name and control tags.
Their semantics, the wider caller path, and the concrete villager binding remain unresolved.
The action owner `MainNpc` does not establish that every word belongs to one character.
The candidate remains ineligible until native observations and a control-preserving replacement establish those bindings.

## Message catalog

`tools/dialogue_catalog.py` reads local SARC archives and MSBT files.
SARC is the archive format that groups message files.
The tool preserves archive names, member names, labels, controls, byte offsets, and source hashes.
It rejects ambiguous references and incomplete catalogs.
Resource identity alone does not establish a speaker or permission to replace a message.

For an inventory, run this command from the repository root:

```sh
python3 tools/dialogue_catalog.py /path/to/romfs/Message/TalkNNpc_EUen.sarc.zs
```

For one message, supply its exact `file:label` reference:

```sh
python3 tools/dialogue_catalog.py /path/to/archive.sarc.zs \
  --reference 'ArchiveGroup/Member:Label' --output private/selected-message.json
```

For a complete local catalog, use `--messages --limit 0 --output private/catalog.json`.
The output path must name a new file.
Compressed archives require `zstd` or a Python environment that already contains `zstandard`.
Do not commit catalogs or extracted game text.

## Replay service

`tools.villager_chat` requires Python 3.10 or later.
It consumes operator-supplied JSONL events, with one JSON object per line.
Raw native trace records do not satisfy this event schema.
No tool currently converts an unverified native record into an eligible service event.

Each event requires these fields:

```json
{
  "schema_version": 1,
  "event_id": "synthetic-event-1",
  "conversation_id": "synthetic-conversation-1",
  "generation": 1,
  "island_id": "synthetic-island",
  "save_id": "synthetic-save",
  "npc_id": "synthetic-villager",
  "initial_flow_id": "synthetic-conversation-flow",
  "initial_entrypoint": "synthetic-casual-entry",
  "flow_id": "synthetic-casual-flow",
  "message_id": "synthetic-greeting",
  "text": "Hello there.",
  "language": "en-US",
  "observed_facts": [],
  "eligible": true
}
```

These identifiers are synthetic examples.
They do not identify game resources or establish a safe native conversation.
Save the example as one line in `private/events.jsonl`.

A profile fixes one island, save lineage, villager, language, identity, and demeanor.
A save lineage identifies one history of a save, including its restores.
The caller must change `save_id` when restoring an older save to prevent memory from a later history.
The profile permits exact combinations of initial flow, initial entrypoint, executing flow, and message identifier.
The service rejects older allowlist entries that omit the initial story context.
It does not support prefix matching or a general category such as all free conversations.

Save this synthetic profile as `private/profile.json`:

```json
{
  "schema_version": 1,
  "island_id": "synthetic-island",
  "save_id": "synthetic-save",
  "npc_id": "synthetic-villager",
  "language": "en-US",
  "identity": "A fictional villager who likes gardening.",
  "demeanor": "Quiet, warm, and concise. Uses gentle humor.",
  "allowlist": [
    {
      "initial_flow_id": "synthetic-conversation-flow",
      "initial_entrypoint": "synthetic-casual-entry",
      "flow_id": "synthetic-casual-flow",
      "message_id": "synthetic-greeting"
    }
  ],
  "trusted_game_sources": [],
  "max_utf16_units": 240,
  "timeout_seconds": 3,
  "memory_limit": 32
}
```

To make sure that the event matches the schema and profile, run:

```sh
python3 -m tools.villager_chat validate private/events.jsonl --profile private/profile.json
```

To exercise fallback without a model call, run:

```sh
python3 -m tools.villager_chat replay private/events.jsonl --profile private/profile.json \
  --output-dir private/replies
```

Generation requires both `--endpoint` and `--model`.
The endpoint must be the complete URL of a chat-completions-compatible server.
Only loopback servers can use HTTP.
Remote servers require HTTPS, and the client rejects redirects.
If the server requires a key, place it in `VILLAGER_CHAT_API_KEY` before starting replay.
The service sends the profile, original dialogue, allowed observations, and selected memories to that server.

The service permits one outstanding model request.
A timeout, invalid output, missing backend, or scope mismatch returns the original dialogue.
A timed-out worker cannot publish its later answer.
The service rejects a reply when its current-event callback reports a different conversation or event.
Any future game consumer must compare the complete response identity and context fingerprint before use.
A context fingerprint is a hash of every event field.

Replies contain plain text only.
The service rejects controls, markup, line breaks, and text beyond the profile limit.
These rules constrain text shape, but they do not prove that a reply preserves personality or story facts.
The current prototype contains no semantic acceptance test against a real model.

Optional memory lives outside the game save.
Each observation contains `kind`, `value`, and `source`.
The service stores player claims as claims and accepts game observations only from sources named in the profile.
Generated dialogue never becomes a stored fact.
The service separates memory by island, save lineage, and villager.

## Tests and remaining work

The Python tests use synthetic resources, fake model responses, and temporary directories.
The C++ test uses synthetic memory and makes sure that capture does not change it.
Run the Python suite from the repository root:

```sh
python3 -m unittest tests.test_dialogue_catalog tests.test_villager_chat
```

A CMake build with `NX_TESTS=ON` provides the `dialogue_probe_test` target.
CTest runs `dialogue_probe` and, when Python 3.10 is available, `dialogue_tools`.
These tests cover resource bounds, exact scope, fallback, stale replies, Unicode limits, and memory separation.
The current suite contains 54 Python tests and the C++ capture test.
The C++ capture test also passed with address and undefined-behavior sanitizers.

Native generation requires evidence for the active villager and conversation lifetime.
The tracer now observes active and initial flows, but those observations alone do not permit generation.
It also requires a proven replacement boundary that preserves message controls and buffer ownership.
The next native experiment must use one reviewed casual conversation and retain the original dialogue on every failure.
Story events and choices must remain outside that experiment.
Model testing must measure personality drift, invented facts, prompt injection, and latency before broader use.
