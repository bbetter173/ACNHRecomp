// Exercises the real cache reader, writer and shutdown path with a fake Vulkan driver. No GPU or game is needed.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "nvn_pipeline.h"
#include "nvn_pipeline_cache.h"

namespace fs = std::filesystem;
using namespace nx::nvn;

namespace {
fs::path root;
std::string scenario;
const std::string old_blob = "old!", new_blob = "new!";  // equal size, different contents
std::mutex driver_mutex;
std::condition_variable driver_cv;
bool driver_entered = false, release_driver = false;
int failures = 0;
std::atomic<unsigned> temporary_opens{0};

void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); failures++; }
}

std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path path_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

FILE* open_file(const fs::path& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wide_mode(mode, mode + std::strlen(mode));
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void put(const fs::path& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), std::streamsize(bytes.size()));
    expect(bool(f), "fixture write succeeds");
}

std::string read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

fs::path blob_path() { return root / "pipelines_1234_5678.bin"; }
fs::path keys_path() { return root / "pipelines_1234_5678.keys"; }

WarmRec recipe(const char* hash) {
    WarmRec r;
    std::memset(&r, 0, sizeof r);
    std::strcpy(r.vs, hash);
    r.key.topology = 4;
    return r;
}

std::string keys(const std::vector<WarmRec>& records) {
    uint32_t head[2] = {0x314b5750, uint32_t(sizeof(WarmRec))};
    std::string bytes(reinterpret_cast<const char*>(head), sizeof head);
    for (const auto& r : records) bytes.append(reinterpret_cast<const char*>(&r), sizeof r);
    return bytes;
}

void record(const char* hash) {
    ShaderInfo shader;
    shader.hash = hash;
    PipelineKey k;
    std::memset(&k, 0, sizeof k);
    k.vs = &shader;
    k.topology = 4;
    warm_record(k);
    note_pipeline_created(1, false);
}

VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    *p = {};
    p->vendorID = 0x1234;
    p->deviceID = 0x5678;
}

VKAPI_ATTR VkResult VKAPI_CALL create_cache(VkDevice, const VkPipelineCacheCreateInfo*,
                                           const VkAllocationCallbacks*, VkPipelineCache* cache) {
    *cache = reinterpret_cast<VkPipelineCache>(uintptr_t(1));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL get_data(VkDevice, VkPipelineCache, size_t* n, void* data) {
    if (scenario == "writer-join" && !data) {
        std::unique_lock lk(driver_mutex);
        driver_entered = true;
        driver_cv.notify_all();
        driver_cv.wait(lk, [] { return release_driver; });
    }
    *n = new_blob.size();
    if (data) std::memcpy(data, new_blob.data(), *n);
    return data && scenario == "incomplete-driver" ? VK_INCOMPLETE : VK_SUCCESS;
}
}  // namespace

namespace nx {
void log(const char*, ...) {}
}
namespace nx::plat {
std::string data_root() { return utf8(root); }
uint64_t monotonic_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
FILE* fopen_utf8(const std::string& path, const char* mode) {
    if (path.find(".tmp.") != std::string::npos && mode[0] == 'w') {
        const unsigned attempt = temporary_opens++;
        if (scenario == "failed-open" || (scenario == "retry-save" && attempt == 0)) return nullptr;
        // A valid read-only stream makes fwrite fail without platform-specific disk-full devices.
        if (scenario == "failed-write") return open_file(root / "readonly", "rb");
    }
    return open_file(path_utf8(path), mode);
}
int64_t ftell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}
int fseek64(FILE* f, int64_t off, int origin) {
#ifdef _WIN32
    return _fseeki64(f, off, origin);
#else
    return fseeko(f, off, origin);
#endif
}
}  // namespace nx::plat
namespace nx::nvn {
VkDevice vk_device() { return reinterpret_cast<VkDevice>(uintptr_t(1)); }
VkPhysicalDevice vk_physical() { return reinterpret_cast<VkPhysicalDevice>(uintptr_t(1)); }
std::recursive_mutex& cache_mutex() { static std::recursive_mutex m; return m; }
}
namespace nx::vk {
PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties = properties;
PFN_vkCreatePipelineCache p_vkCreatePipelineCache = create_cache;
PFN_vkGetPipelineCacheData p_vkGetPipelineCacheData = get_data;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    scenario = argv[1];
    root = fs::temp_directory_path() / path_utf8("acnh-cache-test-\xc3\xa9-" + scenario + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    put(root / "readonly", "unchanged");
    const auto first = recipe("0123456789abcdef"), second = recipe("fedcba9876543210");
    if (scenario != "short-session") put(blob_path(), old_blob);
    const bool bad_keys = scenario == "bad-header" || scenario == "truncated-keys" ||
                          scenario == "bad-string" || scenario == "over-limit";
    if (bad_keys) {
        auto bytes = keys({first});
        if (scenario == "bad-header") bytes[0] = 0;
        if (scenario == "truncated-keys") bytes += "partial record";
        if (scenario == "bad-string") {
            auto invalid = second;
            std::memset(invalid.vs, 'a', sizeof invalid.vs);
            bytes = keys({first, invalid});
        }
        if (scenario == "over-limit") bytes = keys(std::vector<WarmRec>(16385, first));
        put(keys_path(), bytes);
        std::vector<WarmRec> loaded;
        expect(!warm_load(loaded) && loaded.empty(), "invalid record files are rejected in full");
    } else if (scenario == "roundtrip" || scenario == "failed-open" || scenario == "failed-write") {
        put(keys_path(), keys({first}));
        std::vector<WarmRec> loaded;
        expect(warm_load(loaded) && loaded.size() == 1 && std::strcmp(loaded[0].vs, first.vs) == 0,
               "existing records load from disk");
    }
    if (scenario == "failed-replace") {
        fs::remove(blob_path());
        fs::create_directory(blob_path());
        put(blob_path() / "marker", "keep");
    }
    if (scenario == "save-without-prewarm") {
        put(keys_path(), keys({first}));
        pipeline_cache();
        note_pipeline_created(1, false);  // a compute pipeline does not record graphics prewarm state
        pipeline_cache_save();
        pipeline_cache_shutdown();
        expect(read(keys_path()) == keys({first}), "saving before prewarm preserves existing records");
        fs::remove_all(root);
        return failures ? 1 : 0;
    }
    record(second.vs);
    record(second.vs);  // repeated discovery does not duplicate records
    if (scenario == "retry-save") pipeline_cache_save();
    if (scenario == "periodic-save") {
        for (int i = 0; i < 10; i++) creation_log(60);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!fs::exists(keys_path()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        expect(read(keys_path()) == keys({second}), "new records are saved at the existing periodic interval");
    }
    if (scenario == "writer-join") {
        pipeline_cache_save();
        {
            std::unique_lock lk(driver_mutex);
            expect(driver_cv.wait_for(lk, std::chrono::seconds(5), [] { return driver_entered; }), "save worker starts");
        }
        record(first.vs);  // discovered after the worker took its snapshot
        auto shutdown = std::async(std::launch::async, pipeline_cache_shutdown);
        expect(shutdown.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
               "shutdown waits for the active writer");
        {
            std::lock_guard lk(driver_mutex);
            release_driver = true;
        }
        driver_cv.notify_all();
        shutdown.get();
        expect(read(keys_path()) == keys({second, first}), "final snapshot includes records discovered during a save");
    } else {
        pipeline_cache_shutdown();
        if (scenario == "failed-open" || scenario == "failed-write" || scenario == "incomplete-driver")
            expect(read(blob_path()) == old_blob, "failed saves preserve the previous driver cache");
        else if (scenario == "failed-replace")
            expect(read(blob_path() / "marker") == "keep", "failed replacement preserves the destination");
        else
            expect(read(blob_path()) == new_blob, "short sessions and same-size cache changes reach disk");
        if (scenario != "failed-open" && scenario != "failed-write")
            expect(read(keys_path()) == keys(scenario == "roundtrip" ? std::vector<WarmRec>{first, second} :
                                                                            std::vector<WarmRec>{second}),
                   "complete records are saved without duplicates or partial entries");
        else
            expect(read(keys_path()) == keys({first}), "failed record writes preserve previous records");
    }
    pipeline_cache_shutdown();  // a second shutdown is harmless
    for (const auto& entry : fs::directory_iterator(root))
        expect(utf8(entry.path()).find(".tmp.") == std::string::npos, "failed saves clean up temporary files");
    fs::remove_all(root);
    return failures ? 1 : 0;
}
