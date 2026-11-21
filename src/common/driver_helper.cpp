//
// driver_helper.cpp
//
// Vulkan driver loader for ANGLE's libvulkan_loader.
//
// ==========================================================================
// Design constraint and consequences:
//
//   liblinkerhook.so is UNMODIFIABLE.  It overrides the weak
//   android_load_sphal_library / android_dlopen_ext from libdl.so with
//   strong definitions, and those definitions must be visible to
//   libvulkan.so so that the loader's UND references resolve to the hook.
//   On bionic this requires the hook to be RTLD_GLOBAL in the target
//   namespace; RTLD_LOCAL would leave libvulkan.so bound to the system
//   stubs.
//
//   RTLD_GLOBAL therefore cannot be eliminated under this constraint.
//   What CAN be done - and is done here - is to make the hook's non-vulkan
//   forwarding path actually WORK, so that any incidental
//   android_load_sphal_library call from another library does not fail:
//
//     - target_ns is created with the system "default"/"sphal" namespace
//       as its parent, so the hook's get_exported_namespace("sphal"/
//       "vendor"/"default") lookups return namespaces that are valid in
//       the target_ns context;
//     - hook dependencies (liblog, libbase, etc.) are linked into the
//       target namespace explicitly.
//
//   This does not remove global symbol visibility, but removes the most
//   likely failure mode caused by it.
//
// Other hard requirements satisfied:
//   - __loader_android_create_namespace (7-arg, caller_addr) is used
//     whenever available.  Without caller_addr the new namespace does not
//     inherit the caller's global group correctly, which breaks
//     link_namespaces and hook loading on many devices.
//   - get_exported_namespace returning NULL is FATAL.  liblinkerhook
//     dereferences it unconditionally on every non-"vulkan."
//     android_load_sphal_library call; installing the hook without it
//     converts a load failure into a SIGSEGV.
//
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
constexpr const char* kDriverNameEnv     = "ANGLE_LIBVULKAN_NAME";
constexpr const char* kDriverDirEnv      = "ANGLE_VULKAN_DRIVER_DIR";
constexpr const char* kDefaultDriverName = "libvulkan_freedreno.so";
constexpr const char* kLinkerHookName    = "liblinkerhook.so";

#if defined(__LP64__)
constexpr const char* kSystemLoaderPath = "/system/lib64/libvulkan.so";
constexpr const char* kDefaultLibPath   = "/system/lib64:/vendor/lib64:/system_ext/lib64";
#else
constexpr const char* kSystemLoaderPath = "/system/lib/libvulkan.so";
constexpr const char* kDefaultLibPath   = "/system/lib:/vendor/lib:/system_ext/lib";
#endif

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
// Handles
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

void* ld_android_handle() {
    static void* h = nullptr;
    static bool tried = false;
    if (!tried) { tried = true; h = dlopen("ld-android.so", RTLD_LOCAL | RTLD_LAZY); }
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
    if (!fn) if (auto h = ld_android_handle())
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

    if (auto h = libdl_android_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            dlsym(h, "__loader_android_get_exported_namespace"));
        if (fn) {
            dlog("resolved __loader_android_get_exported_namespace = {} "
                 "(libdl_android.so)", hexp((void*)fn));
            return fn;
        }
    }
    if (auto h = ld_android_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            dlsym(h, "__loader_android_get_exported_namespace"));
        if (fn) {
            dlog("resolved __loader_android_get_exported_namespace = {} "
                 "(ld-android.so)", hexp((void*)fn));
            return fn;
        }
    }
    if (void* h = dlopen("libc.so", RTLD_LOCAL | RTLD_LAZY)) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            dlsym(h, "__loader_android_get_exported_namespace"));
        if (fn) {
            dlog("resolved __loader_android_get_exported_namespace = {} "
                 "(libc.so)", hexp((void*)fn));
            return fn;
        }
    }
    if (auto h = libdl_android_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            dlsym(h, "android_get_exported_namespace"));
        if (fn) {
            dlog("resolved android_get_exported_namespace = {} "
                 "(libdl_android.so, no prefix)", hexp((void*)fn));
            return fn;
        }
    }
    if (auto h = libdl_handle()) {
        fn = reinterpret_cast<get_exported_ns_fn>(
            dlsym(h, "android_get_exported_namespace"));
        if (fn) {
            dlog("resolved android_get_exported_namespace = {} "
                 "(libdl.so, no prefix)", hexp((void*)fn));
            return fn;
        }
    }

    dlog("FAILED to resolve android_get_exported_namespace");
    return nullptr;
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

// noinline: make __builtin_return_address(0) unambiguously refer to
// create_driver_namespace() in libGLESv2_angle.so.
__attribute__((noinline))
static struct android_namespace_t* call_create_ns_with_caller(
    create_ns_7arg_fn create,
    const char* name, const char* ld, const char* def,
    uint64_t type, const char* permitted,
    struct android_namespace_t* parent) {
    return create(name, ld, def, type, permitted, parent,
                  __builtin_return_address(0));
}

struct android_namespace_t* create_driver_namespace(
        const char* driver_dir, const char* name,
        get_exported_ns_fn get_ns) {
    if (android_get_device_api_level() < 26) {
        dlog("cannot create namespace: API < 26 (api_level={})",
             android_get_device_api_level());
        return nullptr;
    }

    create_ns_7arg_fn create7 = nullptr;
    create_ns_fn      create6 = nullptr;
    link_ns_fn        link    = nullptr;

    if (auto h = libdl_android_handle()) {
        create7 = reinterpret_cast<create_ns_7arg_fn>(
            dlsym(h, "__loader_android_create_namespace"));
        create6 = reinterpret_cast<create_ns_fn>(
            dlsym(h, "android_create_namespace"));
        link    = reinterpret_cast<link_ns_fn>(
            dlsym(h, "__loader_android_link_namespaces"));
        if (!link) link = reinterpret_cast<link_ns_fn>(
            dlsym(h, "android_link_namespaces"));
    }
    if (!create7 && !create6) {
        if (auto h = ld_android_handle()) {
            create7 = reinterpret_cast<create_ns_7arg_fn>(
                dlsym(h, "__loader_android_create_namespace"));
            create6 = reinterpret_cast<create_ns_fn>(
                dlsym(h, "android_create_namespace"));
        }
    }
    if (!link) {
        if (auto h = ld_android_handle())
            link = reinterpret_cast<link_ns_fn>(
                dlsym(h, "__loader_android_link_namespaces"));
    }
    if (!link) {
        if (auto h = libdl_handle())
            link = reinterpret_cast<link_ns_fn>(
                dlsym(h, "android_link_namespaces"));
    }

    if (!create7 && !create6) {
        dlog("neither __loader_android_create_namespace nor "
             "android_create_namespace is available");
        return nullptr;
    }
    if (!link) {
        dlog("android_link_namespaces not exported anywhere");
        return nullptr;
    }

    std::string full_path = std::format("{}:{}", driver_dir, kDefaultLibPath);
    const char* permitted = "/system/:/system_ext/:/data/:/vendor/:/apex/";

    // Try "sphal" first (the namespace liblinkerhook itself probes), then
    // "default", then nullptr.  Inheriting from one of these makes the
    // hook's get_exported_namespace() results usable inside target_ns,
    // which is what keeps its non-vulkan forwarding path alive.
    struct android_namespace_t* parent = nullptr;
    const char* parent_name = nullptr;
    if (get_ns) {
        for (const char* candidate : {"sphal", "default"}) {
            parent = get_ns(candidate);
            if (parent) { parent_name = candidate; break; }
        }
    }

    dlog("creating namespace '{}' (ld='{}', parent={})",
         name, full_path, parent_name ? parent_name : "null");

    struct android_namespace_t* ns = nullptr;
    if (create7) {
        ns = call_create_ns_with_caller(
            create7, name, full_path.c_str(), full_path.c_str(), 3,
            permitted, parent);
    } else {
        dlog("WARN: using 6-arg android_create_namespace without "
             "caller_addr; namespace inheritance may be incomplete");
        ns = create6(name, full_path.c_str(), full_path.c_str(), 3,
                     permitted, parent);
    }
    if (!ns) {
        dlog("namespace creation returned NULL for '{}'", name);
        return nullptr;
    }

    auto link_result = [&](const char* soname) {
        (void)dlerror();
        void* r = link(ns, nullptr, soname);
        auto e = last_dlerror();
        dlog("link_namespaces(ns, NULL, '{}') -> {} (err={})",
             soname, hexp(r), e);
    };

    // Core bionic pieces.
    link_result("ld-android.so");
    link_result("libnativeloader.so");
    link_result("libnativeloader_lazy.so");

    // liblinkerhook's own dependencies and common libvulkan deps.  These
    // are linked so that the hook and the loader do not fail on trivial
    // missing symbols inside the isolated namespace.
    for (const char* soname : {
             "liblog.so", "libbase.so", "libcutils.so",
             "libhardware.so", "libsync.so", "libui.so",
             "libnativewindow.so", "libandroid.so" }) {
        link_result(soname);
    }

    dlog("namespace '{}' created = {}", name, hexp((void*)ns));
    return ns;
}

struct android_namespace_t* create_driver_namespace_with_retry(
        const char* driver_dir, const char* base_name,
        get_exported_ns_fn get_ns) {
    std::string unique_base = std::format("{}-{}", base_name,
                                          static_cast<long long>(getpid()));
    if (auto* ns = create_driver_namespace(driver_dir, unique_base.c_str(), get_ns))
        return ns;

    static std::atomic<uint32_t> s_seq{0};
    std::string unique = std::format("{}-{}", unique_base,
                                     s_seq.fetch_add(1, std::memory_order_relaxed));
    dlog("namespace '{}' unavailable, retrying as '{}' "
         "(WARN: this instance will get its own driver copy; "
         "turnip may be initialized more than once)", unique_base, unique);
    return create_driver_namespace(driver_dir, unique.c_str(), get_ns);
}

void* load_in_namespace(struct android_namespace_t* ns, const char* name, int flags) {
    auto dl3 = resolve_dlopen_ext_3();
    if (!ns || !dl3) {
        dlog("load_in_namespace('{}'): ns or dl3 missing; falling back to dlopen",
             name);
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
// Loader load
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
    dlog("loading real loader: alias='{}', fd={}", kLoaderAlias, fd);
    void* h = dl3(kLoaderAlias, RTLD_LOCAL | RTLD_NOW, &info);
    auto err = last_dlerror();
    close(fd);
    if (!h) {
        dlog("load '{}' via fd failed: {}", kLoaderAlias, err);
        return nullptr;
    }
    dlog("loader loaded as '{}' -> {}", kLoaderAlias, hexp(h));

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
             "suspicious but tolerated", hexp(h));
    }
    return h;
}

// ---------------------------------------------------------------------------
// Probes
// ---------------------------------------------------------------------------
bool probe_icd(void* driver_handle, std::string_view driver_name) {
    if (!valid_handle(driver_handle)) {
        dlog("probe_icd: invalid handle for {}", driver_name);
        return false;
    }
    void* sym = dlsym(driver_handle, "vk_icdGetInstanceProcAddr");
    if (!sym) sym = dlsym(driver_handle, "vkGetInstanceProcAddr");
    if (!sym) {
        dlog("probe_icd({}): no vk_icd/vkGetInstanceProcAddr", driver_name);
        return false;
    }
    auto gipa = reinterpret_cast<gipa_t>(sym);

    if (!gipa(nullptr, "vkCreateInstance")) {
        dlog("probe_icd({}): WARN vkCreateInstance == NULL (tolerated)",
             driver_name);
    }

    auto enum_ext = reinterpret_cast<enum_ext_t>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!enum_ext) {
        dlog("probe_icd({}): cannot resolve vkEnumExt", driver_name);
        return false;
    }
    uint32_t n = 0;
    int32_t r = enum_ext(nullptr, &n, nullptr);
    dlog("probe_icd({}): VkResult={} count={}", driver_name, r, n);
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
    dlog("probe_loader: VkResult={} count={}", r, n);
    if (r != 0 || n == 0) return false;

    int maps = count_driver_exec_maps(driver_name);
    if (maps < 0) {
        dlog("probe_loader: OK ({} ext), maps unreadable", n);
        return true;
    }
    if (maps == 0) {
        dlog("probe_loader: NOTE {} ext, 0 r-xp of {} "
             "(normal for memfd, suspicious otherwise)", n, driver_name);
        return true;
    }
    dlog("probe_loader: OK ({} ext, {} r-xp of {})", n, maps, driver_name);
    return true;
}

void print_loader_path(void* loader_handle) {
    void* sym = dlsym(loader_handle, "vkGetInstanceProcAddr");
    if (!sym) return;
    Dl_info info{};
    if (dladdr(sym, &info) && info.dli_fname && *info.dli_fname) {
        dlog("loader real path = '{}'", info.dli_fname);
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
        dlog("cached loader = {}", hexp(c));
        return c;
    }

    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);

    if (void* c = read_cached()) {
        dlog("cached loader = {} (another thread won)", hexp(c));
        return c;
    }

    auto dl3    = resolve_dlopen_ext_3();
    auto dl4    = resolve_dlopen_ext_4();
    auto get_ns = resolve_get_exported_ns();

    if (!dl3 || !dl4) {
        dlog("aborting: required bionic symbols missing (dl3={}, dl4={})",
             hexp((void*)dl3), hexp((void*)dl4));
        return nullptr;
    }

    // HARD REQUIREMENT.  liblinkerhook calls get_exported_namespace_impl()
    // unconditionally on every non-"vulkan." android_load_sphal_library
    // path.  Installing it with a NULL impl turns a load failure into a
    // SIGSEGV inside any library that happens to trigger that path.
    if (!get_ns) {
        dlog("aborting: get_exported_namespace unavailable; installing "
             "liblinkerhook without it would make its non-vulkan forwarding "
             "path dereference NULL");
        return nullptr;
    }

    std::string driver_dir_owned;
    const char* driver_dir = std::getenv(kDriverDirEnv);
    if (driver_dir && *driver_dir) {
        dlog("driver dir = '{}' (env)", driver_dir);
    } else {
        driver_dir_owned = self_dir();
        if (driver_dir_owned.empty()) {
            dlog("aborting: cannot derive driver dir");
            return nullptr;
        }
        driver_dir = driver_dir_owned.c_str();
        dlog("driver dir = '{}' (auto)", driver_dir);
    }

    const char* driver_name = resolve_driver_name();
    if (!driver_name) return nullptr;

    struct android_namespace_t* target_ns =
        create_driver_namespace_with_retry(
            driver_dir, "angle-driver-helper", get_ns);
    if (!target_ns) {
        dlog("aborting: namespace creation failed");
        return nullptr;
    }

    // RTLD_GLOBAL is retained here deliberately: liblinkerhook's strong
    // android_load_sphal_library / android_dlopen_ext must be visible to
    // libvulkan.so.  The mitigations against its side effects live in
    // create_driver_namespace (parent namespace + explicit dep linking).
    dlog("loading {} (RTLD_GLOBAL | RTLD_NOW, required by hook ABI)",
         kLinkerHookName);
    void* linkerhook = load_in_namespace(
        target_ns, kLinkerHookName, RTLD_GLOBAL | RTLD_NOW);
    if (!linkerhook) {
        dlog("aborting: failed to load {}", kLinkerHookName);
        return nullptr;
    }

    dlog("loading driver '{}' (RTLD_LOCAL | RTLD_NOW)", driver_name);
    void* driver_handle = load_in_namespace(
        target_ns, driver_name, RTLD_LOCAL | RTLD_NOW);
    if (!driver_handle) {
        dlog("aborting: failed to load driver '{}'", driver_name);
        return nullptr;
    }

    if (!probe_icd(driver_handle, driver_name)) {
        dlog("aborting: driver probe failed");
        return nullptr;
    }

    auto set_handles = resolve_set_handles(linkerhook);
    if (!set_handles) {
        dlog("aborting: cannot resolve set_handles");
        return nullptr;
    }
    set_handles(driver_handle,
                reinterpret_cast<void*>(dl4),
                reinterpret_cast<void*>(get_ns));
    dlog("set_handles(driver={}, dl4={}, get_ns={})",
         hexp(driver_handle), hexp((void*)dl4), hexp((void*)get_ns));

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

    store_cached(loader_handle);
    dlog("SUCCESS: cached loader = {}", hexp(loader_handle));
    return loader_handle;
}
