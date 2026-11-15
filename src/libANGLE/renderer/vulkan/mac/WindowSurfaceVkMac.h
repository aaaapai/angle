//
// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WindowSurfaceVkMac.h:
//    Subclasses WindowSurfaceVk for the Mac platform.
//

#ifndef LIBANGLE_RENDERER_VULKAN_MAC_WINDOWSURFACEVKMAC_H_
#define LIBANGLE_RENDERER_VULKAN_MAC_WINDOWSURFACEVKMAC_H_

#include "libANGLE/renderer/vulkan/SurfaceVk.h"

#if TARGET_OS_OSX
#    include <Cocoa/Cocoa.h>
#else
#    include <QuartzCore/CAMetalLayer.h>
#    include <UIKit/UIKit.h>
#endif

namespace rx
{

class WindowSurfaceVkMac : public WindowSurfaceVk
{
  public:
    WindowSurfaceVkMac(const egl::SurfaceState &surfaceState, EGLNativeWindowType window);
    ~WindowSurfaceVkMac() override;

  private:
    angle::Result createSurfaceVk(vk::ErrorContext *context) override;
    angle::Result getCurrentWindowSize(vk::ErrorContext *context,
                                       gl::Extents *extentsOut) const override;

    // Returns the size of the layer's drawable in whole pixels, updating the layer if its
    // geometry or drawable size was out of date.
    CGSize updateDrawableSize() const;

    // The EGL native window, retained for the lifetime of the surface.
    CALayer *mLayer;
    // Either |mLayer| when it already is a CAMetalLayer, or a Metal layer owned by this surface.
    CAMetalLayer *mMetalLayer;
    id<MTLDevice> mMetalDevice;
};

}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_MAC_WINDOWSURFACEVKMAC_H_
