#include "dialogue_probe.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace nx::acnh::dialogue;

namespace {
struct Memory {
    std::vector<unsigned char> bytes = std::vector<unsigned char>(0x4000);
    template <typename T> void put(size_t at, T value) { std::memcpy(bytes.data() + at, &value, sizeof value); }
    void text(size_t at, const std::string& value) { std::memcpy(bytes.data() + at, value.c_str(), value.size() + 1); }
    void string(size_t at, size_t storage, const std::string& value) { put<uint64_t>(at + 8, storage); text(storage, value); }
    static bool read(void* user, uint64_t at, void* out, size_t size) {
        auto& memory = *static_cast<Memory*>(user);
        if (!at || at >= memory.bytes.size() || size > memory.bytes.size() - at) return false;
        std::memcpy(out, memory.bytes.data() + at, size);
        return true;
    }
};
int failures = 0;
void expect(bool ok, const char* message) { if (!ok) { std::cerr << "FAIL: " << message << '\n'; failures++; } }
Memory fixture() {
    Memory m;
    m.string(0x2000 - 0x58, 0x2800, "Example");
    m.string(0x1000 + 0x40, 0x2900, "001");
    m.string(0x400 + 0xb8, 0x2a00, "TalkNNpc/Test/Example:001");
    m.put<uint64_t>(0x408, 0x800);
    m.put<uint64_t>(0x848, 0xc00);
    m.put<uint64_t>(0xc00, 0x3100);
    m.put<uint64_t>(0xa88, 0x3200);
    m.put<uint64_t>(0x3218, 0x3300);
    m.string(0x3300, 0x3400, "execution-name-candidate");
    m.string(0xc00 + 0x3e0, 0x2b00, "npc-test");
    m.put<uint64_t>(0x820, 0x2c00);
    m.text(0x2c00, "flow-candidate");
    return m;
}
bool capture(Memory& m, Probe& p) { return capture(Memory::read, &m, 0x400, 0x2000, 0x1000, p); }
}

int main() {
    auto memory = fixture();
    const auto original = memory.bytes;
    Probe probe;
    expect(capture(memory, probe), "known stack layout yields the exact message identity");
    expect(probe.reference == "TalkNNpc/Test/Example:001" && probe.file_id == "Example" && probe.label == "001",
           "reference and parsed fields agree");
    expect(probe.actor_name_candidate == "npc-test", "actor names remain diagnostic candidates");
    expect(probe.actor_vtable == 0x3100, "actor dispatch table is observed without calling guest functions");
    expect(probe.flow_state == 0x3200 && probe.flow_name_candidate == "execution-name-candidate",
           "execution state names follow the dispatcher's indirect string layout");
    expect(memory.bytes == original, "capturing a probe never writes guest memory");
    auto json = to_json(probe, 7, 123, "run-1");
    expect(json.find("\"speaker_verified\":false,\"flow_id\":null") != std::string::npos &&
           json.find("\"eligible\":false") != std::string::npos,
           "the tracer never claims speaker, flow or generation eligibility");
    expect(json.find("flow-candidate") != std::string::npos && json.size() < 1900, "context strings are bounded");
    memory.put<uint64_t>(0x408, std::numeric_limits<uint64_t>::max());
    expect(capture(memory, probe) && probe.actor_name_candidate.empty(), "unreadable optional actor data is omitted");
    memory = fixture();
    memory.put<uint64_t>(0xa88, UINT64_MAX);
    expect(capture(memory, probe) && probe.flow_name_candidate.empty(), "unreadable execution state is optional");
    memory = fixture();
    memory.text(0x3400, std::string(129, 'f'));
    expect(capture(memory, probe) && probe.flow_name_candidate.empty(), "execution name reads remain bounded");
    memory = fixture();
    memory.text(0x2a00, "TalkNNpc/Test/Other:001");
    expect(!capture(memory, probe), "mismatched file identities are rejected");
    memory = fixture();
    memory.text(0x2a00, "TalkNNpc/Test/Example:002");
    expect(!capture(memory, probe), "mismatched labels are rejected");
    memory = fixture();
    memory.put<uint64_t>(0x2000 - 0x50, 0x4000);
    expect(!capture(memory, probe), "unmapped required strings are rejected");
    memory = fixture();
    memory.text(0x2800, std::string(64, 'x'));
    expect(!capture(memory, probe), "overlong strings are rejected without truncation");
    memory = fixture();
    memory.text(0x2900, "bad\nlabel");
    expect(!capture(memory, probe), "control characters are rejected");
    memory = fixture();
    expect(!capture(Memory::read, &memory, 0x400, 0x10, 0x1000, probe), "frame pointer underflow is rejected");
    expect(!capture(Memory::read, &memory, 0x400, 0x2000, UINT64_MAX, probe), "stack pointer overflow is rejected");
    expect(!capture(Memory::read, &memory, UINT64_MAX, 0x2000, 0x1000, probe), "action address overflow is rejected");
    memory = fixture();
    memory.string(0x2000 - 0x58, 0x2800, "A\"B");
    memory.string(0x400 + 0xb8, 0x2a00, "TalkNNpc/Test/A\"B:001");
    expect(capture(memory, probe), "quote-containing identifiers remain bounded observations");
    expect(to_json(probe, 0, 0, "run").find("A\\\"B") != std::string::npos, "JSON strings escape quotation marks");
    memory = fixture();
    memory.text(0x102c, "Example.msbt");
    memory.put<uint64_t>(0x1018, 0x3000);
    memory.put<int32_t>(0x1020, 3);
    memory.put<uint16_t>(0x3000, 'A');
    memory.put<uint16_t>(0x3002, 0x0e);
    memory.put<uint16_t>(0x3004, 0x30c6);
    TextProbe text;
    const auto text_original = memory.bytes;
    expect(capture_text(Memory::read, &memory, 0x1000, 0x1040, text), "lookup spans can be copied without retaining pointers");
    expect(text.utf16_prefix_hex == "41000e00c630" && text.unit_count == 3 && !text.truncated,
           "UTF-16 code units and control markers remain exact bytes");
    expect(memory.bytes == text_original, "capturing source text never writes guest memory");
    expect(to_json(text, 0, 0, "run").find("\"eligible\":false") != std::string::npos,
           "a text lookup never establishes conversation eligibility");
    memory.put<int32_t>(0x1020, -1);
    expect(!capture_text(Memory::read, &memory, 0x1000, 0x1040, text), "negative UTF-16 counts are rejected");
    memory.put<int32_t>(0x1020, 5000);
    expect(!capture_text(Memory::read, &memory, 0x1000, 0x1040, text), "unbounded UTF-16 spans are rejected");
    memory.put<int32_t>(0x1020, 129);
    expect(capture_text(Memory::read, &memory, 0x1000, 0x1040, text) && text.truncated,
           "bounded diagnostic prefixes are marked as truncated");
    memory.put<uint64_t>(0x1018, UINT64_MAX);
    expect(!capture_text(Memory::read, &memory, 0x1000, 0x1040, text), "UTF-16 address overflow is rejected");
    Memory flow_memory;
    flow_memory.put<uint64_t>(0x308, 0x800);
    flow_memory.put<int32_t>(0x310, 1);
    flow_memory.put<uint64_t>(0x838, 0x1000);
    flow_memory.put<int32_t>(0x840, 2);
    flow_memory.put<uint64_t>(0x828, 0x1100);
    flow_memory.put<int32_t>(0x830, 1);
    flow_memory.put<int32_t>(0x888, 0);
    flow_memory.put<int32_t>(0x88c, 0);
    flow_memory.put<uint64_t>(0x1100, 0x2000);
    flow_memory.put<uint64_t>(0x1000, 0x1800);
    flow_memory.put<uint64_t>(0x1020, 0x1900);
    flow_memory.put<uint16_t>(0x1034, 1);
    flow_memory.put<uint64_t>(0x1800, 0x2000);
    flow_memory.put<uint64_t>(0x1900, 0x2100);
    flow_memory.put<uint32_t>(0x2000, 0x4c465645);
    flow_memory.put<uint32_t>(0x2100, 0x4c465645);
    flow_memory.put<uint64_t>(0x2020, 0x2800);
    flow_memory.put<uint64_t>(0x2120, 0x2900);
    flow_memory.put<uint16_t>(0x2116, 2);
    flow_memory.put<uint64_t>(0x2130, 0x2200);
    flow_memory.put<uint64_t>(0x338, 0x2228);
    flow_memory.put<uint64_t>(0x2228, 0x2a00);
    flow_memory.put<uint16_t>(0x2a00, 7);
    flow_memory.text(0x2a02, "message");
    flow_memory.put<uint16_t>(0x2800, 6);
    flow_memory.text(0x2802, "parent");
    flow_memory.put<uint16_t>(0x2900, 7);
    flow_memory.text(0x2902, "subflow");
    flow_memory.put<uint64_t>(0x2038, 0x3000);
    flow_memory.put<uint32_t>(0x3000, 0x20434944);
    flow_memory.put<uint32_t>(0x3004, 1);
    flow_memory.put<uint64_t>(0x3020, 0x3100);
    flow_memory.put<uint16_t>(0x3100, 5);
    flow_memory.text(0x3102, "start");
    const auto flow_original = flow_memory.bytes;
    FlowProbe flow;
    expect(capture_flow(Memory::read, &flow_memory, 0x300, flow) && flow.active_name == "subflow" &&
           flow.thread_index == 1 && flow.resource == 0x2100, "flow identity follows the active execution thread");
    expect(flow.initial_name == "parent" && flow.initial_entrypoint == "start",
           "initial story entry point remains distinct from the current subflow");
    expect(flow.event_index == 1 && flow.event_name == "message", "flow event identity matches the actual dispatched action");
    expect(flow_memory.bytes == flow_original, "flow capture never writes guest memory");
    for (int32_t index : {-1, 2, INT32_MAX}) {
        flow_memory.put<int32_t>(0x310, index);
        expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "invalid thread indices are rejected");
    }
    flow_memory.bytes = flow_original;
    flow_memory.put<uint64_t>(0x338, 0x2200);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "a different action cannot acquire this event identity");
    flow_memory.bytes = flow_original;
    flow_memory.put<uint16_t>(0x1034, 2);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "event indices must fit the flow resource");
    flow_memory.bytes = flow_original;
    flow_memory.put<uint8_t>(0x2230, 1);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "non-action nodes cannot acquire a dispatch identity");
    flow_memory.bytes = flow_original;
    flow_memory.put<uint32_t>(0x1030, 1);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "reused execution slots cannot supply stale flow identity");
    flow_memory.bytes = flow_original;
    flow_memory.put<int32_t>(0x840, 257);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "thread counts remain bounded");
    flow_memory.bytes = flow_original;
    flow_memory.put<uint64_t>(0x838, UINT64_MAX);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "thread array arithmetic cannot overflow");
    flow_memory.bytes = flow_original;
    flow_memory.put<uint32_t>(0x2100, 0);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "flow resources require EVFL magic");
    flow_memory.bytes = flow_original;
    flow_memory.put<uint16_t>(0x2900, 6);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "resource names must match their declared length");
    flow_memory.bytes = flow_original;
    flow_memory.put<uint16_t>(0x2900, 129);
    expect(!capture_flow(Memory::read, &flow_memory, 0x300, flow), "flow names remain bounded");
    for (int32_t index : {-1, 1, INT32_MAX}) {
        flow_memory.bytes = flow_original;
        flow_memory.put<int32_t>(0x88c, index);
        expect(capture_flow(Memory::read, &flow_memory, 0x300, flow) && flow.initial_name.empty() &&
               flow.initial_entrypoint.empty(), "invalid initial entry indices never acquire an entry identity");
    }
    flow_memory.bytes = flow_original;
    flow_memory.put<uint32_t>(0x3000, 0);
    expect(capture_flow(Memory::read, &flow_memory, 0x300, flow) && flow.initial_entrypoint.empty(),
           "initial entry names require a DIC resource");
    return failures ? 1 : 0;
}
