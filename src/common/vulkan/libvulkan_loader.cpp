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
#include <cstring>
 
namespace angle
{
namespace vk
{
void *OpenLibVulkan()
{
    constexpr const char *kLibVulkanNames[] = {
#if defined(ANGLE_PLATFORM_WINDOWS)
        "vulkan-1.dll",
#elif defined(ANGLE_PLATFORM_APPLE)
        "libvulkan.dylib",
        "libvulkan.1.dylib",
        "libMoltenVK.dylib"
#else
        "libvulkan_freedreno.so"
        "libvulkan.so",
        "libvulkan.so.1",
#endif
    };

    const char* dri = std::getenv("LIBGL_VK_DRI");
    constexpr SearchType kSearchTypes[] = {
// On Android, Fuchsia and GGP we use the system libvulkan.
#if defined(ANGLE_USE_CUSTOM_LIBVULKAN)
        SearchType::ModuleDir,
#else
        (dri && std::strcmp(dri, "1") == 0) ? SearchType::AlreadyLoaded : SearchType::SystemDir,
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

    return nullptr;
}
}  // namespace vk
}  // namespace angle
