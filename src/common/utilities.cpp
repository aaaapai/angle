//
// Copyright 2002 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// utilities.cpp: Conversion functions and other utility routines.

#include "common/utilities.h"
#include "GLES3/gl3.h"
#include "common/mathutil.h"
#include "common/platform.h"
#include "common/string_utils.h"
#include "common/unsafe_buffers.h"

#include <set>

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
#    include <windows.applicationmodel.core.h>
#    include <windows.graphics.display.h>
#    include <wrl.h>
#    include <wrl/wrappers/corewrappers.h>
#endif

namespace
{

template <class IndexType>
gl::IndexRange ComputeTypedIndexRange(const IndexType *indices,
                                      size_t count,
                                      bool primitiveRestartEnabled)
{
    constexpr IndexType primitiveRestartIndex = std::numeric_limits<IndexType>::max();
    IndexType minIndex                        = primitiveRestartIndex;
    IndexType maxIndex                        = 0;
    bool hasVertices                          = false;

    if (primitiveRestartEnabled)
    {
        for (size_t i = 0; i < count; i++)
        {
            IndexType index = ANGLE_UNSAFE_TODO(indices[i]);
            if (index == primitiveRestartIndex)
            {
                continue;
            }
            hasVertices = true;
            minIndex    = std::min(minIndex, index);
            maxIndex    = std::max(maxIndex, index);
        }
    }
    else
    {
        for (size_t i = 0; i < count; i++)
        {
            IndexType index = ANGLE_UNSAFE_TODO(indices[i]);
            minIndex        = std::min(minIndex, index);
            maxIndex        = std::max(maxIndex, index);
        }
        hasVertices = count > 0;
    }
    if (!hasVertices)
    {
        return gl::IndexRange();
    }
    return gl::IndexRange(minIndex, maxIndex);
}

}  // anonymous namespace

namespace gl
{

// 这些函数已移至头文件并标记为 inline，此处不再重复定义。
// 保留以下非内联函数：

int AllocateFirstFreeBits(unsigned int *bits, unsigned int allocationSize, unsigned int bitsSize)
{
    ASSERT(allocationSize <= bitsSize);

    unsigned int mask = std::numeric_limits<unsigned int>::max() >>
                        (std::numeric_limits<unsigned int>::digits - allocationSize);

    for (unsigned int i = 0; i < bitsSize - allocationSize + 1; i++)
    {
        if ((*bits & mask) == 0)
        {
            *bits |= mask;
            return i;
        }

        mask <<= 1;
    }

    return -1;
}

IndexRange ComputeIndexRange(DrawElementsType indexType,
                             const GLvoid *indices,
                             size_t count,
                             bool primitiveRestartEnabled)
{
    switch (indexType)
    {
        case DrawElementsType::UnsignedByte:
            return ComputeTypedIndexRange(static_cast<const GLubyte *>(indices), count,
                                          primitiveRestartEnabled);
        case DrawElementsType::UnsignedShort:
            return ComputeTypedIndexRange(static_cast<const GLushort *>(indices), count,
                                          primitiveRestartEnabled);
        case DrawElementsType::UnsignedInt:
            return ComputeTypedIndexRange(static_cast<const GLuint *>(indices), count,
                                          primitiveRestartEnabled);
        default:
            UNREACHABLE();
            return IndexRange();
    }
}

std::string ParseResourceName(const std::string &name, std::vector<unsigned int> *outSubscripts)
{
    if (outSubscripts)
    {
        outSubscripts->clear();
    }
    // Strip any trailing array indexing operators and retrieve the subscripts.
    size_t baseNameLength = name.length();
    bool hasIndex         = true;
    while (hasIndex)
    {
        size_t open  = name.find_last_of('[', baseNameLength - 1);
        size_t close = name.find_last_of(']', baseNameLength - 1);
        hasIndex =
            (open != std::string::npos) && (close == baseNameLength - 1) && (close != open + 1);
        if (hasIndex)
        {
            baseNameLength = open;
            if (outSubscripts)
            {
                if (!isdigit(name[open + 1]))
                {
                    outSubscripts->push_back(GL_INVALID_INDEX);
                    break;
                }
                int index = atoi(name.substr(open + 1).c_str());
                if (index >= 0)
                {
                    outSubscripts->push_back(index);
                }
                else
                {
                    outSubscripts->push_back(GL_INVALID_INDEX);
                }
            }
        }
    }

    return name.substr(0, baseNameLength);
}

std::string StripLastArrayIndex(const std::string &name)
{
    size_t strippedNameLength = name.find_last_of('[');
    if (strippedNameLength != std::string::npos && name.back() == ']')
    {
        return name.substr(0, strippedNameLength);
    }
    return name;
}

bool SamplerNameContainsNonZeroArrayElement(const std::string &name)
{
    constexpr char kZERO_ELEMENT[] = "[0]";

    size_t start = 0;
    while (true)
    {
        start = name.find(kZERO_ELEMENT[0], start);
        if (start == std::string::npos)
        {
            break;
        }
        if (name.compare(start, strlen(kZERO_ELEMENT), kZERO_ELEMENT) != 0)
        {
            return true;
        }
        start++;
    }
    return false;
}

unsigned int ParseArrayIndex(const std::string &name, size_t *nameLengthWithoutArrayIndexOut)
{
    ASSERT(nameLengthWithoutArrayIndexOut != nullptr);

    // Strip any trailing array operator and retrieve the subscript
    size_t open = name.find_last_of('[');
    if (open != std::string::npos && name.back() == ']')
    {
        bool indexIsValidDecimalNumber = true;
        for (size_t i = open + 1; i < name.length() - 1u; ++i)
        {
            if (!isdigit(name[i]))
            {
                indexIsValidDecimalNumber = false;
                break;
            }

            // Leading zeroes are invalid
            if ((i == (open + 1)) && (name[i] == '0') && (name[i + 1] != ']'))
            {
                indexIsValidDecimalNumber = false;
                break;
            }
        }
        if (indexIsValidDecimalNumber)
        {
            errno = 0;  // reset global error flag.
            unsigned long subscript = ANGLE_UNSAFE_TODO(
                strtoul(name.c_str() + open + 1, /*endptr*/ nullptr, /*radix*/ 10));

            // Check if resulting integer is out-of-range or conversion error.
            if (angle::base::IsValueInRangeForNumericType<uint32_t>(subscript) &&
                !(subscript == ULONG_MAX && errno == ERANGE) && !(errno != 0 && subscript == 0))
            {
                *nameLengthWithoutArrayIndexOut = open;
                return static_cast<unsigned int>(subscript);
            }
        }
    }

    *nameLengthWithoutArrayIndexOut = name.length();
    return GL_INVALID_INDEX;
}

const char *GetGenericErrorMessage(GLenum error)
{
    switch (error)
    {
        case GL_NO_ERROR:
            return "";
        case GL_INVALID_ENUM:
            return "Invalid enum.";
        case GL_INVALID_VALUE:
            return "Invalid value.";
        case GL_INVALID_OPERATION:
            return "Invalid operation.";
        case GL_STACK_OVERFLOW:
            return "Stack overflow.";
        case GL_STACK_UNDERFLOW:
            return "Stack underflow.";
        case GL_OUT_OF_MEMORY:
            return "Out of memory.";
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "Invalid framebuffer operation.";
        default:
            UNREACHABLE();
            return "Unknown error.";
    }
}

const char *GetDebugMessageSourceString(GLenum source)
{
    switch (source)
    {
        case GL_DEBUG_SOURCE_API:
            return "API";
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
            return "Window System";
        case GL_DEBUG_SOURCE_SHADER_COMPILER:
            return "Shader Compiler";
        case GL_DEBUG_SOURCE_THIRD_PARTY:
            return "Third Party";
        case GL_DEBUG_SOURCE_APPLICATION:
            return "Application";
        case GL_DEBUG_SOURCE_OTHER:
            return "Other";
        default:
            return "Unknown Source";
    }
}

const char *GetDebugMessageTypeString(GLenum type)
{
    switch (type)
    {
        case GL_DEBUG_TYPE_ERROR:
            return "Error";
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
            return "Deprecated behavior";
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
            return "Undefined behavior";
        case GL_DEBUG_TYPE_PORTABILITY:
            return "Portability";
        case GL_DEBUG_TYPE_PERFORMANCE:
            return "Performance";
        case GL_DEBUG_TYPE_OTHER:
            return "Other";
        case GL_DEBUG_TYPE_MARKER:
            return "Marker";
        default:
            return "Unknown Type";
    }
}

const char *GetDebugMessageSeverityString(GLenum severity)
{
    switch (severity)
    {
        case GL_DEBUG_SEVERITY_HIGH:
            return "High";
        case GL_DEBUG_SEVERITY_MEDIUM:
            return "Medium";
        case GL_DEBUG_SEVERITY_LOW:
            return "Low";
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            return "Notification";
        default:
            return "Unknown Severity";
    }
}

}  // namespace gl

namespace egl
{
bool IsCubeMapTextureTarget(EGLenum target)
{
    return (target >= FirstCubeMapTextureTarget && target <= LastCubeMapTextureTarget);
}

size_t CubeMapTextureTargetToLayerIndex(EGLenum target)
{
    ASSERT(IsCubeMapTextureTarget(target));
    return target - static_cast<size_t>(FirstCubeMapTextureTarget);
}

EGLenum LayerIndexToCubeMapTextureTarget(size_t index)
{
    ASSERT(index <= (LastCubeMapTextureTarget - FirstCubeMapTextureTarget));
    return FirstCubeMapTextureTarget + static_cast<GLenum>(index);
}

bool IsTextureTarget(EGLenum target)
{
    switch (target)
    {
        case EGL_GL_TEXTURE_2D_KHR:
        case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X_KHR:
        case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_X_KHR:
        case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_Y_KHR:
        case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_KHR:
        case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_Z_KHR:
        case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_KHR:
        case EGL_GL_TEXTURE_3D_KHR:
            return true;

        default:
            return false;
    }
}

bool IsRenderbufferTarget(EGLenum target)
{
    return target == EGL_GL_RENDERBUFFER_KHR;
}

bool IsExternalImageTarget(EGLenum target)
{
    switch (target)
    {
        case EGL_NATIVE_BUFFER_ANDROID:
        case EGL_D3D11_TEXTURE_ANGLE:
        case EGL_WEBGPU_TEXTURE_ANGLE:
        case EGL_LINUX_DMA_BUF_EXT:
        case EGL_METAL_TEXTURE_ANGLE:
        case EGL_VULKAN_IMAGE_ANGLE:
            return true;

        default:
            return false;
    }
}

const char *GetGenericErrorMessage(EGLint error)
{
    switch (error)
    {
        case EGL_SUCCESS:
            return "";
        case EGL_NOT_INITIALIZED:
            return "Not initialized.";
        case EGL_BAD_ACCESS:
            return "Bad access.";
        case EGL_BAD_ALLOC:
            return "Bad allocation.";
        case EGL_BAD_ATTRIBUTE:
            return "Bad attribute.";
        case EGL_BAD_CONFIG:
            return "Bad config.";
        case EGL_BAD_CONTEXT:
            return "Bad context.";
        case EGL_BAD_CURRENT_SURFACE:
            return "Bad current surface.";
        case EGL_BAD_DISPLAY:
            return "Bad display.";
        case EGL_BAD_MATCH:
            return "Bad match.";
        case EGL_BAD_NATIVE_WINDOW:
            return "Bad native window.";
        case EGL_BAD_NATIVE_PIXMAP:
            return "Bad native pixmap.";
        case EGL_BAD_PARAMETER:
            return "Bad parameter.";
        case EGL_BAD_SURFACE:
            return "Bad surface.";
        case EGL_CONTEXT_LOST:
            return "Context lost.";
        case EGL_BAD_STREAM_KHR:
            return "Bad stream.";
        case EGL_BAD_STATE_KHR:
            return "Bad state.";
        case EGL_BAD_DEVICE_EXT:
            return "Bad device.";
        default:
            UNREACHABLE();
            return "Unknown error.";
    }
}

}  // namespace egl

namespace egl_gl
{
GLuint EGLClientBufferToGLObjectHandle(EGLClientBuffer buffer)
{
    return static_cast<GLuint>(reinterpret_cast<uintptr_t>(buffer));
}
}  // namespace egl_gl

namespace gl_egl
{
EGLenum GLComponentTypeToEGLColorComponentType(GLenum glComponentType)
{
    switch (glComponentType)
    {
        case GL_FLOAT:
            return EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT;

        case GL_UNSIGNED_NORMALIZED:
            return EGL_COLOR_COMPONENT_TYPE_FIXED_EXT;

        default:
            UNREACHABLE();
            return EGL_NONE;
    }
}

EGLClientBuffer GLObjectHandleToEGLClientBuffer(GLuint handle)
{
    return reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(handle));
}

}  // namespace gl_egl

namespace angle
{
bool IsDrawEntryPoint(EntryPoint entryPoint)
{
    switch (entryPoint)
    {
        case EntryPoint::GLDrawArrays:
        case EntryPoint::GLDrawArraysIndirect:
        case EntryPoint::GLDrawArraysInstanced:
        case EntryPoint::GLDrawArraysInstancedANGLE:
        case EntryPoint::GLDrawArraysInstancedBaseInstanceANGLE:
        case EntryPoint::GLDrawArraysInstancedEXT:
        case EntryPoint::GLDrawElements:
        case EntryPoint::GLDrawElementsBaseVertex:
        case EntryPoint::GLDrawElementsBaseVertexEXT:
        case EntryPoint::GLDrawElementsBaseVertexOES:
        case EntryPoint::GLDrawElementsIndirect:
        case EntryPoint::GLDrawElementsInstanced:
        case EntryPoint::GLDrawElementsInstancedANGLE:
        case EntryPoint::GLDrawElementsInstancedBaseVertexBaseInstanceANGLE:
        case EntryPoint::GLDrawElementsInstancedBaseVertexEXT:
        case EntryPoint::GLDrawElementsInstancedBaseVertexOES:
        case EntryPoint::GLDrawElementsInstancedEXT:
        case EntryPoint::GLDrawRangeElements:
        case EntryPoint::GLDrawRangeElementsBaseVertex:
        case EntryPoint::GLDrawRangeElementsBaseVertexEXT:
        case EntryPoint::GLDrawRangeElementsBaseVertexOES:
        case EntryPoint::GLDrawTexfOES:
        case EntryPoint::GLDrawTexfvOES:
        case EntryPoint::GLDrawTexiOES:
        case EntryPoint::GLDrawTexivOES:
        case EntryPoint::GLDrawTexsOES:
        case EntryPoint::GLDrawTexsvOES:
        case EntryPoint::GLDrawTexxOES:
        case EntryPoint::GLDrawTexxvOES:
            return true;
        default:
            return false;
    }
}

bool IsDispatchEntryPoint(EntryPoint entryPoint)
{
    switch (entryPoint)
    {
        case EntryPoint::GLDispatchCompute:
        case EntryPoint::GLDispatchComputeIndirect:
            return true;
        default:
            return false;
    }
}

bool IsClearEntryPoint(EntryPoint entryPoint)
{
    switch (entryPoint)
    {
        case EntryPoint::GLClear:
        case EntryPoint::GLClearBufferfi:
        case EntryPoint::GLClearBufferfv:
        case EntryPoint::GLClearBufferiv:
        case EntryPoint::GLClearBufferuiv:
            return true;
        default:
            return false;
    }
}

bool IsQueryEntryPoint(EntryPoint entryPoint)
{
    switch (entryPoint)
    {
        case EntryPoint::GLBeginQuery:
        case EntryPoint::GLBeginQueryEXT:
        case EntryPoint::GLEndQuery:
        case EntryPoint::GLEndQueryEXT:
            return true;
        default:
            return false;
    }
}
}  // namespace angle

void writeFile(const char *path, std::string_view content)
{
#if !defined(ANGLE_ENABLE_WINDOWS_UWP)
    FILE *file = fopen(path, "w");
    if (!file)
    {
        UNREACHABLE();
        return;
    }

    ANGLE_UNSAFE_TODO(fwrite(content.data(), sizeof(char), content.size(), file));
    fclose(file);
#else
    UNREACHABLE();
    return;
#endif  // !ANGLE_ENABLE_WINDOWS_UWP
}