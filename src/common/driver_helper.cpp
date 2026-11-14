//
// driver_helper.cpp
//
// Self-contained Vulkan driver loader for ANGLE's libvulkan_loader.
//
// ==========================================================================
// Improvements (carried over from the previous revision):
//
//   [I1]  self_dir() is cached; /proc/self/maps fallback scans only once.
//   [I2]  Every dlerror() result is copied to std::string BEFORE any dlog(),
//         because std::format may internally call into the linker on some
//         libc++ builds, invalidating the thread-local error buffer.
//   [I3]  Namespace creation retries with a unique name
//         ("<base>-<pid>-<counter>") when android_create_namespace returns
//         NULL.  A WARN marks this case so the caller knows the driver
//         may have been initialized twice.
//   [I4]  Driver name is checked for the substring "vulkan." at runtime.
//   [I5]  probe_icd downgrades the vkCreateInstance check to a WARN.
//   [I6]  probe_loader's /proc/self/maps cross-check is a NOTE, not a fail.
//   [I7]  android_link_namespaces return values are logged.
//   [I8]  Each log line carries [pid] so multi-instance failures can be
//         traced from logcat alone.
//
//   [FIX] print_loader_path no longer uses dlinfo / RTLD_DI_LINKMAP.
//         Neither exists in bionic; dladdr() on a resolved symbol is used
//         instead (portable across API levels).
//
//   [SAFE-BUFFERS] Replaced all C-string helpers (strcmp/strstr/strchr/
//         fgets/snprintf) with std::string / std::string_view / std::format
//         / std::ifstream to satisfy clang's -Wunsafe-buffer-usage plugin.
//
// ==========================================================================
// Deployment layout this file targets:
//     .../lib/libGLESv2_angle.so       <- contains this driver_helper
//     .../lib/liblinkerhook.so         <- UNMODIFIABLE
//     .../lib/libpojavexec.so          <- not used by this path
//     .../lib/libvulkan_freedreno.so   <- turnip driver
//     /system/lib64/libvulkan.so       <- system loader
//
// Contract with liblinkerhook (UNMODIFIABLE):
//   - exports linker_hook_set_handles(void*, void*, void*)
//   - 2nd arg is cast to the 4-arg __loader_android_dlopen_ext
//   - hooks only intercept paths containing "vulkan."
//   - overrides libdl.so's weak android_load_sphal_library with a strong
//     definition; must therefore be RTLD_GLOBAL in the target namespace
//     so libvulkan.so's UND symbol resolves to the strong version
//
// Therefore:
//   - driver dir is auto-derived from dladdr(libGLESv2_angle.so)
//   - hook, driver, libvulkan.so are loaded into ONE isolated namespace
//   - hook is RTLD_GLOBAL, driver & loader are RTLD_LOCAL
//   - bionic symbols used by driver_helper itself are resolved BEFORE
//     loading the hook (otherwise dlsym may hit the hook)
//   - set_handles() is called BEFORE loading libvulkan.so
//   - loader & driver are probed with real Vulkan calls, cross-checked
//     against /proc/self/maps; only a healthy pair is cached
//
// Activation:
//   ANGLE_LOAD_VULKAN_WITH_BUILTIN_DRIVER_HELPER = "true" | "1"
//   ANGLE_LIBVULKAN_NAME        (driver name; default libvulkan_freedreno.so)
//   ANGLE_VULKAN_DRIVER_DIR     (optional; auto-derived otherwise)
//

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>

#include <dlfcn.h>
#include <android/dlext.h>
#include <android/api-level.h>

extern "C" void* load_vulkan_with_driver_helper();

namespace {

constexpr const char* kEnableEnv         = "ANGLE_LOAD_VULKAN_WITH_BUILTIN_DRIVER_HELPER";
constexpr const char* kDriverNameEnv     = "ANGLE_LIBVULKAN_NAME";
constexpr const char* kDriverDirEnv      = "ANGLE_VULKAN_DRIVER_DIR";
constexpr const char* kDefaultDriverName = "libvulkan_freedreno.so";
constexpr const char* kLinkerHookName    = "liblinkerhook.so";
constexpr const char* kNamespaceName     = "angle-driver-helper";

#if defined(__LP64__)
constexpr const char* kSystemLoaderPath = "/system/lib64/libvulkan.so";
constexpr const char* kDefaultLibPath   = "/system/lib64:/vendor/lib64:/system_ext/lib64";
#else
constexpr const char* kSystemLoaderPath = "/system/lib/libvulkan.so";
constexpr const char* kDefaultLibPath   = "/system/lib:/vendor/lib:/system_ext/lib";
#endif

// Filename alias used to load libvulkan.so via ANDROID_DLEXT_USE_LIBRARY_FD.
// MUST NOT contain "vulkan." or liblinkerhook will intercept it and return
// the driver handle instead of the loader handle.
constexpr const char* kLoaderAlias = "libangle_vulkan.so";

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
template <class... Args>
void dlog(std::format_string<Args...> fmt, Args&&... args) {
    auto body = std::format(fmt, std::forward<Args>(args)...);
    std::string line = "[ANGLE][driver_helper][";
    line += std::to_string(static_cast<long long>(getpid()));
    line += "] ";
    line.append(body).push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stdout);
}

std::string hexp(const void* p) {
    return std::format("{:#x}", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)));
}

// [I2] Copy dlerror() into a std::string immediately; never pass the raw
// pointer into dlog()/std::format(), which may invoke dl* on some libc++.
std::string last_dlerror() {
    const char* e = dlerror();
    return e ? std::string(e) : std::string("-");
}

// ---------------------------------------------------------------------------
// Env
// ---------------------------------------------------------------------------
bool is_enabled() {
    const char* e = std::getenv(kEnableEnv);
    if (!e || !*e) {
        dlog("disabled: {} unset", kEnableEnv);
        return false;
    }
    std::string_view sv(e);
    bool on = (sv == "true") || (sv == "1");
    dlog("{}={} -> {}", kEnableEnv, e, on ? "enabled" : "disabled");
    return on;
}

const char* resolve_driver_name() {
    const char* n = std::getenv(kDriverNameEnv);
    const char* chosen = (n && *n) ? n : kDefaultDriverName;

    // [I4] liblinkerhook matches "vulkan." substring on every dlopen path.
    // A driver name containing it would be intercepted and recursively
    // returned as the hook's ready_handle -> stack overflow.
    if (std::string_view(chosen).find("vulkan.") != std::string_view::npos) {
        dlog("ERROR: driver name '{}' contains the forbidden substring "
             "'vulkan.'; liblinkerhook would intercept its dlopen",
             chosen);
        return nullptr;
    }
    dlog("driver name = '{}' (from {})", chosen, (n && *n) ? kDriverNameEnv : "default");
    return chosen;
}

// ---------------------------------------------------------------------------
// Process-wide cache (replaces VULKAN_PTR env var)
// ---------------------------------------------------------------------------
std::atomic<void*> g_cached_loader{nullptr};

void* read_cached() {
    return g_cached_loader.load(std::memory_order_acquire);
}

void store_cached(void* h) {
    g_cached_loader.store(h, std::memory_order_release);
}

bool valid_handle(void* h) {
    return h && h != RTLD_DEFAULT && h != RTLD_NEXT;
}

// ---------------------------------------------------------------------------
// [I1] Self directory, cached
// ---------------------------------------------------------------------------
const std::string& self_dir() {
    static const std::string s = []() -> std::string {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&load_vulkan_with_driver_helper), &info)
            && info.dli_fname && *info.dli_fname) {
            std::string path(info.dli_fname);
            auto del = path.find(" (deleted)");
            if (del != std::string::npos) path.resize(del);
            auto slash = path.find_last_of('/');
            if (slash != std::string::npos && slash > 0) {
                return path.substr(0, slash);
            }
        }
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find("libGLESv2_angle.so") == std::string::npos) continue;
            auto slash = line.find('/');
            if (slash == std::string::npos) continue;
            std::string path = line.substr(slash);
            while (!path.empty() &&
                   (path.back() == '\n' || path.back() == '\r' ||
                    path.back() == ' '  || path.back() == '\t')) {
                path.pop_back();
            }
            auto last_slash = path.find_last_of('/');
            if (last_slash != std::string::npos && last_slash > 0) {
                return path.substr(0, last_slash);
            }
        }
        return {};
    }();
    return s;
}

// ---------------------------------------------------------------------------
// Typedefs
// ---------------------------------------------------------------------------
using dlopen_ext_3arg_fn  = void* (*)(const char*, int, const android_dlextinfo*);
using dlopen_ext_4arg_fn  = void* (*)(const char*, int, const android_dlextinfo*, const void*);
using get_exported_ns_fn  = struct android_namespace_t* (*)(const char*);
using create_ns_fn        = struct android_namespace_t* (*)(
    const char*, const char*, const char*, uint64_t, const char*, struct android_namespace_t*);
using link_ns_fn          = void* (*)(struct android_namespace_t*,
                                      struct android_namespace_t*,
                                      const char*);
using set_handles_fn      = void (*)(void*, void*, void*);
using gipa_t              = void* (*)(void*, const char*);
using enum_ext_t          = int32_t (*)(const char*, uint32_t*, void*);

// ---------------------------------------------------------------------------
// Symbol resolution  (never via RTLD_DEFAULT; always via concrete handles)
// ---------------------------------------------------------------------------
void* libdl_handle() {
    static void* h = nullptr;
    static bool tried = false;
    if (!tried) { tried = true; h = dlopen("libdl.so", RTLD_LOCAL | RTLD_LAZY); }
    return h;
}

void* libdl_android_handle() {
    static void* h = nullptr;
    static bool tried = false;
    if (!tried) { tried = true; h = dlopen("libdl_android.so", RTLD_LOCAL | RTLD_LAZY); }
    return h;
}

dlopen_ext_3arg_fn resolve_dlopen_ext_3() {
    static dlopen_ext_3arg_fn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    if (auto h = libdl_handle())
        fn = reinterpret_cast<dlopen_ext_3arg_fn>(dlsym(h, "android_dlopen_ext"));
    if (!fn) if (auto h = libdl_android_handle())
        fn = reinterpret_cast<dlopen_ext_3arg_fn>(dlsym(h, "android_dlopen_ext"));
    if (fn) dlog("resolved android_dlopen_ext = {}", hexp((void*)fn));
    else    dlog("FAILED to resolve android_dlopen_ext");
    return fn;
}

dlopen_ext_4arg_fn resolve_dlopen_ext_4() {
    static dlopen_ext_4arg_fn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    if (auto h = libdl_android_handle())
        fn = reinterpret_cast<dlopen_ext_4arg_fn>(dlsym(h, "__loader_android_dlopen_ext"));
    if (fn) {
        dlog("resolved __loader_android_dlopen_ext = {}", hexp((void*)fn));
    } else {
        dlog("__loader_android_dlopen_ext not found; "
             "falling back to 3-arg android_dlopen_ext (ABI-tolerated)");
        fn = reinterpret_cast<dlopen_ext_4arg_fn>(resolve_dlopen_ext_3());
    }
    return fn;
}

get_exported_ns_fn resolve_get_exported_ns() {
    static get_exported_ns_fn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    if (auto h = libdl_android_handle())
        fn = reinterpret_cast<get_exported_ns_fn>(dlsym(h, "android_get_exported_namespace"));
    if (!fn) if (auto h = libdl_handle())
        fn = reinterpret_cast<get_exported_ns_fn>(dlsym(h, "android_get_exported_namespace"));
    if (fn) dlog("resolved android_get_exported_namespace = {}", hexp((void*)fn));
    else    dlog("FAILED to resolve android_get_exported_namespace");
    return fn;
}

set_handles_fn resolve_set_handles(void* linkerhook) {
    if (!linkerhook) return nullptr;
    void* s = dlsym(linkerhook, "linker_hook_set_handles");
    if (s) {
        dlog("found linker_hook_set_handles = {}", hexp(s));
        return reinterpret_cast<set_handles_fn>(s);
    }
    s = dlsym(linkerhook, "app__pojav_linkerhook_pass_handles");
    if (s) {
        dlog("found app__pojav_linkerhook_pass_handles = {}", hexp(s));
        return reinterpret_cast<set_handles_fn>(s);
    }
    dlog("ERROR: liblinkerhook exports neither linker_hook_set_handles "
         "nor app__pojav_linkerhook_pass_handles");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Namespace
// ---------------------------------------------------------------------------
struct android_namespace_t* create_driver_namespace(const char* driver_dir, const char* name) {
    if (android_get_device_api_level() < 26) {
        dlog("cannot create namespace: API < 26 (api_level={})",
             android_get_device_api_level());
        return nullptr;
    }
    void* h = libdl_android_handle();
    if (!h) { dlog("libdl_android.so unavailable; cannot create namespace"); return nullptr; }

    auto create = reinterpret_cast<create_ns_fn>(dlsym(h, "android_create_namespace"));
    auto link   = reinterpret_cast<link_ns_fn>(dlsym(h, "android_link_namespaces"));
    if (!create || !link) {
        dlog("android_create_namespace/link_namespaces not exported "
             "(create={}, link={})", hexp((void*)create), hexp((void*)link));
        return nullptr;
    }

    dlog("creating namespace '{}' (ld='{}', default='{}', type=3)",
         name, driver_dir, kDefaultLibPath);
    struct android_namespace_t* ns = create(
        name, driver_dir, kDefaultLibPath, 3,
        "/system/:/system_ext/:/data/:/vendor/:/apex/", nullptr);
    if (!ns) {
        dlog("android_create_namespace('{}') returned NULL", name);
        return nullptr;
    }
    // [I7] log link results
    auto link_result = [&](const char* soname) {
        (void)dlerror();
        void* r = link(ns, nullptr, soname);
        auto e = last_dlerror();
        dlog("link_namespaces(ns, NULL, '{}') -> {} (err={})",
             soname, hexp(r), e);
    };
    link_result("ld-android.so");
    link_result("libnativeloader.so");
    link_result("libnativeloader_lazy.so");

    dlog("namespace '{}' created = {}", name, hexp((void*)ns));
    return ns;
}

// [I3] Retry with a unique name when the base name is rejected.
struct android_namespace_t* create_driver_namespace_with_retry(
        const char* driver_dir, const char* base_name) {
    if (auto* ns = create_driver_namespace(driver_dir, base_name)) return ns;

    static std::atomic<uint32_t> s_seq{0};
    std::string base_sv(base_name);
    std::string unique = std::format(
        "{}-{}-{}",
        base_sv,
        static_cast<long long>(getpid()),
        s_seq.fetch_add(1, std::memory_order_relaxed));
    dlog("namespace '{}' unavailable, retrying as '{}' "
         "(WARN: this instance will get its own driver copy; "
         "turnip may be initialized more than once)", base_sv, unique);
    return create_driver_namespace(driver_dir, unique.c_str());
}

void* load_in_namespace(struct android_namespace_t* ns, const char* name, int flags) {
    auto dl3 = resolve_dlopen_ext_3();
    if (!ns || !dl3) {
        dlog("load_in_namespace('{}'): ns or dl3 missing (ns={}, dl3={}); "
             "falling back to dlopen", name, hexp((void*)ns), hexp((void*)dl3));
        (void)dlerror();
        void* r = dlopen(name, flags);
        dlog("dlopen('{}', {:#x}) -> {} (err={})",
             name, flags, hexp(r), r ? "-" : last_dlerror());
        return r;
    }
    android_dlextinfo info{};
    info.flags = ANDROID_DLEXT_USE_NAMESPACE;
    info.library_namespace = ns;
    (void)dlerror();
    void* r = dl3(name, flags, &info);
    dlog("android_dlopen_ext(ns={}, '{}', {:#x}) -> {} (err={})",
         hexp((void*)ns), name, flags, hexp(r), r ? "-" : last_dlerror());
    return r;
}

// ---------------------------------------------------------------------------
// Loader load (bypasses hook by using an alias filename + a real fd)
// ---------------------------------------------------------------------------
void* load_loader_bypassing_hook(struct android_namespace_t* ns) {
    int fd = open(kSystemLoaderPath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        dlog("open({}) failed: {} (errno={})",
             kSystemLoaderPath, std::strerror(errno), errno);
        return nullptr;
    }
    auto dl3 = resolve_dlopen_ext_3();
    if (!dl3) {
        dlog("android_dlopen_ext missing; cannot load loader via fd");
        close(fd);
        return nullptr;
    }
    android_dlextinfo info{};
    info.flags = ANDROID_DLEXT_USE_NAMESPACE | ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = fd;
    info.library_namespace = ns;
    (void)dlerror();
    dlog("loading real loader: android_dlopen_ext(ns={}, alias='{}', fd={}, "
         "USE_LIBRARY_FD)", hexp((void*)ns), kLoaderAlias, fd);
    void* h = dl3(kLoaderAlias, RTLD_LOCAL | RTLD_NOW, &info);
    auto err = last_dlerror();
    close(fd);
    if (!h) {
        dlog("load '{}' via fd failed: {}", kLoaderAlias, err);
        return nullptr;
    }
    dlog("loader loaded as '{}' -> {}", kLoaderAlias, hexp(h));

    // Sanity: must export vkGetInstanceProcAddr (loader).
    void* gipa = dlsym(h, "vkGetInstanceProcAddr");
    void* icd  = dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa && icd) {
        dlog("ERROR: handle {} exports vk_icdGetInstanceProcAddr but not "
             "vkGetInstanceProcAddr - we got the driver, not the loader!",
             hexp(h));
        return nullptr;
    }
    if (icd) {
        dlog("WARN: handle {} also exports vk_icdGetInstanceProcAddr; "
             "suggests this loader build also carries a driver entrypoint",
             hexp(h));
    }
    return h;
}

// ---------------------------------------------------------------------------
// Health checks
// ---------------------------------------------------------------------------
bool probe_icd(void* driver_handle, std::string_view driver_name) {
    if (!valid_handle(driver_handle)) {
        dlog("probe_icd: invalid handle for {}", driver_name);
        return false;
    }
    void* sym = dlsym(driver_handle, "vk_icdGetInstanceProcAddr");
    const char* entry = "vk_icdGetInstanceProcAddr";
    if (!sym) { sym = dlsym(driver_handle, "vkGetInstanceProcAddr"); entry = "vkGetInstanceProcAddr"; }
    if (!sym) {
        dlog("probe_icd({}): no vk_icd/vkGetInstanceProcAddr", driver_name);
        return false;
    }
    auto gipa = reinterpret_cast<gipa_t>(sym);

    // [I5] vkCreateInstance is loader-imported on some ICDs and may not be
    // exposed through the ICD entry point; downgrade to WARN.
    if (!gipa(nullptr, "vkCreateInstance")) {
        dlog("probe_icd({}): WARN {}(NULL, vkCreateInstance) == NULL "
             "(tolerated: some ICDs don't re-export core entry points)",
             driver_name, entry);
    }

    auto enum_ext = reinterpret_cast<enum_ext_t>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!enum_ext) {
        dlog("probe_icd({}): cannot resolve vkEnumExt", driver_name);
        return false;
    }
    uint32_t n = 0;
    int32_t r = enum_ext(nullptr, &n, nullptr);
    dlog("probe_icd({}): enum_ext VkResult={} count={}", driver_name, r, n);
    return r == 0 && n > 0;
}

int count_driver_exec_maps(std::string_view driver_name) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        dlog("cannot open /proc/self/maps: {}", std::strerror(errno));
        return -1;
    }
    int n = 0;
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(driver_name) == std::string::npos) continue;
        // Format: "addr perms offset dev inode pathname"
        // The first space separates address from perms; perms[2] == 'x' means r-xp/rwxp etc.
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string_view perms(line);
        perms.remove_prefix(sp + 1);
        if (perms.size() < 3 || perms[2] != 'x') continue;
        ++n;
    }
    return n;
}

bool probe_loader(void* loader_handle, std::string_view driver_name) {
    if (!valid_handle(loader_handle)) {
        dlog("probe_loader: invalid handle");
        return false;
    }
    void* sym = dlsym(loader_handle, "vkGetInstanceProcAddr");
    if (!sym) {
        dlog("probe_loader: missing vkGetInstanceProcAddr");
        return false;
    }
    auto gipa = reinterpret_cast<gipa_t>(sym);
    if (!gipa(nullptr, "vkCreateInstance")) {
        dlog("probe_loader: gipa(NULL, vkCreateInstance) == NULL");
        return false;
    }
    auto enum_ext = reinterpret_cast<enum_ext_t>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!enum_ext) {
        dlog("probe_loader: cannot resolve vkEnumExt");
        return false;
    }
    uint32_t n = 0;
    int32_t r = enum_ext(nullptr, &n, nullptr);
    dlog("probe_loader: enum_ext VkResult={} count={}", r, n);
    if (r != 0 || n == 0) return false;

    int maps = count_driver_exec_maps(driver_name);
    if (maps < 0) {
        dlog("probe_loader: loader OK ({} ext), maps unreadable", n);
        return true;
    }
    if (maps == 0) {
        // [I6] memfd-loaded drivers won't match their original filename in
        // /proc/self/maps.  Zero matches can be normal; only suspicious if
        // the loader really fell back to a system ICD.
        dlog("probe_loader: NOTE loader OK ({} ext) but {} has 0 r-xp mappings "
             "- normal for memfd loads, suspicious otherwise "
             "(loader may have used a system ICD)", n, driver_name);
        return true;
    }
    dlog("probe_loader: loader OK ({} ext, {} r-xp of {})", n, maps, driver_name);
    return true;
}

// [FIX] dlinfo() / RTLD_DI_LINKMAP do not exist on Android bionic.
// Resolve a known loader symbol and ask dladdr() for its image path.
void print_loader_path(void* loader_handle) {
    void* sym = dlsym(loader_handle, "vkGetInstanceProcAddr");
    if (!sym) {
        dlog("print_loader_path: cannot resolve vkGetInstanceProcAddr");
        return;
    }
    Dl_info info{};
    if (dladdr(sym, &info) && info.dli_fname && *info.dli_fname) {
        dlog("loader real path = '{}'", info.dli_fname);
    } else {
        dlog("print_loader_path: dladdr failed");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported entry point
// ---------------------------------------------------------------------------
extern "C" void* load_vulkan_with_driver_helper() {
    dlog("=== load_vulkan_with_driver_helper entered ===");

    if (!is_enabled()) return nullptr;

    if (void* c = read_cached()) {
        dlog("fast path: cached loader = {}", hexp(c));
        return c;
    }
    dlog("fast path: cache miss");

    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    if (void* c = read_cached()) {
        dlog("locked path: cached loader = {} (another thread won)", hexp(c));
        return c;
    }
    dlog("locked path: still cache miss, proceeding");

    auto dl3    = resolve_dlopen_ext_3();
    auto dl4    = resolve_dlopen_ext_4();
    auto get_ns = resolve_get_exported_ns();
    if (!dl3 || !dl4 || !get_ns) {
        dlog("aborting: required bionic symbols missing "
             "(dl3={}, dl4={}, get_ns={})",
             hexp((void*)dl3), hexp((void*)dl4), hexp((void*)get_ns));
        return nullptr;
    }

    std::string driver_dir_owned;
    const char* driver_dir = std::getenv(kDriverDirEnv);
    if (driver_dir && *driver_dir) {
        dlog("driver dir = '{}' (from {})", driver_dir, kDriverDirEnv);
    } else {
        driver_dir_owned = self_dir();
        if (driver_dir_owned.empty()) {
            dlog("aborting: cannot derive driver dir; "
                 "set ANGLE_VULKAN_DRIVER_DIR");
            return nullptr;
        }
        driver_dir = driver_dir_owned.c_str();
        dlog("driver dir = '{}' (auto)", driver_dir);
    }

    const char* driver_name = resolve_driver_name();
    if (!driver_name) {
        dlog("aborting: invalid driver name");
        return nullptr;
    }

    // [I3] Create with retry-on-conflict
    struct android_namespace_t* target_ns =
        create_driver_namespace_with_retry(driver_dir, kNamespaceName);
    if (!target_ns) {
        dlog("aborting: namespace creation failed");
        return nullptr;
    }

    dlog("loading {} into target ns (RTLD_GLOBAL | RTLD_NOW)", kLinkerHookName);
    void* linkerhook = load_in_namespace(
        target_ns, kLinkerHookName, RTLD_GLOBAL | RTLD_NOW);
    if (!linkerhook) {
        dlog("aborting: failed to load {}", kLinkerHookName);
        return nullptr;
    }

    dlog("loading driver '{}' into target ns (RTLD_LOCAL | RTLD_NOW)", driver_name);
    void* driver_handle = load_in_namespace(
        target_ns, driver_name, RTLD_LOCAL | RTLD_NOW);
    if (!driver_handle) {
        dlog("aborting: failed to load driver '{}'", driver_name);
        return nullptr;
    }
    dlog("driver loaded at {}", hexp(driver_handle));

    if (!probe_icd(driver_handle, driver_name)) {
        dlog("aborting: driver probe failed");
        return nullptr;
    }

    auto set_handles = resolve_set_handles(linkerhook);
    if (!set_handles) {
        dlog("aborting: cannot resolve set_handles in {}", kLinkerHookName);
        return nullptr;
    }
    dlog("calling set_handles(driver={}, dlopen_ext_4arg={}, get_ns={})",
         hexp(driver_handle), hexp((void*)dl4), hexp((void*)get_ns));
    set_handles(driver_handle,
                reinterpret_cast<void*>(dl4),
                reinterpret_cast<void*>(get_ns));
    dlog("set_handles returned");

    dlog("loading system loader via fd bypass (filename alias = '{}')",
         kLoaderAlias);
    void* loader_handle = load_loader_bypassing_hook(target_ns);
    if (!loader_handle) {
        dlog("aborting: loader load failed");
        return nullptr;
    }
    print_loader_path(loader_handle);

    if (!probe_loader(loader_handle, driver_name)) {
        dlog("aborting: loader probe failed");
        return nullptr;
    }

    if (!valid_handle(loader_handle)) {
        dlog("aborting: loader handle failed final validation");
        return nullptr;
    }

    store_cached(loader_handle);
    dlog("SUCCESS: cached loader = {} (static, not an env var)",
         hexp(loader_handle));
    return loader_handle;
}
