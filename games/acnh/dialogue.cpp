// Opt-in, read-only observations at the message-reference boundary. Address and layout evidence is recorded in
// docs/VILLAGER_DIALOGUE.md. Unsupported builds and missing hooks leave this feature disabled.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include <nx/runtime.h>

#include "dialogue_probe.h"
#include "platform.h"

namespace nx::acnh::dialogue {
namespace {

bool read_guest(void*, uint64_t address, void* out, size_t size) {
    if (!size || address > std::numeric_limits<uint64_t>::max() - size) return false;
    for (uint64_t at = address, end = address + size; at < end;) {
        const VmInfo region = vm_query(at);
        if (!(region.perm & 1) || at < region.addr || at - region.addr >= region.size) return false;
        const uint64_t available = region.size - (at - region.addr);
        at += std::min<uint64_t>(available, end - at);
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
}

bool supported() {
    static const bool match = [] {
        const char* enabled = std::getenv("NX_DIALOGUE_TRACE");
        if (!enabled || std::strcmp(enabled, "1")) return false;
        // Match the executable identity as well as the live instructions that define the probed stack layout.
        unsigned char build[16]{};
        const unsigned char expected[] = {0xff,0x1d,0x1c,0x05,0x67,0x0d,0xb6,0x02,0x1c,0x85,0xb6,0x24,0xa7,0x10,0xb9,0x63};
        FILE* f = plat::fopen_utf8(plat::data_root() + "/exefs/main", "rb");
        bool ok = f && plat::fseek64(f, 0x40, SEEK_SET) == 0 && std::fread(build, sizeof build, 1, f) == 1;
        if (f) std::fclose(f);
        uint32_t parser_call = 0, next = 0, prologue = 0, lookup_call = 0, lookup_next = 0;
        uint32_t dispatch_entry = 0, dispatch_return = 0;
        ok = ok && !std::memcmp(build, expected, sizeof build) &&
             read_guest(nullptr, kModuleBase + 0x9d8430, &prologue, 4) && prologue == 0xd10603ff &&
             read_guest(nullptr, kModuleBase + 0x9d89b4, &parser_call, 4) && parser_call == 0x97fffe9f &&
             read_guest(nullptr, kModuleBase + 0x9d85bc, &next, 4) && next == 0xa9574ff4 &&
             read_guest(nullptr, kModuleBase + 0x3c4ce38, &lookup_call, 4) && lookup_call == 0x97f702da &&
             read_guest(nullptr, kModuleBase + 0x3c4ce3c, &lookup_next, 4) && lookup_next == 0xf9400fe8 &&
             read_guest(nullptr, kModuleBase + 0x60f4f0, &dispatch_entry, 4) && dispatch_entry == 0xd101c3ff &&
             read_guest(nullptr, kModuleBase + 0x60f7c4, &dispatch_return, 4) && dispatch_return == 0xa9464ff4;
        log("dialogue: read-only probe %s for main+9d8430", ok ? "enabled" : "disabled: build or instruction mismatch");
        return ok;
    }();
    return match;
}

thread_local std::vector<Locations> t_parsers;
thread_local unsigned t_ignored = 0;

struct Dispatch {
    uint64_t receptor, stack_pointer;
    FlowProbe flow;
};
thread_local std::vector<Dispatch> t_dispatches;
thread_local unsigned t_dispatch_ignored = 0;

void before_dispatch(CPU& cpu) {
    if (!supported()) return;
    if (t_dispatch_ignored) { t_dispatch_ignored++; return; }
    // Discard scopes left by a nonlocal unwind before accepting another dispatch at the same or higher frame.
    while (!t_dispatches.empty() && cpu.x[31] >= t_dispatches.back().stack_pointer) t_dispatches.pop_back();
    if (t_dispatches.size() >= 16) { t_dispatch_ignored++; return; }
    Dispatch dispatch{cpu.x[0], cpu.x[31], {}};
    capture_flow(read_guest, nullptr, cpu.x[2], dispatch.flow);
    t_dispatches.push_back(std::move(dispatch));
}

void after_dispatch(CPU& cpu) {
    if (!supported()) return;
    if (t_dispatch_ignored) { t_dispatch_ignored--; return; }
    if (!t_dispatches.empty() && cpu.x[31] <= std::numeric_limits<uint64_t>::max() - 0x70 &&
        t_dispatches.back().stack_pointer == cpu.x[31] + 0x70) t_dispatches.pop_back();
    else t_dispatches.clear();
}

const std::string& run_id() {
    static const std::string id = std::to_string(plat::monotonic_ns());
    return id;
}

void before_reference(CPU& cpu) {
    if (!supported()) return;
    if (t_ignored || t_parsers.size() >= 16) { t_ignored++; return; }
    Locations locations{cpu.x[0], cpu.x[1], cpu.x[2], 0, cpu.x[30] - kModuleBase};
    if (cpu.x[2] >= 0xb8 && cpu.x[19] == cpu.x[2] - 0xb8) locations.action = cpu.x[19];
    t_parsers.push_back(locations);
}

void after_reference(CPU& cpu) {
    if (!supported()) return;
    if (t_ignored) { t_ignored--; return; }
    if (t_parsers.empty()) return;
    const Locations locations = t_parsers.back();
    t_parsers.pop_back();
    if (!(cpu.x[0] & 1)) return;
    Probe probe;
    if (!capture(read_guest, nullptr, locations, probe)) return;
    if (!t_dispatch_ignored && !t_dispatches.empty()) {
        const Dispatch& dispatch = t_dispatches.back();
        if (probe.actor_context == dispatch.receptor && cpu.x[31] < dispatch.stack_pointer)
            probe.flow = dispatch.flow;
    }
    static std::atomic<uint64_t> sequence{0};
    const uint64_t id = sequence.fetch_add(1);
    if (id >= 10000) return;
    const std::string line = to_json(probe, id, g_frame.load(), run_id());
    if (line.size() < 1900) log("dialogue_probe: %s", line.c_str());
}

void after_text_lookup(CPU& cpu) {
    if (!supported()) return;
    TextProbe probe;
    if (!capture_text(read_guest, nullptr, cpu.x[31], cpu.x[20], probe)) return;
    static std::atomic<uint64_t> sequence{0};
    const uint64_t id = sequence.fetch_add(1);
    if (id >= 10000) return;
    const std::string line = to_json(probe, id, g_frame.load(), run_id());
    if (line.size() < 1900) log("dialogue_probe: %s", line.c_str());
}

}  // namespace
NX_HOOK(0x9d8430, before_reference);
NX_HOOK(0x9d85bc, after_reference);
NX_HOOK(0x3c4ce3c, after_text_lookup);
NX_HOOK(0x60f4f0, before_dispatch);
NX_HOOK(0x60f7c4, after_dispatch);
}  // namespace nx::acnh::dialogue
