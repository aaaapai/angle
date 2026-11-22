//
// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WindowSurfaceVkMac.mm:
//    Implements methods from WindowSurfaceVkMac.
//

#include "libANGLE/renderer/vulkan/mac/WindowSurfaceVkMac.h"

#include <cmath>

#include <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>

#include "libANGLE/renderer/vulkan/vk_renderer.h"
#include "libANGLE/renderer/vulkan/vk_utils.h"

// Swapchain diagnostics are compile-time optional: the trace lives in the per-frame path and
// NSLog is far too slow to leave enabled in a build that renders.  Define
// ANGLE_VULKAN_SWAPCHAIN_TRACE=1 to enable it.
#ifndef ANGLE_VULKAN_SWAPCHAIN_TRACE
#    define ANGLE_VULKAN_SWAPCHAIN_TRACE 0
#endif

#if ANGLE_VULKAN_SWAPCHAIN_TRACE
#    define SWAPCHAIN_TRACE(...) NSLog(__VA_ARGS__)
#else
#    define SWAPCHAIN_TRACE(...) \
        do                       \
        {                        \
        } while (0)
#endif

namespace rx
{

WindowSurfaceVkMac::WindowSurfaceVkMac(const egl::SurfaceState &surfaceState,
                                       EGLNativeWindowType window)
    : WindowSurfaceVk(surfaceState, window),
      mLayer(nil),
      mMetalLayer(nil),
      mMetalDevice(nil)
{}

WindowSurfaceVkMac::~WindowSurfaceVkMac()
{
    if (mMetalLayer != mLayer)
    {
        // Detach the layer this surface created from the native window's layer hierarchy.
        [mMetalLayer removeFromSuperlayer];
    }
    [mMetalLayer release];
    [mMetalDevice release];
    [mLayer release];
}

angle::Result WindowSurfaceVkMac::createSurfaceVk(vk::ErrorContext *context)
{
    mLayer = [reinterpret_cast<CALayer *>(mNativeWindowType) retain];
    ANGLE_VK_CHECK(context, mLayer != nil, VK_ERROR_INITIALIZATION_FAILED);

    // A Metal layer provided by the application is rendered into directly, so that the surface
    // and the application share a single drawable, and thus a single drawable geometry.
    CAMetalLayer *nativeMetalLayer =
        [mLayer isKindOfClass:CAMetalLayer.class] ? static_cast<CAMetalLayer *>(mLayer) : nil;

    // Share the device when the application already configured one; creating a second MTLDevice
    // for the same surface only wastes memory.
    mMetalDevice = [nativeMetalLayer.device retain];
    if (mMetalDevice == nil)
    {
        mMetalDevice = MTLCreateSystemDefaultDevice();
    }
    ANGLE_VK_CHECK(context, mMetalDevice != nil, VK_ERROR_INITIALIZATION_FAILED);

    if (nativeMetalLayer != nil)
    {
        mMetalLayer = [nativeMetalLayer retain];
    }
    else
    {
        // A plain layer (for example an NSView's backing layer) needs a Metal layer to render
        // into.  It is kept in step with the native window in updateDrawableSize().
        mMetalLayer = [[CAMetalLayer alloc] init];
        ANGLE_VK_CHECK(context, mMetalLayer != nil, VK_ERROR_OUT_OF_HOST_MEMORY);
        mMetalLayer.frame = CGRectMake(0, 0, mLayer.bounds.size.width, mLayer.bounds.size.height);
        [mLayer addSublayer:mMetalLayer];
    }

    mMetalLayer.device          = mMetalDevice;
    mMetalLayer.framebufferOnly = NO;  // Required for blits and readbacks.
#if TARGET_OS_OSX
    // Autoresizing masks are only available on macOS.  On iOS the layer's frame is managed by
    // the view hierarchy.
    if (!usesNativeLayer())
    {
        mMetalLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    }
#endif

    const CGSize drawableSize = updateDrawableSize();
    ANGLE_VK_CHECK(context, drawableSize.width >= 1.0 && drawableSize.height >= 1.0,
                   VK_ERROR_INITIALIZATION_FAILED);

    VkMetalSurfaceCreateInfoEXT createInfo = {};
    createInfo.sType                       = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    createInfo.flags                       = 0;
    createInfo.pNext                       = nullptr;
    createInfo.pLayer                      = mMetalLayer;
    ANGLE_VK_TRY(context, VK_CALL(vkCreateMetalSurfaceEXT, context->getRenderer()->getInstance(),
                                  &createInfo, nullptr, &mSurface));

    SWAPCHAIN_TRACE(@"[swapchain] surface %p: %dx%d, native layer %s", (void *)mSurface,
                    static_cast<int>(drawableSize.width), static_cast<int>(drawableSize.height),
                    usesNativeLayer() ? "yes" : "no");

    return angle::Result::Continue;
}

CGSize WindowSurfaceVkMac::updateDrawableSize() const
{
    if (!usesNativeLayer())
    {
        // The Metal layer belongs to this surface, so mirror the native window's geometry.
        // Only macOS propagates it through autoresizing masks.
#if !TARGET_OS_OSX
        if (!CGSizeEqualToSize(mMetalLayer.bounds.size, mLayer.bounds.size))
        {
            mMetalLayer.frame =
                CGRectMake(0, 0, mLayer.bounds.size.width, mLayer.bounds.size.height);
        }
#endif
        if (mMetalLayer.contentsScale != mLayer.contentsScale)
        {
            mMetalLayer.contentsScale = mLayer.contentsScale;
        }
    }

    // Mirror the Metal backend (WindowSurfaceMtl::calcExpectedDrawableSize): the layer's point
    // geometry multiplied by its contents scale is the single source of truth.  In particular,
    // do not use the CAMetalLayer's drawableSize, which an application may have set to a slightly
    // different value, and do not use MoltenVK's natural extent: it rounds half-to-even
    // (375 pt * 1.5 = 562.5 -> 562), one pixel short of the drawable that Core Animation and the
    // application render into (563).  A one pixel short swapchain makes the GL viewport fall
    // outside of the presented image and can fault the GPU on present.
    CGSize size = CGSizeMake(mMetalLayer.bounds.size.width * mMetalLayer.contentsScale,
                             mMetalLayer.bounds.size.height * mMetalLayer.contentsScale);

    // Drawables are allocated in whole pixels.  Round up so the swapchain is never smaller than
    // the geometry the application renders into, and store the result on the layer so the layer
    // and the swapchain can never disagree.
    size = CGSizeMake(std::ceil(size.width), std::ceil(size.height));
    if (!CGSizeEqualToSize(mMetalLayer.drawableSize, size))
    {
        SWAPCHAIN_TRACE(@"[swapchain] drawable size %gx%g -> %gx%g", mMetalLayer.drawableSize.width,
                        mMetalLayer.drawableSize.height, size.width, size.height);
        mMetalLayer.drawableSize = size;
    }

    return size;
}

angle::Result WindowSurfaceVkMac::getCurrentWindowSize(vk::ErrorContext *context,
                                                       gl::Extents *extentsOut) const
{
    ANGLE_VK_CHECK(context, mMetalLayer != nil, VK_ERROR_INITIALIZATION_FAILED);

    const CGSize drawableSize = updateDrawableSize();
    ANGLE_VK_CHECK(context, drawableSize.width >= 1.0 && drawableSize.height >= 1.0,
                   VK_ERROR_INITIALIZATION_FAILED);

    *extentsOut =
        gl::Extents(static_cast<int>(drawableSize.width), static_cast<int>(drawableSize.height), 1);

    SWAPCHAIN_TRACE(@"[swapchain] extent %dx%d, scale %g", extentsOut->width, extentsOut->height,
                    mMetalLayer.contentsScale);

    return angle::Result::Continue;
}

}  // namespace rx

#undef SWAPCHAIN_TRACE
