//
// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WindowSurfaceVkMac.mm:
//    Implements methods from WindowSurfaceVkMac.
//

#include "libANGLE/renderer/vulkan/mac/WindowSurfaceVkMac.h"

#include <algorithm>
#include <cmath>

#include <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>

#include "libANGLE/renderer/vulkan/vk_renderer.h"
#include "libANGLE/renderer/vulkan/vk_utils.h"

namespace rx
{

namespace
{

// 比较两个 CGRect 是否近似相等。UIKit 的 frame 带浮点误差，用 == 会误判"变了"。
bool RectApproxEqual(CGRect a, CGRect b, CGFloat eps)
{
    return std::fabs(static_cast<double>(a.origin.x    - b.origin.x))    <= eps &&
           std::fabs(static_cast<double>(a.origin.y    - b.origin.y))    <= eps &&
           std::fabs(static_cast<double>(a.size.width  - b.size.width))  <= eps &&
           std::fabs(static_cast<double>(a.size.height - b.size.height)) <= eps;
}

// 计算 swapchain surface 尺寸。**用 ceil 而不是 floor/round**。
//
// 背景：bounds × scale 在 1.5x iPad 上会得到半整数（例如 375 × 1.5 = 562.5）。
// 不同代码路径取整不一致：
//   - CoreAnimation 内部：floor(562.5) = 562
//   - GLFW / 游戏：round/ceil(562.5) = 563
// 若 ANGLE 的 surface 是 562 而 viewport 是 563，Metal 光栅化会写到 swapchain
// image 之外的内存，触发 GPU Address Fault (kIOGPUCommandBufferCallbackErrorPageFault)，
// 最终导致 device lost 和 Minecraft 的 "Failed to complete GPU fence: 0" 崩溃。
//
// 用 ceil 让 surface ≥ 任何取整结果，viewport 再也不会越界。
// 代价：1.5x iPad 上 surface 多 1px，可能 1px 黑边，可接受。
//
// iOS 12 device（iPhone 5S ~ XS）的 contentsScale 是 2.0/3.0 整数倍，
// ceil == floor == round，完全无害。
static CGSize ComputeSurfaceSize(CGRect bounds, CGFloat scale)
{
    const CGFloat w = std::ceil(bounds.size.width  * scale);
    const CGFloat h = std::ceil(bounds.size.height * scale);
    return CGSizeMake(std::max<CGFloat>(w, 1.0),
                      std::max<CGFloat>(h, 1.0));
}

// 只在状态变化时调，避免每次 swap 刷屏。
void MetalLogLayerState(const char *tag, CALayer *parent, CAMetalLayer *metal)
{
    const CGRect  pb = parent ? parent.bounds        : CGRectZero;
    const CGFloat ps = parent ? parent.contentsScale : 0.0;
    const CGRect  mf = metal  ? metal.frame          : CGRectZero;
    const CGSize  md = metal  ? metal.drawableSize   : CGSizeZero;
    const CGFloat ms = metal  ? metal.contentsScale  : 0.0;

    const double rawW   = static_cast<double>(pb.size.width)  * static_cast<double>(ps);
    const double rawH   = static_cast<double>(pb.size.height) * static_cast<double>(ps);
    const int    floorW = static_cast<int>(std::floor(rawW));
    const int    floorH = static_cast<int>(std::floor(rawH));
    const int    ceilW  = static_cast<int>(std::ceil(rawW));
    const int    ceilH  = static_cast<int>(std::ceil(rawH));

    NSLog(@"[GLGeo][Metal] %s "
          @"parent.bounds=(%.2f,%.2f,%.2f,%.2f) parent.scale=%.2f "
          @"raw=(%.3f,%.3f) floor=(%d,%d) ceil=(%d,%d) "
          @"metal.frame=(%.2f,%.2f,%.2f,%.2f) metal.scale=%.2f "
          @"metal.drawableSize=(%.3f,%.3f) int=(%d,%d)",
          tag,
          static_cast<double>(pb.origin.x), static_cast<double>(pb.origin.y),
          static_cast<double>(pb.size.width), static_cast<double>(pb.size.height),
          static_cast<double>(ps),
          rawW, rawH, floorW, floorH, ceilW, ceilH,
          static_cast<double>(mf.origin.x), static_cast<double>(mf.origin.y),
          static_cast<double>(mf.size.width), static_cast<double>(mf.size.height),
          static_cast<double>(ms),
          static_cast<double>(md.width), static_cast<double>(md.height),
          static_cast<int>(md.width), static_cast<int>(md.height));
}

}  // namespace

WindowSurfaceVkMac::WindowSurfaceVkMac(const egl::SurfaceState &surfaceState,
                                       EGLNativeWindowType window)
    : WindowSurfaceVk(surfaceState, window), mMetalLayer(nullptr)
{
    NSLog(@"[GLGeo][Metal] WindowSurfaceVkMac ctor native=%p", window);
}

WindowSurfaceVkMac::~WindowSurfaceVkMac()
{
    NSLog(@"[GLGeo][Metal] WindowSurfaceVkMac dtor");
    [mMetalDevice release];
    [mMetalLayer release];
}

angle::Result WindowSurfaceVkMac::createSurfaceVk(vk::ErrorContext *context)
{
    NSLog(@"[GLGeo][Metal] createSurfaceVk enter native=%p", mNativeWindowType);

    mMetalDevice = MTLCreateSystemDefaultDevice();
    if (mMetalDevice == nil)
    {
        NSLog(@"[GLGeo][Metal] FATAL: MTLCreateSystemDefaultDevice returned nil");
        ANGLE_VK_TRY(context, VK_ERROR_INITIALIZATION_FAILED);
    }

    if (__builtin_available(iOS 12.0, macOS 10.14, *))
    {
        NSLog(@"[GLGeo][Metal] Metal device name=%@", [mMetalDevice name]);
    }

    CALayer *layer = reinterpret_cast<CALayer *>(mNativeWindowType);
    if (layer == nil)
    {
        NSLog(@"[GLGeo][Metal] FATAL: native window layer is nil");
        ANGLE_VK_TRY(context, VK_ERROR_INITIALIZATION_FAILED);
    }

    if (__builtin_available(iOS 12.0, macOS 10.14, *))
    {
        NSLog(@"[GLGeo][Metal] parent CALayer=%p class=%@ "
              @"bounds=(%.3f,%.3f,%.3f,%.3f) contentsScale=%.3f",
              layer, NSStringFromClass([layer class]),
              static_cast<double>(layer.bounds.origin.x),
              static_cast<double>(layer.bounds.origin.y),
              static_cast<double>(layer.bounds.size.width),
              static_cast<double>(layer.bounds.size.height),
              static_cast<double>(layer.contentsScale));
    }

    mMetalLayer = [[CAMetalLayer alloc] init];
    if (mMetalLayer == nil)
    {
        NSLog(@"[GLGeo][Metal] FATAL: failed to allocate CAMetalLayer");
        ANGLE_VK_TRY(context, VK_ERROR_OUT_OF_HOST_MEMORY);
    }

    mMetalLayer.device          = mMetalDevice;
    mMetalLayer.framebufferOnly = NO;
    mMetalLayer.contentsScale   = layer.contentsScale;
    mMetalLayer.frame = CGRectMake(0, 0,
                                   layer.bounds.size.width,
                                   layer.bounds.size.height);

#if TARGET_OS_OSX
    // autoresizingMask 只在 macOS 上有效。iOS 上 mMetalLayer 不是 UIView 的 backing
    // layer，父 layer 的 bounds 变化不会自动传播，必须在 getCurrentWindowSize 里手动同步。
    mMetalLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
#endif

    // 关键修复 #1：用 ceil 计算 drawableSize。
    // 绝不要用 layer.bounds.size * scale 直接赋值 —— 半整数会引发 1px 越界 PageFault。
    const CGSize surfaceSize = ComputeSurfaceSize(layer.bounds, layer.contentsScale);
    mMetalLayer.drawableSize = surfaceSize;

    if (__builtin_available(iOS 12.0, macOS 10.14, *))
    {
        NSLog(@"[GLGeo][Metal] mMetalLayer created: "
              @"frame=(%.2f,%.2f,%.2f,%.2f) contentsScale=%.3f "
              @"drawableSize=(%.3f,%.3f) floor_raw=(%.3f,%.3f) ceil_raw=(%.3f,%.3f)",
              static_cast<double>(mMetalLayer.frame.origin.x),
              static_cast<double>(mMetalLayer.frame.origin.y),
              static_cast<double>(mMetalLayer.frame.size.width),
              static_cast<double>(mMetalLayer.frame.size.height),
              static_cast<double>(mMetalLayer.contentsScale),
              static_cast<double>(surfaceSize.width),
              static_cast<double>(surfaceSize.height),
              static_cast<double>(std::floor(layer.bounds.size.width  * layer.contentsScale)),
              static_cast<double>(std::floor(layer.bounds.size.height * layer.contentsScale)),
              static_cast<double>(std::ceil(layer.bounds.size.width  * layer.contentsScale)),
              static_cast<double>(std::ceil(layer.bounds.size.height * layer.contentsScale)));
    }

    [layer addSublayer:mMetalLayer];
    MetalLogLayerState("after addSublayer", layer, mMetalLayer);

    VkMetalSurfaceCreateInfoEXT createInfo = {};
    createInfo.sType                       = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    createInfo.flags                       = 0;
    createInfo.pNext                       = nullptr;
    createInfo.pLayer                      = mMetalLayer;
    ANGLE_VK_TRY(context, VK_CALL(vkCreateMetalSurfaceEXT,
                                  context->getRenderer()->getInstance(),
                                  &createInfo, nullptr, &mSurface));

    NSLog(@"[GLGeo][Metal] vkCreateMetalSurfaceEXT OK surface=%p", mSurface);
    return angle::Result::Continue;
}

angle::Result WindowSurfaceVkMac::getCurrentWindowSize(vk::ErrorContext *context,
                                                       gl::Extents *extentsOut) const
{
    ANGLE_VK_CHECK(context, (mMetalLayer != nullptr), VK_ERROR_INITIALIZATION_FAILED);

    CALayer *parent = reinterpret_cast<CALayer *>(mNativeWindowType);

    if (parent != nil)
    {
        const CGRect  pb = parent.bounds;
        const CGFloat ps = parent.contentsScale;

        // 1) 同步 frame。iOS 上 mMetalLayer 不会自动跟随父 layer 的 bounds 变化。
        if (!RectApproxEqual(mMetalLayer.frame, pb, 0.5))
        {
            if (__builtin_available(iOS 12.0, macOS 10.14, *))
            {
                NSLog(@"[GLGeo][Metal] sync frame (%.2f,%.2f,%.2f,%.2f) -> "
                      @"(%.2f,%.2f,%.2f,%.2f)",
                      static_cast<double>(mMetalLayer.frame.origin.x),
                      static_cast<double>(mMetalLayer.frame.origin.y),
                      static_cast<double>(mMetalLayer.frame.size.width),
                      static_cast<double>(mMetalLayer.frame.size.height),
                      static_cast<double>(pb.origin.x),
                      static_cast<double>(pb.origin.y),
                      static_cast<double>(pb.size.width),
                      static_cast<double>(pb.size.height));
            }
            mMetalLayer.frame = pb;
        }

        // 2) 同步 contentsScale。
        if (std::fabs(static_cast<double>(mMetalLayer.contentsScale - ps)) > 1e-6)
        {
            if (__builtin_available(iOS 12.0, macOS 10.14, *))
            {
                NSLog(@"[GLGeo][Metal] sync contentsScale %.3f -> %.3f",
                      static_cast<double>(mMetalLayer.contentsScale),
                      static_cast<double>(ps));
            }
            mMetalLayer.contentsScale = ps;
        }

        // 3) 关键修复 #2：强制校正 drawableSize = ceil(bounds * scale)。
        //    - 若 createSurfaceVk 已设正确值，这里不触发；
        //    - 若 CA 某次 layout 把 drawableSize 覆盖回 floor 值（562），改回 563；
        //    - 若 bounds 变了，这里也会更新。
        //    这就是防止 viewport 越界 PageFault 的最终兜底。
        const CGSize expected = ComputeSurfaceSize(pb, ps);
        if (std::fabs(static_cast<double>(mMetalLayer.drawableSize.width  - expected.width))  > 1e-6 ||
            std::fabs(static_cast<double>(mMetalLayer.drawableSize.height - expected.height)) > 1e-6)
        {
            if (__builtin_available(iOS 12.0, macOS 10.14, *))
            {
                NSLog(@"[GLGeo][Metal] FIX drawableSize (%.3f,%.3f) -> (%.3f,%.3f) "
                      @"  [关键：ceil 保证 surface >= viewport，修 1px 越界]",
                      static_cast<double>(mMetalLayer.drawableSize.width),
                      static_cast<double>(mMetalLayer.drawableSize.height),
                      static_cast<double>(expected.width),
                      static_cast<double>(expected.height));
            }
            mMetalLayer.drawableSize = expected;
            MetalLogLayerState("after drawableSize fix", parent, mMetalLayer);
        }
    }

    const CGSize ds = mMetalLayer.drawableSize;
    const int w = std::max(1, static_cast<int>(ds.width));
    const int h = std::max(1, static_cast<int>(ds.height));
    *extentsOut = gl::Extents(w, h, 1);

    // 每次调用都打一行，方便诊断。若 logcat 被刷，可改成只在变化时打。
    NSLog(@"[GLGeo][Metal] getCurrentWindowSize -> %dx%d "
          @"(scale=%.2f raw_w=%.3f raw_h=%.3f)",
          w, h,
          static_cast<double>(mMetalLayer.contentsScale),
          static_cast<double>(mMetalLayer.frame.size.width  * mMetalLayer.contentsScale),
          static_cast<double>(mMetalLayer.frame.size.height * mMetalLayer.contentsScale));

    return angle::Result::Continue;
}

}  // namespace rx
