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
      mMetalDevice(nil),
      mAppProvidedDrawableSize(false)
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
        // Capture this before updateDrawableSize() writes the size, so that a size the application
        // configured is never mistaken for one this surface derived.
        mAppProvidedDrawableSize =
            mMetalLayer.drawableSize.width >= 1.0 && mMetalLayer.drawableSize.height >= 1.0;
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

    SWAPCHAIN_TRACE(@"[swapchain] surface %p: %dx%d, native layer %s, app drawable %s",
                    (void *)mSurface, static_cast<int>(drawableSize.width),
                    static_cast<int>(drawableSize.height), usesNativeLayer() ? "yes" : "no",
                    mAppProvidedDrawableSize ? "yes" : "no");

    return angle::Result::Continue;
}

CGSize WindowSurfaceVkMac::updateDrawableSize() const
{
    CGSize size;

    if (usesNativeLayer() && mAppProvidedDrawableSize)
    {
        // The application owns the drawable geometry: it may render at a resolution below the
        // layer's native one, so the configured size must not be overridden.
        size = mMetalLayer.drawableSize;
    }
    else
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

        size = CGSizeMake(mMetalLayer.bounds.size.width * mMetalLayer.contentsScale,
                          mMetalLayer.bounds.size.height * mMetalLayer.contentsScale);
    }

    // Drawables are allocated in whole pixels.  Scaling the layer's point geometry can produce a
    // fractional pixel count (375 pt at 1.5x is 562.5), and if the layer and the swapchain round
    // it differently the surface ends up one pixel smaller than the window, leaving the GL
    // viewport outside of the surface.  Round up and store the result so that both sides use the
    // same value.
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
