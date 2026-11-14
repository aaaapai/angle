#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>
#include <string_view>
#include <array>
#include <algorithm>
#include <cstring>

extern "C" {

static std::mutex g_log_mutex;

static void safe_write(int prio, const char* tag, const char* msg) noexcept {
    static constexpr std::string_view null_tag = "(null-tag)";
    static constexpr std::string_view null_msg = "(null-msg)";
    
    std::string_view tag_sv = tag ? tag : null_tag;
    std::string_view msg_sv = msg ? msg : null_msg;
    
    std::lock_guard lock(g_log_mutex);
    
    const char prefix[] = "[";
    const char suffix[] = "] ";
    const char separator[] = ": ";
    const char newline[] = "\n";
    
    size_t total_len = std::size(prefix) - 1 + 
                       std::to_string(prio).length() +
                       std::size(suffix) - 1 +
                       tag_sv.length() +
                       std::size(separator) - 1 +
                       msg_sv.length() +
                       std::size(newline) - 1;
    
    std::string buffer;
    buffer.reserve(total_len);
    buffer.append(prefix);
    buffer.append(std::to_string(prio));
    buffer.append(suffix);
    buffer.append(tag_sv);
    buffer.append(separator);
    buffer.append(msg_sv);
    buffer.append(newline);
    
    fwrite(buffer.data(), 1, buffer.size(), stdout);
    fflush(stdout);
}

int __android_log_write(int prio, const char* tag, const char* text) noexcept {
    safe_write(prio, tag, text);
    return 0;
}

// ---------------------------------------------------------------------------
// __android_log_print is a printf-style variadic API: the format string and
// va_list have to stay.  clang's -Wunsafe-buffer-usage plugin cannot reason
// about va_list boundaries, so it flags every printf-family call in this
// function even though we bound the destination buffer explicitly with
// std::vsnprintf.  The suppressed diagnostics are false positives; the
// alternative would be to break the __android_log_print ABI, which callers
// rely on.
// ---------------------------------------------------------------------------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-format-attr-call"

int __android_log_print(int prio, const char* tag, const char* fmt, ...) noexcept {
    if (!fmt) {
        safe_write(prio, tag, "(null format)");
        return -1;
    }
    
    va_list args;
    va_start(args, fmt);
    
    // First pass: measure required length with a bounded (null) destination.
    va_list args_len;
    va_copy(args_len, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, args_len);
    va_end(args_len);
    
    std::string formatted;
    if (needed > 0) {
        // std::string guarantees a trailing '\0' at data()[size()], so
        // passing (size() + 1) as the bound is exact and safe.
        formatted.resize(static_cast<size_t>(needed));
        std::vsnprintf(formatted.data(), formatted.size() + 1, fmt, args);
    }
    va_end(args);
    
    if (needed > 0) {
        safe_write(prio, tag, formatted.c_str());
    } else {
        safe_write(prio, tag, "(format error)");
    }
    return 0;
}

#pragma clang diagnostic pop

} // extern "C"
