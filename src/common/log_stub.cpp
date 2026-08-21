#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>
#include <string_view>
#include <array>
#include <algorithm>

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

int __android_log_print(int prio, const char* tag, const char* fmt, ...) noexcept {
    if (!fmt) {
        safe_write(prio, tag, "(null format)");
        return -1;
    }
    
    va_list args;
    va_start(args, fmt);
    
    va_list args_copy;
    va_copy(args_copy, args);
    
    int len = vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);
    
    if (len < 0) {
        safe_write(prio, tag, "(format error)");
        va_end(args);
        return -1;
    }
    
    std::string msg;
    msg.resize(static_cast<size_t>(len) + 1);
    int written = vsnprintf(msg.data(), msg.size(), fmt, args);
    va_end(args);
    
    if (written < 0) {
        safe_write(prio, tag, "(format error after allocation)");
        return -1;
    }
    
    msg.resize(static_cast<size_t>(written));
    
    safe_write(prio, tag, msg.c_str());
    return written;
}

} // extern "C"
