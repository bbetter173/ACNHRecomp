// A VkPipelineCache kept on disk (<data root>/pipelines_<vendor>_<device>.bin), and the cost of what the replay
// creates on the fly. Pipelines are compiled the first time a draw needs them, inside the replay: the course's
// stutters when new objects come in (16-42 ms replays on the RTX 3050). The driver checks the cache header itself
// (vendor, device, driver build): a stale file is ignored. Saves run off the replay thread and at orderly exit.
// NX_PIPELINE_CACHE=0: no cache (bisecting, or to measure first-run stutter).
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "nvn_pipeline.h"
#include "nvn_pipeline_cache.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_tried = false;
std::atomic<uint64_t> g_revision{0}, g_saved_revision{0};
std::atomic<size_t> g_saved_records{0};
std::atomic<bool> g_saving{false};
std::thread g_save_thread;
bool g_stopping = false, g_keys_loaded = false, g_keys_valid = false;
std::string g_path;
uint64_t g_alias = 0, g_modules = 0, g_module_ns = 0, g_astc = 0, g_astc_ns = 0;
std::atomic<uint64_t> g_pipelines{0}, g_pipeline_ns{0}, g_worker_pipelines{0}, g_worker_ns{0};  // also counted from the compile worker threads
uint64_t g_waited = 0, g_waited_ns = 0, g_skipped = 0, g_prewarmed = 0;
std::unordered_set<uint64_t> g_known;  // hashes of the records in the .keys file or already queued to it
std::vector<WarmRec> g_records;
constexpr size_t kMaxRecords = 16384;
constexpr uint32_t kKeysMagic = 0x314b5750;  // "PWK1"

}  // namespace

VkPipelineCache pipeline_cache() {
    if (g_tried) return g_cache;
    g_tried = true;
    if (const char* e = std::getenv("NX_PIPELINE_CACHE"); e && e[0] == '0') return VK_NULL_HANDLE;
    VkPhysicalDeviceProperties p{};
    p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
    char name[64];
    std::snprintf(name, sizeof name, "/pipelines_%04x_%04x.bin", p.vendorID, p.deviceID);
    g_path = plat::data_root() + name;
    std::vector<uint8_t> data;
    std::error_code ec;
    const std::filesystem::path path(std::u8string(g_path.begin(), g_path.end()));
    if (std::filesystem::is_regular_file(path, ec)) {
        if (FILE* f = plat::fopen_utf8(g_path, "rb")) {
            if (plat::fseek64(f, 0, SEEK_END) == 0) {
                const int64_t n = plat::ftell64(f);
                if (n > 0 && plat::fseek64(f, 0, SEEK_SET) == 0) {
                    data.resize(size_t(n));
                    data.resize(std::fread(data.data(), 1, data.size(), f));
                }
            }
            std::fclose(f);
        }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;  // a cache the driver refuses: start empty
        p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache);
    }
    log("nvn/vk: pipeline cache %s (%zu bytes loaded)", g_path.c_str(), data.size());
    return g_cache;
}

// Keep the previous cache until the replacement is complete. The temporary file is beside it, on the same
// filesystem, and belongs to this process. Windows rename does not replace an existing file.
static bool write_cache_file(const std::string& path, const void* data, size_t size) {
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    const std::string tmp = path + ".tmp." + std::to_string(pid);
    FILE* f = plat::fopen_utf8(tmp, "wb");
    if (!f) {
        log("nvn/vk: cannot save cache %s", path.c_str());
        return false;
    }
    bool ok = std::fwrite(data, 1, size, f) == size;
    ok = std::fclose(f) == 0 && ok;
    const std::filesystem::path from(std::u8string(tmp.begin(), tmp.end()));
#ifdef _WIN32
    const std::filesystem::path to(std::u8string(path.begin(), path.end()));
#endif
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
    }
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(from, ec);
        log("nvn/vk: cannot replace cache %s", path.c_str());
    }
    return ok;
}

namespace {

std::string keys_path() {
    return (g_path.size() > 4 ? g_path.substr(0, g_path.size() - 4) : g_path) + ".keys";
}

// The caller holds cache_mutex. Only complete files enter the prewarm list.
bool load_records() {
    if (g_keys_loaded) return g_keys_valid;
    g_keys_loaded = true;
    FILE* f = plat::fopen_utf8(keys_path(), "rb");
    if (!f) return false;
    uint32_t head[2] = {};
    bool ok = std::fread(head, sizeof head, 1, f) == 1 && head[0] == kKeysMagic && head[1] == sizeof(WarmRec);
    std::vector<WarmRec> records;
    while (ok) {
        WarmRec r;
        const size_t n = std::fread(&r, 1, sizeof r, f);
        if (!n) { ok = !std::ferror(f); break; }
        if (n != sizeof r || records.size() == kMaxRecords || !r.vs[0] ||
            !std::memchr(r.vs, 0, sizeof r.vs) || !std::memchr(r.fs, 0, sizeof r.fs) || r.key.vs || r.key.fs) {
            ok = false;
            break;
        }
        records.push_back(r);
    }
    std::fclose(f);
    if (!ok) return false;
    for (const WarmRec& r : records) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&r);
        if (g_known.insert(hash_bytes(bytes, sizeof r)).second) g_records.push_back(r);
    }
    g_saved_records = g_records.size();
    return g_keys_valid = true;
}

// Driver serialization and disk writes stay off the replay thread. Snapshots own their records while the game
// discovers more. A revision, rather than blob size, detects updates that leave the driver cache the same size.
void save_snapshot(VkDevice device, VkPipelineCache cache, uint64_t revision, const std::vector<WarmRec>& records) {
    if (revision != g_saved_revision) {
        size_t n = 0;
        if (p_vkGetPipelineCacheData(device, cache, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<uint8_t> data(n);
            if (p_vkGetPipelineCacheData(device, cache, &n, data.data()) == VK_SUCCESS &&
                write_cache_file(g_path, data.data(), n)) {
                g_saved_revision = revision;
            }
        }
    }
    if (records.size() != g_saved_records) {
        std::vector<uint8_t> data(8 + records.size() * sizeof(WarmRec));
        const uint32_t head[2] = {kKeysMagic, uint32_t(sizeof(WarmRec))};
        std::memcpy(data.data(), head, sizeof head);
        if (!records.empty()) std::memcpy(data.data() + sizeof head, records.data(), records.size() * sizeof(WarmRec));
        if (write_cache_file(keys_path(), data.data(), data.size())) g_saved_records = records.size();
    }
}

}  // namespace

void pipeline_cache_save() {
    std::lock_guard lk(cache_mutex());
    if (!g_cache || g_stopping || g_saving.exchange(true)) return;
    load_records();
    if (g_save_thread.joinable()) g_save_thread.join();
    const VkDevice device = vk_device();
    const VkPipelineCache cache = g_cache;
    const uint64_t revision = g_revision.load();
    g_save_thread = std::thread([device, cache, revision, records = g_records] {
        save_snapshot(device, cache, revision, records);
        g_saving = false;
    });
}

// Orderly exits only. Error paths still stop immediately. Cache state is frozen while the previous writer joins
// and the final snapshot is saved; no GPU completion or presenter wait is needed.
void pipeline_cache_shutdown() {
    std::lock_guard lk(cache_mutex());
    g_stopping = true;
    if (g_save_thread.joinable()) g_save_thread.join();
    if (g_cache) {
        load_records();
        save_snapshot(vk_device(), g_cache, g_revision.load(), g_records);
    }
}

// What the replay built or waited for since the last present, for the hitch lines (present_hitch_check).
struct FrameCost {
    std::atomic<uint64_t> pipelines{0}, pipeline_ns{0}, modules{0}, module_ns{0}, waited{0}, waited_ns{0};
    std::atomic<uint64_t> textures{0}, texture_ns{0}, texture_bytes{0};
};
FrameCost g_frame_cost;

void note_pipeline_created(uint64_t ns, bool worker) {
    g_revision++;
    (worker ? g_worker_pipelines : g_pipelines)++, (worker ? g_worker_ns : g_pipeline_ns) += ns;
    if (!worker) g_frame_cost.pipelines++, g_frame_cost.pipeline_ns += ns;
}
void note_pipeline_waited(uint64_t ns) { g_waited++, g_waited_ns += ns, g_frame_cost.waited++, g_frame_cost.waited_ns += ns; }
void note_pipeline_skipped() { g_skipped++; }
void note_texture_uploaded(uint64_t ns, uint64_t bytes) {
    g_frame_cost.textures++, g_frame_cost.texture_ns += ns, g_frame_cost.texture_bytes += bytes;
}
void note_pipeline_prewarmed() { g_prewarmed++; }

// ---- the keys of earlier sessions (<cache path minus .bin>.keys) ---------------------------------------------------
// Header {magic, sizeof(WarmRec)} then WarmRec records. A file of another layout is ignored (and rewritten).

static uint64_t rec_hash(const WarmRec& r) { return hash_bytes(reinterpret_cast<const unsigned char*>(&r), sizeof r); }

bool warm_load(std::vector<WarmRec>& out) {
    if (!pipeline_cache() || !load_records()) return false;
    out.insert(out.end(), g_records.begin(), g_records.end());
    return true;
}

void warm_record(const PipelineKey& k) {
    if (!pipeline_cache()) return;
    load_records();
    WarmRec r;
    std::memset(&r, 0, sizeof r);
    if (!k.vs || k.vs->hash.size() >= sizeof r.vs || (k.fs && k.fs->hash.size() >= sizeof r.fs)) return;
    std::memcpy(r.vs, k.vs->hash.c_str(), k.vs->hash.size());
    if (k.fs) std::memcpy(r.fs, k.fs->hash.c_str(), k.fs->hash.size());
    r.key = k;
    r.key.vs = nullptr, r.key.fs = nullptr;
    if (g_known.size() < kMaxRecords && g_known.insert(rec_hash(r)).second) g_records.push_back(r);  // ~700 bytes each
}

void note_pipeline_alias() { g_alias++; }
void note_module_created(uint64_t ns) {
    g_modules++, g_module_ns += ns;
    g_frame_cost.modules++, g_frame_cost.module_ns += ns;
}

// Every present: a frame shown 50 ms or more after the previous one (a visible stutter at 30 fps) is logged with what
// the replay spent on shaders in between, so a stutter report says whether it was a shader build or something else.
// NX_HITCH_MS=<ms> sets the threshold; 300 lines at most per run.
void present_hitch_check(uint64_t replay_ns, uint64_t upload_bytes, uint64_t present_ns, uint64_t fence_ns) {
    static const double limit = std::getenv("NX_HITCH_MS") ? std::atof(std::getenv("NX_HITCH_MS")) : 50.0;
    static uint64_t last = 0, presents = 0;
    static int lines = 0;
    uint64_t now = plat::monotonic_ns();
    double ms = last ? double(now - last) / 1e6 : 0.0;
    last = now, presents++;
    FrameCost& f = g_frame_cost;
    if (ms >= limit && lines < 300) {
        lines++;
        // The rest of the interval (not replay, fences or the previous present): waiting for the game's next frame.
        log("hitch: present %llu, %.1f ms after the previous; replay and submit %.1f ms (fences %.1f), the previous "
            "present %.1f ms, upload %.1f MB; built %llu "
            "pipelines (%.1f ms), %llu shader modules (%.1f ms); waited for %llu compiles in flight (%.1f ms); "
            "%llu textures uploaded (%.1f MB, %.1f ms)",
            (unsigned long long)presents, ms, double(replay_ns) / 1e6, double(fence_ns) / 1e6,
            double(present_ns) / 1e6, double(upload_bytes) / 1e6,
            (unsigned long long)f.pipelines.load(), double(f.pipeline_ns.load()) / 1e6,
            (unsigned long long)f.modules.load(), double(f.module_ns.load()) / 1e6, (unsigned long long)f.waited.load(),
            double(f.waited_ns.load()) / 1e6, (unsigned long long)f.textures.load(),
            double(f.texture_bytes.load()) / 1e6, double(f.texture_ns.load()) / 1e6);
    }
    f.pipelines = 0, f.pipeline_ns = 0, f.modules = 0, f.module_ns = 0, f.waited = 0, f.waited_ns = 0;
    f.textures = 0, f.texture_ns = 0, f.texture_bytes = 0;
}
void note_astc_decoded(uint64_t ns) { g_astc++, g_astc_ns += ns; }

// One line per timing window (nvn_vk.cpp log_timing): what the replay created, and the time it took.
void creation_log(uint64_t frames) {
    if (g_pipelines || g_astc)
        log("nvn/vk: created over %llu frames: %llu pipelines (%.1f ms), %llu ASTC textures decoded (%.1f ms)",
            (unsigned long long)frames, (unsigned long long)g_pipelines.load(), double(g_pipeline_ns.load()) / 1e6,
            (unsigned long long)g_astc, double(g_astc_ns) / 1e6);
    if (g_alias || g_modules)
        log("nvn/vk: over %llu frames: %llu raw keys shared an existing pipeline, %llu shader modules (%.1f ms)",
            (unsigned long long)frames, (unsigned long long)g_alias, (unsigned long long)g_modules, double(g_module_ns) / 1e6);
    g_alias = g_modules = g_module_ns = 0;
    if (g_prewarmed || g_skipped || g_waited || g_worker_pipelines)
        log("nvn/vk: over %llu frames: %llu pipelines sent to workers, %llu compiled there (%.1f ms), the replay waited for %llu (%.1f ms), %llu draws skipped",
            (unsigned long long)frames, (unsigned long long)g_prewarmed, (unsigned long long)g_worker_pipelines.load(),
            double(g_worker_ns.load()) / 1e6, (unsigned long long)g_waited, double(g_waited_ns) / 1e6,
            (unsigned long long)g_skipped);
    g_worker_pipelines = 0, g_worker_ns = 0;
    g_prewarmed = g_waited = g_waited_ns = g_skipped = 0;
    g_pipelines = 0, g_pipeline_ns = 0, g_astc = g_astc_ns = 0;
    static uint64_t windows = 0;
    if (++windows % 10 == 0) pipeline_cache_save();
}

}  // namespace nx::nvn
