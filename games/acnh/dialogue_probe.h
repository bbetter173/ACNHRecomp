#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nx::acnh::dialogue {

using ReadMemory = bool (*)(void*, uint64_t, void*, size_t);

struct StringCandidate {
    uint32_t offset;
    std::string text;
};

struct FlowProbe {
    uint64_t context = 0, resource = 0;
    int32_t thread_index = -1;
    int32_t event_index = -1;
    uint32_t generation = 0;
    std::string active_name, event_name, initial_name, initial_entrypoint;
};
bool capture_flow(ReadMemory read, void* user, uint64_t engine_argument, FlowProbe& out);

struct Probe {
    std::string reference, file_id, label, actor_name_candidate, flow_name_candidate;
    uint64_t action = 0, actor_context = 0, actor = 0, actor_vtable = 0, flow_state = 0, caller = 0;
    FlowProbe flow;
    std::vector<StringCandidate> context_strings;
};

struct Locations {
    uint64_t file_string = 0, label_string = 0, reference_string = 0, action = 0, caller = 0;
};
bool capture(ReadMemory read, void* user, const Locations& locations, Probe& out);

struct TextProbe {
    std::string filename, label, utf16_prefix_hex;
    int32_t unit_count = 0;
    bool truncated = false;
};
bool capture_text(ReadMemory read, void* user, uint64_t stack_pointer, uint64_t label_string, TextProbe& out);
std::string to_json(const TextProbe& probe, uint64_t sequence, uint64_t frame, const std::string& run_id);

// These inputs come from the instruction after the message-reference parser returns successfully. ReadMemory
// must reject unmapped ranges. This function never writes guest memory or assigns a verified NPC/flow identity.
bool capture(ReadMemory read, void* user, uint64_t action, uint64_t frame_pointer, uint64_t stack_pointer, Probe& out);
std::string to_json(const Probe& probe, uint64_t sequence, uint64_t frame, const std::string& run_id);

}  // namespace nx::acnh::dialogue
