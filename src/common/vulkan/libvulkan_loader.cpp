//
// Copyright 2021 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// libvulkan_loader.cpp:
//    Helper functions for the loading Vulkan libraries.
//

#include "common/vulkan/libvulkan_loader.h"

#include "common/system_utils.h"

#include <cstdlib>
#include <dlfcn.h>

namespace angle
{
namespace vk
{
void *OpenLibVulkan()
{
    // 首先检查环境变量 VULKAN_PTR
    const char* vulkan_ptr_env = std::getenv("VULKAN_PTR");
    if (vulkan_ptr_env) {
        printf("[ANGLE] Use VULKAN_PTR = %s\n", vulkan_ptr_env);
        return (void*)std::strtoul(vulkan_ptr_env, NULL, 0x10);
    }
    
    // 尝试获取已加载的 libpojavexec.so 句柄
    void* lib_handle = dlopen("libpojavexec.so", RTLD_LAZY | RTLD_NOLOAD);
   
    // 如果库未加载，则加载它
    if (!lib_handle) {
        lib_handle = dlopen("libpojavexec.so", RTLD_LAZY);
        if (!lib_handle) {
            // 库加载失败，返回 nullptr
            return nullptr;
        }
    }
    
    // 获取 load_vulkan 函数
    void (*load_vulkan_func)() = reinterpret_cast<void(*)()>(dlsym(lib_handle, "load_vulkan"));
    if (load_vulkan_func) {
        // 调用 load_vulkan 函数
        load_vulkan_func();
    }
    
    // 再次检查环境变量 VULKAN_PTR
    vulkan_ptr_env = std::getenv("VULKAN_PTR");
    if (vulkan_ptr_env) {
        printf("[ANGLE]Use VULKAN_PTR = %s\n", vulkan_ptr_env);
        return (void*)std::strtoul(vulkan_ptr_env, NULL, 0x10);
    }

 
    printf("[ANGLE] Warning: No environment variable VULKAN_PTR! Will load libvulkan.\n");
    constexpr const char *kLibVulkanNames[] = {
#if defined(ANGLE_PLATFORM_WINDOWS)
        "vulkan-1.dll",
#elif defined(ANGLE_PLATFORM_APPLE)
        "libvulkan.dylib",
        "libvulkan.1.dylib",
        "libMoltenVK.dylib"
#else
        "libvulkan.so",
        "libvulkan.so.1",
#endif
    };

    constexpr SearchType kSearchTypes[] = {
// On Android and Fuchsia we use the system libvulkan.
#if defined(ANGLE_USE_CUSTOM_LIBVULKAN)
        SearchType::ModuleDir,
#else
        SearchType::SystemDir,
#endif  // defined(ANGLE_USE_CUSTOM_LIBVULKAN)
    };

    for (angle::SearchType searchType : kSearchTypes)
    {
        for (const char *libraryName : kLibVulkanNames)
        {
            void *library = OpenSystemLibraryWithExtension(libraryName, searchType);
            if (library)
            {
                return library;
            }
        }
    }

    printf("[ANGLE] Error: failed to load libvulkan.\n");
    return nullptr;
}
}  // namespace vk
}  // namespace angle
