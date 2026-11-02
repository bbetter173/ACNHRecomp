#include "dialogue_probe.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace nx::acnh::dialogue {
namespace {

template <typename T>
bool field(ReadMemory read, void* user, uint64_t base, uint64_t offset, T& out) {
    return base && base <= std::numeric_limits<uint64_t>::max() - offset &&
           read(user, base + offset, &out, sizeof out);
}

bool ascii(ReadMemory read, void* user, uint64_t address, size_t limit, std::string& out) {
    if (!address || address > std::numeric_limits<uint64_t>::max() - limit) return false;
    std::string value;
    for (size_t i = 0; i <= limit; i++) {
        uint8_t ch;
        if (!read(user, address + i, &ch, 1)) return false;
        if (!ch) { out = std::move(value); return true; }
        if (ch < 0x20 || ch > 0x7e) return false;
        value += char(ch);
    }
    return false;
}

bool safe_string(ReadMemory read, void* user, uint64_t object, size_t limit, std::string& out) {
    uint64_t p;
    return field(read, user, object, 8, p) && ascii(read, user, p, limit, out);
}

bool resource_string(ReadMemory read, void* user, uint64_t record, std::string& out) {
    uint16_t length = 0;
    return field(read, user, record, 0, length) && length && length <= 128 &&
           record <= std::numeric_limits<uint64_t>::max() - 2 &&
           ascii(read, user, record + 2, length, out) && out.size() == length;
}

bool flow_name(ReadMemory read, void* user, uint64_t resource, std::string& out) {
    uint32_t magic = 0;
    uint64_t name = 0;
    return field(read, user, resource, 0, magic) && magic == 0x4c465645 &&
           field(read, user, resource, 0x20, name) && resource_string(read, user, name, out);
}

std::string quoted(const std::string& value) {
    std::string result = "\"";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') result += '\\';
        result += char(ch);
    }
    return result + '"';
}

std::string address(uint64_t value) { return std::to_string(value); }

}  // namespace

bool capture_flow(ReadMemory read, void* user, uint64_t argument, FlowProbe& out) {
    if (!read) return false;
    FlowProbe p;
    int32_t count = 0;
    uint64_t threads = 0, runtime_flow = 0;
    uint32_t slot_generation = 0;
    if (!field(read, user, argument, 8, p.context) || !field(read, user, argument, 0x10, p.thread_index) ||
        !field(read, user, p.context, 0x40, count) || count <= 0 || count > 256 ||
        p.thread_index < 0 || p.thread_index >= count || !field(read, user, p.context, 0x38, threads) ||
        !field(read, user, argument, 0x14, p.generation) ||
        !field(read, user, threads, uint64_t(p.thread_index) * 0x20 + 0x10, slot_generation) ||
        p.generation != slot_generation ||
        !field(read, user, threads, uint64_t(p.thread_index) * 0x20, runtime_flow) ||
        !field(read, user, runtime_flow, 0, p.resource) || !flow_name(read, user, p.resource, p.active_name)) return false;
    uint16_t event_index = 0, event_count = 0;
    uint8_t event_type = 0;
    uint64_t events = 0, argument_event = 0, event_name = 0;
    if (!field(read, user, threads, uint64_t(p.thread_index) * 0x20 + 0x14, event_index) ||
        !field(read, user, p.resource, 0x16, event_count) || event_index >= event_count ||
        !field(read, user, p.resource, 0x30, events) || !events ||
        events > std::numeric_limits<uint64_t>::max() - uint64_t(event_index) * 0x28 ||
        !field(read, user, argument, 0x38, argument_event) || argument_event != events + uint64_t(event_index) * 0x28 ||
        !field(read, user, argument_event, 8, event_type) || event_type != 0 ||
        !field(read, user, argument_event, 0, event_name) ||
        !resource_string(read, user, event_name, p.event_name)) return false;
    p.event_index = event_index;
    // These indices describe the initial entry point. They do not describe the entry point of a current subflow.
    int32_t initial_index = -1, entry_index = -1, flow_count = 0;
    uint64_t flows = 0, initial = 0, dictionary = 0, entry = 0;
    uint32_t dictionary_magic = 0, entries = 0;
    std::string initial_name, initial_entrypoint;
    if (field(read, user, p.context, 0x88, initial_index) && field(read, user, p.context, 0x8c, entry_index) &&
        field(read, user, p.context, 0x30, flow_count) && flow_count > 0 && flow_count <= 1024 &&
        initial_index >= 0 && initial_index < flow_count && entry_index >= 0 &&
        field(read, user, p.context, 0x28, flows) &&
        field(read, user, flows, uint64_t(initial_index) * 0x30, initial) &&
        flow_name(read, user, initial, initial_name) && field(read, user, initial, 0x38, dictionary) &&
        field(read, user, dictionary, 0, dictionary_magic) && dictionary_magic == 0x20434944 &&
        field(read, user, dictionary, 4, entries) && entries <= 65536 && uint32_t(entry_index) < entries &&
        field(read, user, dictionary, 0x20 + uint64_t(entry_index) * 0x10, entry) &&
        resource_string(read, user, entry, initial_entrypoint)) {
        p.initial_name = std::move(initial_name);
        p.initial_entrypoint = std::move(initial_entrypoint);
    }
    out = std::move(p);
    return true;
}

bool capture(ReadMemory read, void* user, const Locations& locations, Probe& out) {
    if (!read) return false;
    Probe p;
    if (!safe_string(read, user, locations.file_string, 63, p.file_id) || p.file_id.empty() ||
        !safe_string(read, user, locations.label_string, 127, p.label) || p.label.empty() ||
        !safe_string(read, user, locations.reference_string, 127, p.reference)) return false;
    const uint64_t action = locations.action;
    p.caller = locations.caller;
    const size_t colon = p.reference.find(':');
    if (colon == std::string::npos || !colon || p.reference.substr(colon + 1) != p.label) return false;
    const auto slash = p.reference.rfind('/', colon);
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    if (p.reference.substr(start, colon - start) != p.file_id) return false;
    p.action = action;
    if (field(read, user, action, 8, p.actor_context) && p.actor_context) {
        field(read, user, p.actor_context, 0x48, p.actor);
        field(read, user, p.actor, 0, p.actor_vtable);
        // The dispatcher reads this indirect SafeString from its current execution state. Keep its meaning
        // provisional until the creation path establishes whether it names a flow, entry point, or other context.
        uint64_t name = 0;
        if (field(read, user, p.actor_context, 0x288, p.flow_state) &&
            field(read, user, p.flow_state, 0x18, name))
            safe_string(read, user, name, 128, p.flow_name_candidate);
        // A nearby action reads this SafeString when building an NPC-dependent message reference. The name is
        // diagnostic evidence only until a native run and the selected actor type establish its meaning.
        if (p.actor && p.actor <= std::numeric_limits<uint64_t>::max() - 0x3e0)
            safe_string(read, user, p.actor + 0x3e0, 32, p.actor_name_candidate);
        // Bounded candidates help locate the active flow. A printable string is not proof of a field's meaning.
        for (uint32_t offset = 0; offset < 0x80 && p.context_strings.size() < 4; offset += 8) {
            uint64_t ptr;
            std::string value;
            if (field(read, user, p.actor_context, offset, ptr) && ascii(read, user, ptr, 64, value) && value.size() >= 3)
                p.context_strings.push_back({offset, std::move(value)});
        }
    }
    out = std::move(p);
    return true;
}

bool capture(ReadMemory read, void* user, uint64_t action, uint64_t fp, uint64_t sp, Probe& out) {
    if (!action || action > std::numeric_limits<uint64_t>::max() - 0xb8 || fp < 0x58 ||
        sp > std::numeric_limits<uint64_t>::max() - 0x40) return false;
    return capture(read, user, {fp - 0x58, sp + 0x40, action + 0xb8, action, 0}, out);
}

bool capture_text(ReadMemory read, void* user, uint64_t sp, uint64_t label_string, TextProbe& out) {
    if (!read || !sp || sp > std::numeric_limits<uint64_t>::max() - 0x12b) return false;
    TextProbe p;
    uint64_t text = 0;
    if (!ascii(read, user, sp + 0x2c, 255, p.filename) || p.filename.empty() ||
        !safe_string(read, user, label_string, 127, p.label) || p.label.empty() ||
        !field(read, user, sp, 0x18, text) || !text || !field(read, user, sp, 0x20, p.unit_count) ||
        p.unit_count < 0 || p.unit_count > 4096) return false;
    const size_t units = std::min<size_t>(128, size_t(p.unit_count));
    if (text > std::numeric_limits<uint64_t>::max() - units * 2) return false;
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < units * 2; i++) {
        uint8_t byte;
        if (!read(user, text + i, &byte, 1)) return false;
        p.utf16_prefix_hex += hex[byte >> 4];
        p.utf16_prefix_hex += hex[byte & 15];
    }
    p.truncated = units != size_t(p.unit_count);
    out = std::move(p);
    return true;
}

std::string to_json(const TextProbe& p, uint64_t sequence, uint64_t frame, const std::string& run_id) {
    return "{\"schema_version\":1,\"kind\":\"acnh.message_text\",\"build\":\"3.0.3\",\"pc\":\"0x3c4ce3c\","
        "\"run_id\":" + quoted(run_id) + ",\"sequence\":" + std::to_string(sequence) + ",\"frame\":" + std::to_string(frame) +
        ",\"filename\":" + quoted(p.filename) + ",\"message_label\":" + quoted(p.label) +
        ",\"unit_count\":" + std::to_string(p.unit_count) + ",\"utf16_prefix_hex\":" + quoted(p.utf16_prefix_hex) +
        ",\"truncated\":" + (p.truncated ? "true" : "false") + ",\"speaker_verified\":false,\"flow_id\":null,\"eligible\":false}";
}

std::string to_json(const Probe& p, uint64_t sequence, uint64_t frame, const std::string& run_id) {
    std::string result = "{\"schema_version\":1,\"kind\":\"acnh.dialogue_probe\",\"build\":\"3.0.3\",\"pc\":\"0x9d85bc\","
        "\"run_id\":" + quoted(run_id) + ",\"sequence\":" + std::to_string(sequence) + ",\"frame\":" + std::to_string(frame) +
        ",\"message_reference\":" + quoted(p.reference) + ",\"file_id\":" + quoted(p.file_id) + ",\"message_label\":" + quoted(p.label) +
        ",\"actor_name_candidate\":" + quoted(p.actor_name_candidate) + ",\"action\":" + address(p.action) +
        ",\"actor_context\":" + address(p.actor_context) + ",\"actor\":" + address(p.actor) +
        ",\"actor_vtable\":" + address(p.actor_vtable) +
        ",\"flow_state\":" + address(p.flow_state) + ",\"flow_name_candidate\":" + quoted(p.flow_name_candidate) +
        ",\"speaker_verified\":false,\"flow_id\":" + (p.flow.active_name.empty() ? "null" : quoted(p.flow.active_name)) +
        ",\"flow_context\":" + address(p.flow.context) + ",\"flow_resource\":" + address(p.flow.resource) +
        ",\"flow_thread\":" + std::to_string(p.flow.thread_index) +
        ",\"flow_generation\":" + std::to_string(p.flow.generation) +
        ",\"flow_event_index\":" + std::to_string(p.flow.event_index) +
        ",\"flow_event_name\":" + (p.flow.event_name.empty() ? "null" : quoted(p.flow.event_name)) +
        ",\"initial_flow_id\":" + (p.flow.initial_name.empty() ? "null" : quoted(p.flow.initial_name)) +
        ",\"initial_entrypoint\":" + (p.flow.initial_entrypoint.empty() ? "null" : quoted(p.flow.initial_entrypoint)) +
        ",\"eligible\":false,\"context_strings\":[";
    bool first = true;
    for (const auto& c : p.context_strings) {
        if (!first) result += ',';
        first = false;
        result += "{\"offset\":" + std::to_string(c.offset) + ",\"text\":" + quoted(c.text) + "}";
    }
    return result + "],\"caller\":" + std::to_string(p.caller) + "}";
}

}  // namespace nx::acnh::dialogue
