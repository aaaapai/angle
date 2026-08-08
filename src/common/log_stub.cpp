#include <cstdio>
#include <cstdarg>

extern "C" {

int __android_log_write(int prio, const char* tag, const char* text)
{
    // 输出格式：[优先级] 标签: 内容
    return printf("[%d] %s: %s\n", prio, tag, text);
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    
    // 先输出前缀
    printf("[%d] %s: ", prio, tag);
    // 再输出格式化内容
    int ret = vprintf(fmt, args);
    // 追加换行
    printf("\n");
    
    va_end(args);
    // 返回 vprintf 写入的字符数（不包括前缀和换行），可依需求调整
    return ret;
}

}
