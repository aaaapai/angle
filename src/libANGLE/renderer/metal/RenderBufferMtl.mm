//
// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RenderBufferMtl.mm:
//    Implements the class methods for RenderBufferMtl.
//

#include "libANGLE/renderer/metal/RenderBufferMtl.h"
#include "libANGLE/renderer/metal/TextureMtl.h"

#include "libANGLE/ErrorStrings.h"
#include "libANGLE/renderer/metal/ContextMtl.h"
#include "libANGLE/renderer/metal/ImageMtl.h"
#include "libANGLE/renderer/metal/mtl_format_utils.h"
#include "libANGLE/renderer/metal/mtl_utils.h"

namespace rx
{

RenderbufferMtl::RenderbufferMtl(const gl::RenderbufferState &state) : RenderbufferImpl(state) {}

RenderbufferMtl::~RenderbufferMtl() {}

void RenderbufferMtl::onDestroy(const gl::Context *context)
{
    releaseTexture();
}

void RenderbufferMtl::releaseTexture()
{
    mTexture           = nullptr;
    mImplicitMSTexture = nullptr;
}

angle::Result RenderbufferMtl::copyRenderbufferSubData(const gl::Context *context,
                                                       const gl::Renderbuffer *srcBuffer,
                                                       GLint srcX,
                                                       GLint srcY,
                                                       GLint dstX,
                                                       GLint dstY,
                                                       GLsizei srcWidth,
                                                       GLsizei srcHeight)
{
    // MTL31-PARTIAL: MTL31-EXT-copyimage - same-format 2D copies only; Metal blit requires
    // identical pixel formats.
    ContextMtl *contextMtl          = mtl::GetImpl(context);
    RenderbufferMtl *srcMtl         = mtl::GetImpl(srcBuffer);
    const mtl::TextureRef &srcImage = srcMtl->getTexture();
    const mtl::TextureRef &dstImage = getTexture();
    ANGLE_CHECK(contextMtl, srcImage && dstImage, gl::err::kInternalError, GL_INVALID_OPERATION);
    ANGLE_CHECK(contextMtl, srcImage->pixelFormat() == dstImage->pixelFormat(),
                gl::err::kInternalError, GL_INVALID_OPERATION);
    ANGLE_CHECK(contextMtl, srcImage->samples() == dstImage->samples(), gl::err::kInternalError,
                GL_INVALID_OPERATION);

    mtl::BlitCommandEncoder *encoder = contextMtl->getBlitCommandEncoder();
    encoder->copyTextureRegion(srcImage, 0, mtl::kZeroNativeMipLevel, MTLOriginMake(srcX, srcY, 0),
                               MTLSizeMake(srcWidth, srcHeight, 1), dstImage, 0,
                               mtl::kZeroNativeMipLevel, MTLOriginMake(dstX, dstY, 0));
    return angle::Result::Continue;
}

angle::Result RenderbufferMtl::copyTextureSubData(const gl::Context *context,
                                                  const gl::Texture *srcTexture,
                                                  gl::LevelIndex srcLevel,
                                                  GLint srcX,
                                                  GLint srcY,
                                                  gl::LayerIndex srcZ,
                                                  GLint dstX,
                                                  GLint dstY,
                                                  GLsizei srcWidth,
                                                  GLsizei srcHeight)
{
    ContextMtl *contextMtl = mtl::GetImpl(context);
    TextureMtl *srcMtl     = mtl::GetImpl(srcTexture);

    gl::ImageIndex srcIndex;
    switch (srcTexture->getType())
    {
        case gl::TextureType::_2D:
        case gl::TextureType::_2DMultisample:
            srcIndex = gl::ImageIndex::Make2D(static_cast<GLint>(srcLevel.get()));
            break;
        case gl::TextureType::_2DArray:
            srcIndex = gl::ImageIndex::Make2DArray(static_cast<GLint>(srcLevel.get()));
            break;
        case gl::TextureType::_3D:
            srcIndex = gl::ImageIndex::Make3D(static_cast<GLint>(srcLevel.get()));
            break;
        case gl::TextureType::CubeMap:
        {
            if (srcZ.get() >= 6)
            {
                ANGLE_CHECK(contextMtl, false, gl::err::kInternalError, GL_INVALID_OPERATION);
            }
            const gl::TextureTarget face = static_cast<gl::TextureTarget>(
                static_cast<uint8_t>(gl::kCubeMapTextureTargetMin) + srcZ.get());
            srcIndex = gl::ImageIndex::MakeCubeMapFace(face, static_cast<GLint>(srcLevel.get()));
            break;
        }
        default:
            ANGLE_CHECK(contextMtl, false, gl::err::kInternalError, GL_INVALID_OPERATION);
            break;
    }

    mtl::TextureRef srcImage;
    ANGLE_TRY(srcMtl->getImageForCopy(context, srcIndex, &srcImage));
    const mtl::TextureRef &dstImage = getTexture();
    ANGLE_CHECK(contextMtl, srcImage && dstImage, gl::err::kInternalError, GL_INVALID_OPERATION);
    ANGLE_CHECK(contextMtl, srcImage->pixelFormat() == dstImage->pixelFormat(),
                gl::err::kInternalError, GL_INVALID_OPERATION);
    ANGLE_CHECK(contextMtl, srcImage->samples() == dstImage->samples(), gl::err::kInternalError,
                GL_INVALID_OPERATION);

    const uint32_t srcSlice          = (srcTexture->getType() == gl::TextureType::CubeMap ||
                               srcTexture->getType() == gl::TextureType::_2D)
                                           ? 0u
                                           : srcZ.get();
    mtl::BlitCommandEncoder *encoder = contextMtl->getBlitCommandEncoder();
    encoder->copyTextureRegion(srcImage, srcSlice, mtl::kZeroNativeMipLevel,
                               MTLOriginMake(srcX, srcY, 0), MTLSizeMake(srcWidth, srcHeight, 1),
                               dstImage, 0, mtl::kZeroNativeMipLevel, MTLOriginMake(dstX, dstY, 0));
    return angle::Result::Continue;
}

angle::Result RenderbufferMtl::setStorageImpl(const gl::Context *context,
                                              GLsizei samples,
                                              GLenum internalformat,
                                              GLsizei width,
                                              GLsizei height,
                                              gl::MultisamplingMode mode)
{
    ContextMtl *contextMtl = mtl::GetImpl(context);

    if (mTexture != nullptr && mTexture->valid())
    {
        // Check against the state if we need to recreate the storage.
        if (internalformat != mState.getFormat().info->internalFormat ||
            width != mState.getWidth() || height != mState.getHeight() ||
            samples != mState.getSamples())
        {
            releaseTexture();
        }
    }

    const gl::InternalFormat &internalFormat = gl::GetSizedInternalFormatInfo(internalformat);
    angle::FormatID angleFormatId =
        angle::Format::InternalFormatToID(internalFormat.sizedInternalFormat);
    mFormat = contextMtl->getPixelFormat(angleFormatId);

    uint32_t actualSamples;
    if (samples == 0)
    {
        actualSamples = 1;
    }
    else
    {
        // We always start at at least 2 samples
        actualSamples = static_cast<uint32_t>(std::max<size_t>(2, samples));

        const gl::TextureCaps &textureCaps =
            contextMtl->getTextureCaps().get(mFormat.intendedFormatId);
        actualSamples = textureCaps.sampleCounts.getNearestSamples(actualSamples);
        ANGLE_CHECK(contextMtl, actualSamples != 0, gl::err::kInternalError, GL_INVALID_VALUE);
    }

    if ((mTexture == nullptr || !mTexture->valid()) && (width != 0 && height != 0))
    {
        if (actualSamples == 1 || mode == gl::MultisamplingMode::MultisampledRenderToTexture)
        {
            ANGLE_TRY(mtl::Texture::Make2DTexture(
                contextMtl, mFormat, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1,
                /* renderTargetOnly */ false,
                /* allowFormatView */ mFormat.hasDepthAndStencilBits(), &mTexture));

            if (mode == gl::MultisamplingMode::MultisampledRenderToTexture)
            {
                // This format must supports implicit resolve
                ASSERT(mFormat.getCaps().resolve);

                ANGLE_TRY(mtl::Texture::MakeMemoryLess2DMSTexture(
                    contextMtl, mFormat, static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height), actualSamples, &mImplicitMSTexture));
            }
        }
        else
        {
            ANGLE_TRY(mtl::Texture::Make2DMSTexture(
                contextMtl, mFormat, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                actualSamples,
                /* renderTargetOnly */ false,
                /* allowFormatView */ mFormat.hasDepthAndStencilBits(), &mTexture));
        }

        mRenderTarget.setWithImplicitMSTexture(mTexture, mImplicitMSTexture,
                                               mtl::kZeroNativeMipLevel, 0, mFormat);

        // For emulated channels that GL texture intends to not have,
        // we need to initialize their content.
        bool emulatedChannels = mtl::IsFormatEmulated(mFormat);
        bool isDepthStencil   = mFormat.hasDepthOrStencilBits();
        bool toNonZero = contextMtl->getDisplay()->getFeatures().allocateNonZeroTextures.enabled;

        if (toNonZero || emulatedChannels || isDepthStencil)
        {
            gl::ImageIndex index = mTexture->samples() > 1 ? gl::ImageIndex::Make2DMultisample()
                                                           : gl::ImageIndex::Make2D(0);
            auto nativeIndex     = mtl::ImageNativeIndex(index, 0);

            if (isDepthStencil)
            {
                ANGLE_TRY(mtl::InitializeDepthStencilTextureContentsGPU(context, mTexture, mFormat,
                                                                        nativeIndex, toNonZero));
            }
            else
            {
                ANGLE_TRY(mtl::InitializeTextureContents(context, mTexture, mFormat, nativeIndex,
                                                         toNonZero));
            }
        }
    }

    return angle::Result::Continue;
}

angle::Result RenderbufferMtl::setStorage(const gl::Context *context,
                                          GLenum internalformat,
                                          GLsizei width,
                                          GLsizei height)
{
    return setStorageImpl(context, 0, internalformat, width, height,
                          gl::MultisamplingMode::Regular);
}

angle::Result RenderbufferMtl::setStorageMultisample(const gl::Context *context,
                                                     GLsizei samples,
                                                     GLenum internalformat,
                                                     GLsizei width,
                                                     GLsizei height,
                                                     gl::MultisamplingMode mode)
{
    return setStorageImpl(context, samples, internalformat, width, height, mode);
}

angle::Result RenderbufferMtl::setStorageEGLImageTarget(const gl::Context *context,
                                                        egl::Image *image)
{
    releaseTexture();

    ContextMtl *contextMtl = mtl::GetImpl(context);

    ImageMtl *imageMtl = mtl::GetImpl(image);
    mTexture           = imageMtl->getTexture();

    const angle::FormatID angleFormatId =
        angle::Format::InternalFormatToID(image->getFormat().info->sizedInternalFormat);
    mFormat = contextMtl->getPixelFormat(angleFormatId);

    mRenderTarget.set(mTexture, mtl::kZeroNativeMipLevel, 0, mFormat);

    return angle::Result::Continue;
}

angle::Result RenderbufferMtl::getAttachmentRenderTarget(const gl::Context *context,
                                                         GLenum binding,
                                                         const gl::ImageIndex &imageIndex,
                                                         GLsizei samples,
                                                         FramebufferAttachmentRenderTarget **rtOut)
{
    ASSERT(mTexture && mTexture->valid());
    *rtOut = &mRenderTarget;
    return angle::Result::Continue;
}

angle::Result RenderbufferMtl::initializeContents(const gl::Context *context,
                                                  GLenum binding,
                                                  const gl::ImageIndex &imageIndex)
{
    if (imageIndex.valid())
        return mtl::InitializeTextureContents(
            context, mTexture, mFormat, mtl::ImageNativeIndex::FromBaseZeroGLIndex(imageIndex));
    else
        return mtl::InitializeTextureContents(
            context, mTexture, mFormat,
            mtl::ImageNativeIndex::FromBaseZeroGLIndex(gl::ImageIndex::Make2D(0)));
}
}  // namespace rx
