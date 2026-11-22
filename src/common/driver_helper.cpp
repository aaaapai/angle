//
// driver_helper.cpp
//
// Vulkan driver loader for ANGLE's libvulkan_loader.
//
// ==========================================================================
// Design constraints:
//
//   - liblinkerhook.so is UNMODIFIABLE; RTLD_GLOBAL is required so
//     libvulkan.so's UND android_load_sphal_library binds to the hook.
//   - libdl_android.so / ld-android.so are bionic internals.  dlopen'ing
//     them from an app namespace can abort the linker on Android 12+.
//     We therefore probe them with RTLD_NOLOAD and only take a handle if
//     they are already mapped.
//   - __loader_android_create_namespace (7-arg) with caller_addr is used
//     when available (proven by nsbypass.cpp).
//   - get_exported_namespace returning NULL is FATAL, because
//     liblinkerhook dereferences it on every non-"vulkan." sphal load.
//
// Debug contract:
//   Every dlopen/dlsym/open prints a ">>> ..." line BEFORE and a "<<< ..."
//   line AFTER.  If the process dies inside a libc call, the last ">>>"
//   line identifies exactly which call crashed.  Critical failures call
//   std::abort() (SIGABRT + tombstone) instead of returning nullptr.
// ==========================================================================

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
constexpr const char* kDriverNameEnv     = "DRIVER_LIB";
constexpr const char* kDriverDirEnv      = "DRIVER_PATH";
constexpr const char* kSafeDlopenEnv     = "ANGLE_DRIVER_HELPER_SAFE_DLOPEN";
constexpr const char* kDefaultDriverName = "libvulkan_freedreno.so";
constexpr const char* kLinkerHookName    = "liblinkerhook.so";

#if defined(__LP64__)
constexpr const char* kSystemLoaderPath = "/system/lib64/libvulkan.so";
constexpr const char* kDefaultLibPath   = "/system/lib64:/vendor/lib64:/system_ext/lib64";
#else
constexpr const char* kSystemLoaderPath = "/system/lib/libvulkan.so";
constexpr const char* kDefaultLibPath   = "/system/lib:/vendor/lib:/system_ext/lib";
#endif

constexpr const char* kLoaderAlias = "libangkan.so";

// ---------------------------------------------------------------------------
// Logging (flush every line so nothing is lost if we abort)
// ---------------------------------------------------------------------------
template <class... Args>
void dlog(std::format_string<Args...> fmt, Args&&... args) {
    auto body = std::format(fmt, std::forward<Args>(args)...);
    std::string line = "[ANGLE][driver_helper][";
    line += std::to_string(static_cast<long long>(getpid()));
    line += "] ";
    line.append(body).push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

std::string hexp(const void* p) {
    return std::format("{:#x}", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)));
}

std::string last_dlerror() {
    const char* e = dlerror();
    return e ? std::string(e) : std::string("-");
}

[[noreturn]] void fatal(const char* what) {
    dlog("[FATAL] {}", what);
    std::fflush(nullptr);
    std::abort();
}

// ---------------------------------------------------------------------------
// Safe wrappers (every call is logged pre & post)
// ---------------------------------------------------------------------------
void* safe_dlopen(const char* name, int flags, const char* ctx) {
    dlog(">>> dlopen(\"{}\", {:#x})  [{}]", name, flags, ctx);
    (void)dlerror();
    void* h = dlopen(name, flags);
    if (h) dlog("<<< dlopen(\"{}\") OK   = {}  [{}]", name, hexp(h), ctx);
    else   dlog("<<< dlopen(\"{}\") FAIL = nullptr  err=\"{}\"  [{}]",
                name, last_dlerror(), ctx);
    return h;
}

void* safe_dlsym(void* handle, const char* name, const char* ctx) {
    dlog(">>> dlsym({}, \"{}\")  [{}]", hexp(handle), name, ctx);
    (void)dlerror();
    void* s = dlsym(handle, name);
    if (s) dlog("<<< dlsym({}, \"{}\") OK   = {}  [{}]",
                hexp(handle), name, hexp(s), ctx);
    else   dlog("<<< dlsym({}, \"{}\") FAIL = nullptr  err=\"{}\"  [{}]",
                hexp(handle), name, last_dlerror(), ctx);
    return s;
}

int safe_open(const char* path, int flags, const char* ctx) {
    dlog(">>> open(\"{}\", {:#x})  [{}]", path, flags, ctx);
    int fd = open(path, flags);
    if (fd >= 0) dlog("<<< open(\"{}\") OK   fd={}  [{}]", path, fd, ctx);
    else         dlog("<<< open(\"{}\") FAIL = -1  errno={} ({})  [{}]",
                      path, errno, std::strerror(errno), ctx);
    return fd;
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

bool safe_dlopen_mode() {
    const char* e = std::getenv(kSafeDlopenEnv);
    if (!e || !*e) return true;  // default: safe
    std::string_view sv(e);
    bool on = !(sv == "0" || sv == "false");
    dlog("{}={} -> {}", kSafeDlopenEnv, e, on ? "safe (NOLOAD-only)" : "unsafe (allow real load)");
    return on;
}

const char* resolve_driver_name() {
    const char* n = std::getenv(kDriverNameEnv);
    const char* chosen = (n && *n) ? n : kDefaultDriverName;

    if (std::string_view(chosen).find("vulkan.") != std::string_view::npos) {
        dlog("ERROR: driver name '{}' contains 'vulkan.'", chosen);
        return nullptr;
    }
    dlog("driver name = '{}' (from {})", chosen,
         (n && *n) ? kDriverNameEnv : "default");
    return chosen;
}

// ---------------------------------------------------------------------------
// Cache
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
// Self directory
// ---------------------------------------------------------------------------
const std::string& self_dir() {
    static const std::string s = []() -> std::string {
        dlog("[self_dir] trying dladdr(&load_vulkan_with_driver_helper)");
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&load_vulkan_with_driver_helper), &info)
            && info.dli_fname && *info.dli_fname) {
            std::string path(info.dli_fname);
            auto del = path.find(" (deleted)");
            if (del != std::string::npos) path.resize(del);
            auto slash = path.find_last_of('/');
            if (slash != std::string::npos && slash > 0) {
                std::string r = path.substr(0, slash);
                dlog("[self_dir] dladdr -> '{}'", r);
                return r;
            }
        }
        dlog("[self_dir] dladdr failed, scanning /proc/self/maps");
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find("libGLESv2_angle.so") == std::string::npos) continue;
            auto slash = line.find('/');
            if (slash == std::string::npos) continue;
            std::string p = line.substr(slash);
            while (!p.empty() &&
                   (p.back() == '\n' || p.back() == '\r' ||
                    p.back() == ' '  || p.back() == '\t')) p.pop_back();
            auto last_slash = p.find_last_of('/');
            if (last_slash != std::string::npos && last_slash > 0) {
                std::string r = p.substr(0, last_slash);
                dlog("[self_dir] maps -> '{}'", r);
                return r;
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
using create_ns_7arg_fn   = struct android_namespace_t* (*)(
    const char*, const char*, const char*, uint64_t, const char*,
    struct android_namespace_t*, const void*);
using link_ns_fn          = void* (*)(struct android_namespace_t*,
                                      struct android_namespace_t*,
                                      const char*);
using set_handles_fn      = void (*)(void*, void*, void*);
using gipa_t              = void* (*)(void*, const char*);
using enum_ext_t          = int32_t (*)(const char*, uint32_t*, void*);

// ---------------------------------------------------------------------------
// Handles (NOLOAD-only for dangerous libs)
// ---------------------------------------------------------------------------
void* libdl_handle() {
    static void* h = nullptr;
    static bool tried = false;
    if (tried) return h;
    tried = true;
    h = safe_dlopen("libdl.so", RTLD_LOCAL | RTLD_LAZY, "libdl_handle");
    return h;
}

// libdl_android.so is a private bionic lib.  dlopen'ing it from an app
// namespace can abort the linker on Android 12+.  Take the handle only
// if it is already mapped (RTLD_NOLOAD).  Only fall through to a real
// dlopen if ANGLE_DRIVER_HELPER_SAFE_DLOPEN=0 (explicit opt-in).
void* libdl_android_handle() {
    static void* h = nullptr;
    static bool tried = false;
    if (tried) return h;
    tried = true;

    dlog("[libdl_android_handle] probing with RTLD_NOLOAD");
    h = dlopen("libdl_android.so", RTLD_LOCAL | RTLD_LAZY | RTLD_NOLOAD);
    if (h) {
        dlog("[libdl_android_handle] already loaded -> {}", hexp(h));
        return h;
    }
    dlog("[libdl_android_handle] not loaded: err=\"{}\"", last_dlerror());

    if (safe_dlopen_mode()) {
        dlog("[libdl_android_handle] SAFE mode: NOT attempting real load "
             "(prevents app-namespace linker abort)");
        return nullptr;
    }
    dlog("[libdl_android_handle] UNSAFE mode: attempting real load");
    h = safe_dlopen("libdl_android.so", RTLD_LOCAL | RTLD_LAZY,
                    "libdl_android_handle/real");
    return h;
}

// ld-android.so is the linker itself; dlopen'ing it is a singleton
// violation.  Only ever take it via NOLOAD.
void* ld_android_handle() {
    static void* h = nullptr;
    static bool tried = false;
    if (tried) return h;
    tried = true;

    dlog("[ld_android_handle] probing with RTLD_NOLOAD");
    h = dlopen("ld-android.so", RTLD_LOCAL | RTLD_LAZY | RTLD_NOLOAD);
    if (h) {
        dlog("[ld_android_handle] already loaded -> {}", hexp(h));
        return h;
    }
    dlog("[ld_android_handle] not loaded: err=\"{}\" — skipping "
         "(dlopen of linker itself is unsafe)", last_dlerror());
    return nullptr;
}

// libc.so is always mapped in a running process; use NOLOAD to be safe.
void* libc_handle() {
    static void* h = nullptr;
    static bool tried = false;
    if (tried) return h;
    tried = true;
    dlog("[libc_handle] probing libc.so with RTLD_NOLOAD");
    h = dlopen("libc.so", RTLD_LOCAL | RTLD_LAZY | RTLD_NOLOAD);
    if (h) {
        dlog("[libc_handle] already loaded -> {}", hexp(h));
        return h;
    }
    dlog("[libc_handle] not loaded, doing real dlopen");
    h = safe_dlopen("libc.so", RTLD_LOCAL | RTLD_LAZY, "libc_handle");
    return h;
}

// ---------------------------------------------------------------------------
// Resolution: 3-arg dlopen_ext
// ---------------------------------------------------------------------------
dlopen_ext_3arg_fn resolve_dlopen_ext_3() {
    static dlopen_ext_3arg_fn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;

    dlog("[resolve_dlopen_ext_3] ENTER");

    if (void* h = libdl_android_handle()) {
        fn = reinterpret_cast<dlopen_ext_3arg_fn>(
            safe_dlsym(h, "android_dlopen_ext", "resolve3/libdl_android"));
        if (fn) {
            dlog("[resolve_dlopen_ext_3] OK from libdl_android.so = {}", hexp((void*)fn));
            return fn;
        }
    }

    if (void* h = libdl_handle()) {
        fn = reinterpret_cast<dlopen_ext_3arg_fn>(
            safe_dlsym(h, "android_dlopen_ext", "resolve3/libdl"));
        if (fn) {
            dlog("[resolve_dlopen_ext_3] OK from libdl.so = {}", hexp((void*)fn));
            return fn;
        }
    }
    dlog("[resolve_dlopen_ext_3] FAILED");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Resolution: 4-arg __loader_android_dlopen_ext
// ---------------------------------------------------------------------------
dlopen_ext_4arg_fn resolve_dlopen_ext_4() {
    static dlopen_ext_4arg_fn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;

    dlog("[resolve_dlopen_ext_4] ENTER");

    if (void* h = ld_android_handle()) {
        fn = reinterpret_cast<dlopen_ext_4arg_fn>(
            safe_dlsym(h, "__loader_android_dlopen_ext", "resolve4/ld_android"));
        if (fn) {
            dlog("[resolve_dlopen_ext_4] OK from ld-android.so = {}", hexp((void*)fn));
            return fn;
        }
    }

    if (void* h = libdl_android_handle()) {
        fn = reinterpret_cast<dlopen_ext_4arg_fn>(
            safe_dlsym(h, "__loader_android_dlopen_ext", "resolve4/libdl_android"));
        if (fn) {
            dlog("[resolve_dlopen_ext_4] OK from libdl_android.so = {}", hexp((void*)fn));
            return fn;
        }
    }
    dlog("[resolve_dlopen_ext_4] not found; falling back to 3-arg");
    auto f3 = resolve_dlopen_ext_3();
    fn = reinterpret_cast<dlopen_ext_4arg_fn>(f3);
    if (fn) dlog("[resolve_dlopen_ext_4] fell back to 3-arg = {}", hexp((void*)fn));
    else    dlog("[resolve_dlopen_ext_4] no 4-arg and no 3-arg — returning null");
    return fn;
}

// ---------------------------------------------------------------------------
// Resolution: get_exported_namespace
// ---------------------------------------------------------------------------
get_exported_ns_fn resolve_get_exported_ns() {
    static get_exported_ns_fn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;

    dlog("[resolve_get_exported_ns] ENTER");
    if (void* h = libdl_android_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            safe_dlsym(h, "__loader_android_get_exported_namespace",
                       "get_ns/libdl_android"));
        if (fn) { dlog("[resolve_get_exported_ns] OK (libdl_android) = {}", hexp((void*)fn)); return fn; }
    }
    if (void* h = ld_android_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            safe_dlsym(h, "__loader_android_get_exported_namespace",
                       "get_ns/ld_android"));
        if (fn) { dlog("[resolve_get_exported_ns] OK (ld-android) = {}", hexp((void*)fn)); return fn; }
    }
    if (void* h = libc_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            safe_dlsym(h, "__loader_android_get_exported_namespace",
                       "get_ns/libc"));
        if (fn) { dlog("[resolve_get_exported_ns] OK (libc) = {}", hexp((void*)fn)); return fn; }
    }
    if (void* h = libdl_android_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            safe_dlsym(h, "android_get_exported_namespace",
                       "get_ns/libdl_android/no_prefix"));
        if (fn) { dlog("[resolve_get_exported_ns] OK (libdl_android no-prefix) = {}", hexp((void*)fn)); return fn; }
    }
    if (void* h = libdl_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            safe_dlsym(h, "android_get_exported_namespace",
                       "get_ns/libdl/no_prefix"));
        if (fn) { dlog("[resolve_get_exported_ns] OK (libdl no-prefix) = {}", hexp((void*)fn)); return fn; }
    }
    dlog("[resolve_get_exported_ns] FAILED — every source exhausted");
    return nullptr;
}

// ---------------------------------------------------------------------------
// set_handles
// ---------------------------------------------------------------------------
set_handles_fn resolve_set_handles(void* linkerhook) {
    if (!linkerhook) return nullptr;
    dlog("[resolve_set_handles] ENTER (linkerhook={})", hexp(linkerhook));

    void* s = safe_dlsym(linkerhook, "linker_hook_set_handles", "set_handles");
    if (s) return reinterpret_cast<set_handles_fn>(s);

    s = safe_dlsym(linkerhook, "app__pojav_linkerhook_pass_handles",
                   "set_handles/legacy");
    if (s) return reinterpret_cast<set_handles_fn>(s);

    dlog("[resolve_set_handles] FAILED");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Namespace
// ---------------------------------------------------------------------------
__attribute__((noinline))
static struct android_namespace_t* call_create_ns_with_caller(
    create_ns_7arg_fn create,
    const char* name, const char* ld, const char* def,
    uint64_t type, const char* permitted,
    struct android_namespace_t* parent) {
    dlog("[create_ns] calling __loader_android_create_namespace(name='{}', "
         "ld='{}', parent={})", name, ld,
         parent ? "non-null" : "null");
    return create(name, ld, def, type, permitted, parent,
                  __builtin_return_address(0));
}

struct android_namespace_t* create_driver_namespace(
        const char* driver_dir, const char* name,
        get_exported_ns_fn get_ns) {
    dlog("[create_ns] ENTER name='{}' api={}", name,
         android_get_device_api_level());
    if (android_get_device_api_level() < 26) {
        dlog("[create_ns] FAIL: API < 26");
        return nullptr;
    }

    create_ns_7arg_fn create7 = nullptr;
    create_ns_fn      create6 = nullptr;
    link_ns_fn        link    = nullptr;

    if (void* h = libdl_android_handle()) {
        create7 = reinterpret_cast<create_ns_7arg_fn>(
            safe_dlsym(h, "__loader_android_create_namespace", "create_ns/7/libdl_android"));
        create6 = reinterpret_cast<create_ns_fn>(
            safe_dlsym(h, "android_create_namespace", "create_ns/6/libdl_android"));
        link    = reinterpret_cast<link_ns_fn>(
            safe_dlsym(h, "__loader_android_link_namespaces", "link_ns/libdl_android"));
        if (!link) link = reinterpret_cast<link_ns_fn>(
            safe_dlsym(h, "android_link_namespaces", "link_ns/libdl_android/no_prefix"));
    }
    if (!create7 && !create6) {
        if (void* h = ld_android_handle()) {
            create7 = reinterpret_cast<create_ns_7arg_fn>(
                safe_dlsym(h, "__loader_android_create_namespace", "create_ns/7/ld_android"));
            create6 = reinterpret_cast<create_ns_fn>(
                safe_dlsym(h, "android_create_namespace", "create_ns/6/ld_android"));
        }
    }
    if (!link) {
        if (void* h = ld_android_handle())
            link = reinterpret_cast<link_ns_fn>(
                safe_dlsym(h, "__loader_android_link_namespaces", "link_ns/ld_android"));
    }
    if (!link) {
        if (void* h = libdl_handle())
            link = reinterpret_cast<link_ns_fn>(
                safe_dlsym(h, "android_link_namespaces", "link_ns/libdl"));
    }

    if (!create7 && !create6) {
        dlog("[create_ns] FAIL: no create_namespace available");
        return nullptr;
    }
    if (!link) {
        dlog("[create_ns] FAIL: no link_namespaces available");
        return nullptr;
    }
    dlog("[create_ns] symbols: create7={} create6={} link={}",
         hexp((void*)create7), hexp((void*)create6), hexp((void*)link));

    std::string full_path = std::format("{}:{}", driver_dir, kDefaultLibPath);
    const char* permitted = "/system/:/system_ext/:/data/:/vendor/:/apex/";

    struct android_namespace_t* parent = nullptr;
    const char* parent_name = nullptr;
    if (get_ns) {
        for (const char* candidate : {"sphal", "default"}) {
            dlog("[create_ns] probing parent namespace '{}'", candidate);
            parent = get_ns(candidate);
            if (parent) { parent_name = candidate; break; }
        }
    }

    struct android_namespace_t* ns = nullptr;
    if (create7) {
        ns = call_create_ns_with_caller(create7, name,
                                        full_path.c_str(), full_path.c_str(),
                                        3, permitted, parent);
    } else {
        dlog("[create_ns] WARN: using 6-arg create (no caller_addr)");
        ns = create6(name, full_path.c_str(), full_path.c_str(),
                     3, permitted, parent);
    }
    if (!ns) {
        dlog("[create_ns] create returned null");
        return nullptr;
    }
    dlog("[create_ns] ns created = {}", hexp((void*)ns));

    auto link_result = [&](const char* soname) {
        dlog("[create_ns] linking '{}'", soname);
        (void)dlerror();
        void* r = link(ns, nullptr, soname);
        dlog("[create_ns] link '{}' -> {} err=\"{}\"",
             soname, hexp(r), r ? "-" : last_dlerror());
    };

    link_result("ld-android.so");
    link_result("libnativeloader.so");
    link_result("libnativeloader_lazy.so");
    for (const char* soname : {
             "liblog.so", "libbase.so", "libcutils.so",
             "libhardware.so", "libsync.so", "libui.so",
             "libnativewindow.so", "libandroid.so" }) {
        link_result(soname);
    }
    return ns;
}

struct android_namespace_t* create_driver_namespace_with_retry(
        const char* driver_dir, const char* base_name,
        get_exported_ns_fn get_ns) {
    std::string unique_base = std::format("{}-{}", base_name,
                                          static_cast<long long>(getpid()));
    dlog("[create_ns_retry] attempt 1 name='{}'", unique_base);
    if (auto* ns = create_driver_namespace(driver_dir, unique_base.c_str(), get_ns))
        return ns;
    static std::atomic<uint32_t> s_seq{0};
    std::string unique = std::format("{}-{}", unique_base,
                                     s_seq.fetch_add(1, std::memory_order_relaxed));
    dlog("[create_ns_retry] attempt 2 name='{}'", unique);
    return create_driver_namespace(driver_dir, unique.c_str(), get_ns);
}

// ---------------------------------------------------------------------------
// Load in namespace
// ---------------------------------------------------------------------------
void* load_in_namespace(struct android_namespace_t* ns, const char* name, int flags) {
    auto dl3 = resolve_dlopen_ext_3();
    if (!ns || !dl3) {
        dlog("[load_in_ns] ns or dl3 missing (ns={} dl3={}), falling back to dlopen",
             hexp((void*)ns), hexp((void*)dl3));
        return safe_dlopen(name, flags, "load_in_ns/fallback");
    }
    dlog(">>> dl3(ns={}, \"{}\", {:#x})", hexp((void*)ns), name, flags);
    android_dlextinfo info{};
    info.flags = ANDROID_DLEXT_USE_NAMESPACE;
    info.library_namespace = ns;
    (void)dlerror();
    void* r = dl3(name, flags, &info);
    if (r) dlog("<<< dl3(\"{}\") OK   = {}", name, hexp(r));
    else   dlog("<<< dl3(\"{}\") FAIL = nullptr err=\"{}\"", name, last_dlerror());
    return r;
}

// ---------------------------------------------------------------------------
// Loader load
// ---------------------------------------------------------------------------
void* load_loader_bypassing_hook(struct android_namespace_t* ns) {
    int fd = safe_open(kSystemLoaderPath, O_RDONLY | O_CLOEXEC, "loader");
    if (fd < 0) return nullptr;

    auto dl3 = resolve_dlopen_ext_3();
    if (!dl3) {
        dlog("[loader] dl3 missing");
        close(fd);
        return nullptr;
    }

    android_dlextinfo info{};
    info.flags = ANDROID_DLEXT_USE_NAMESPACE | ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = fd;
    info.library_namespace = ns;

    dlog(">>> dl3(ns={}, \"{}\", RTLD_LOCAL|RTLD_NOW, fd={})",
         hexp((void*)ns), kLoaderAlias, fd);
    (void)dlerror();
    void* h = dl3(kLoaderAlias, RTLD_LOCAL | RTLD_NOW, &info);
    auto err = last_dlerror();
    close(fd);
    if (!h) {
        dlog("<<< dl3(\"{}\") FAIL = nullptr err=\"{}\"", kLoaderAlias, err);
        return nullptr;
    }
    dlog("<<< dl3(\"{}\") OK = {}", kLoaderAlias, hexp(h));

    void* gipa = safe_dlsym(h, "vkGetInstanceProcAddr", "loader/check/gipa");
    void* icd  = safe_dlsym(h, "vk_icdGetInstanceProcAddr", "loader/check/icd");
    if (!gipa && icd) {
        dlog("[loader] got the DRIVER, not the loader — aborting");
        return nullptr;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Probes
// ---------------------------------------------------------------------------
bool probe_icd(void* driver_handle, std::string_view driver_name) {
    dlog("[probe_icd] ENTER driver='{}' handle={}",
         driver_name, hexp(driver_handle));
    if (!valid_handle(driver_handle)) return false;

    void* sym = safe_dlsym(driver_handle, "vk_icdGetInstanceProcAddr",
                           "probe_icd/gipa");
    if (!sym) sym = safe_dlsym(driver_handle, "vkGetInstanceProcAddr",
                               "probe_icd/gipa_fallback");
    if (!sym) {
        dlog("[probe_icd] no entry point");
        return false;
    }
    auto gipa = reinterpret_cast<gipa_t>(sym);

    if (!gipa(nullptr, "vkCreateInstance")) {
        dlog("[probe_icd] WARN vkCreateInstance == NULL (tolerated)");
    }

    auto enum_ext = reinterpret_cast<enum_ext_t>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!enum_ext) {
        dlog("[probe_icd] cannot resolve vkEnumExt");
        return false;
    }
    uint32_t n = 0;
    int32_t r = enum_ext(nullptr, &n, nullptr);
    dlog("[probe_icd] VkResult={} count={}", r, n);
    return r == 0 && n > 0;
}

int count_driver_exec_maps(std::string_view driver_name) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) return -1;
    int n = 0;
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(driver_name) == std::string::npos) continue;
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
    dlog("[probe_loader] ENTER handle={}", hexp(loader_handle));
    if (!valid_handle(loader_handle)) return false;

    void* sym = safe_dlsym(loader_handle, "vkGetInstanceProcAddr",
                           "probe_loader/gipa");
    if (!sym) {
        dlog("[probe_loader] missing vkGetInstanceProcAddr");
        return false;
    }
    auto gipa = reinterpret_cast<gipa_t>(sym);
    if (!gipa(nullptr, "vkCreateInstance")) {
        dlog("[probe_loader] gipa(NULL, vkCreateInstance) == NULL");
        return false;
    }
    auto enum_ext = reinterpret_cast<enum_ext_t>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!enum_ext) {
        dlog("[probe_loader] cannot resolve vkEnumExt");
        return false;
    }
    uint32_t n = 0;
    int32_t r = enum_ext(nullptr, &n, nullptr);
    dlog("[probe_loader] VkResult={} count={}", r, n);
    if (r != 0 || n == 0) return false;

    int maps = count_driver_exec_maps(driver_name);
    if (maps < 0) {
        dlog("[probe_loader] OK ({} ext), maps unreadable", n);
        return true;
    }
    if (maps == 0) {
        dlog("[probe_loader] NOTE {} ext, 0 r-xp of {} "
             "(normal for memfd, suspicious otherwise)", n, driver_name);
        return true;
    }
    dlog("[probe_loader] OK ({} ext, {} r-xp of {})", n, maps, driver_name);
    return true;
}

void print_loader_path(void* loader_handle) {
    void* sym = dlsym(loader_handle, "vkGetInstanceProcAddr");
    if (!sym) return;
    Dl_info info{};
    if (dladdr(sym, &info) && info.dli_fname && *info.dli_fname) {
        dlog("[loader] real path = '{}'", info.dli_fname);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
extern "C" void* load_vulkan_with_driver_helper() {
    dlog("=== load_vulkan_with_driver_helper entered ===");

    if (!is_enabled()) return nullptr;

    if (void* c = read_cached()) {
        dlog("[entry] cached loader = {}, returning", hexp(c));
        return c;
    }

    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    if (void* c = read_cached()) {
        dlog("[entry] cached loader = {} (won race)", hexp(c));
        return c;
    }
    dlog("[entry] cache miss, proceeding");

    (void)safe_dlopen_mode();  // log it

    dlog("[entry] ===== phase 1: resolve bionic symbols =====");
    auto dl3    = resolve_dlopen_ext_3();
    auto dl4    = resolve_dlopen_ext_4();
    auto get_ns = resolve_get_exported_ns();

    if (!dl3)    fatal("resolve_dlopen_ext_3() returned nullptr");
    if (!dl4)    fatal("resolve_dlopen_ext_4() returned nullptr");
    if (!get_ns) fatal("resolve_get_exported_ns() returned nullptr "
                       "(liblinkerhook would dereference NULL)");

    dlog("[entry] symbols: dl3={} dl4={} get_ns={}",
         hexp((void*)dl3), hexp((void*)dl4), hexp((void*)get_ns));

    dlog("[entry] ===== phase 2: driver dir/name =====");
    std::string driver_dir_owned;
    const char* driver_dir = std::getenv(kDriverDirEnv);
    if (driver_dir && *driver_dir) {
        dlog("[entry] driver_dir from env = '{}'", driver_dir);
    } else {
        driver_dir_owned = self_dir();
        if (driver_dir_owned.empty())
            fatal("cannot derive driver dir");
        driver_dir = driver_dir_owned.c_str();
        dlog("[entry] driver_dir auto = '{}'", driver_dir);
    }

    const char* driver_name = resolve_driver_name();
    if (!driver_name) fatal("invalid driver name");
    dlog("[entry] driver_name = '{}'", driver_name);

    dlog("[entry] ===== phase 3: namespace =====");
    struct android_namespace_t* target_ns =
        create_driver_namespace_with_retry(
            driver_dir, "angle-driver-helper", get_ns);
    if (!target_ns) fatal("namespace creation failed (both attempts)");
    dlog("[entry] target_ns = {}", hexp((void*)target_ns));

    dlog("[entry] ===== phase 4: load liblinkerhook (RTLD_GLOBAL) =====");
    void* linkerhook = load_in_namespace(
        target_ns, kLinkerHookName, RTLD_GLOBAL | RTLD_NOW);
    if (!linkerhook) fatal("failed to load liblinkerhook.so");
    dlog("[entry] linkerhook = {}", hexp(linkerhook));

    dlog("[entry] ===== phase 5: load driver '{}' =====", driver_name);
    void* driver_handle = load_in_namespace(
        target_ns, driver_name, RTLD_LOCAL | RTLD_NOW);
    if (!driver_handle) fatal("failed to load driver");
    dlog("[entry] driver_handle = {}", hexp(driver_handle));

    dlog("[entry] ===== phase 6: probe driver =====");
    if (!probe_icd(driver_handle, driver_name)) fatal("driver probe failed");

    dlog("[entry] ===== phase 7: resolve set_handles =====");
    auto set_handles = resolve_set_handles(linkerhook);
    if (!set_handles) fatal("cannot resolve set_handles");

    dlog("[entry] calling set_handles(driver={}, dl4={}, get_ns={})",
         hexp(driver_handle), hexp((void*)dl4), hexp((void*)get_ns));
    set_handles(driver_handle, (void*)dl4, (void*)get_ns);
    dlog("[entry] set_handles returned");

    dlog("[entry] ===== phase 8: load system loader =====");
    void* loader_handle = load_loader_bypassing_hook(target_ns);
    if (!loader_handle) fatal("failed to load libvulkan.so");
    dlog("[entry] loader_handle = {}", hexp(loader_handle));
    print_loader_path(loader_handle);

    dlog("[entry] ===== phase 9: probe loader =====");
    if (!probe_loader(loader_handle, driver_name)) fatal("loader probe failed");
    if (!valid_handle(loader_handle)) fatal("loader handle invalid");

    store_cached(loader_handle);
    dlog("[entry] SUCCESS: cached loader = {}", hexp(loader_handle));
    return loader_handle;
}
