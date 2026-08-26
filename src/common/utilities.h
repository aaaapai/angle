//
// Copyright 2002 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// utilities.h: Conversion functions and other utility routines.

#ifndef COMMON_UTILITIES_H_
#define COMMON_UTILITIES_H_

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLSLANG/ShaderLang.h>
#include "common/unsafe_buffers.h"

#include <math.h>
#include <string>
#include <string_view>
#include <vector>

#include "angle_gl.h"

#include "common/PackedEnums.h"
#include "common/mathutil.h"
#include "common/platform.h"

namespace sh
{
struct ShaderVariable;
}

namespace gl
{

// 这些轻量级查询函数现在声明为 inline 并在头文件中定义，以允许内联优化。
inline int VariableComponentCount(GLenum type);
inline GLenum VariableComponentType(GLenum type);
inline size_t VariableComponentSize(GLenum type);
inline size_t VariableInternalSize(GLenum type);
inline size_t VariableExternalSize(GLenum type);
inline int VariableRowCount(GLenum type);
inline int VariableColumnCount(GLenum type);
inline bool IsSamplerType(GLenum type);
inline bool IsSamplerCubeType(GLenum type);
inline bool IsSamplerYUVType(GLenum type);
inline bool IsImageType(GLenum type);
inline bool IsImage2DType(GLenum type);
inline bool IsAtomicCounterType(GLenum type);
inline bool IsOpaqueType(GLenum type);
inline bool IsMatrixType(GLenum type);
inline bool IsFloatScalarAndVectorType(GLenum type);
inline bool IsFloatVectorType(GLenum type);
inline GLenum TransposeMatrixType(GLenum type);
inline int VariableRegisterCount(GLenum type);
inline int MatrixRegisterCount(GLenum type, bool isRowMajorMatrix);
inline int MatrixComponentCount(GLenum type, bool isRowMajorMatrix);
inline int VariableSortOrder(GLenum type);
inline GLenum VariableBoolVectorType(GLenum type);
inline std::string GetGLSLTypeString(GLenum type);

int AllocateFirstFreeBits(unsigned int *bits, unsigned int allocationSize, unsigned int bitsSize);

// Parse the base resource name and array indices. Returns the base name of the resource.
// If the provided name doesn't index an array, the outSubscripts vector will be empty.
// If the provided name indexes an array, the outSubscripts vector will contain indices with
// outermost array indices in the back. If an array index is invalid, GL_INVALID_INDEX is added to
// outSubscripts.
std::string ParseResourceName(const std::string &name, std::vector<unsigned int> *outSubscripts);

inline bool IsBuiltInName(const char *name);
inline bool IsBuiltInName(const std::string &name);

// Strips only the last array index from a resource name.
std::string StripLastArrayIndex(const std::string &name);

bool SamplerNameContainsNonZeroArrayElement(const std::string &name);

// Find the range of index values in the provided indices pointer.  Primitive restart indices are
// only counted in the range if primitive restart is disabled.
IndexRange ComputeIndexRange(DrawElementsType indexType,
                             const GLvoid *indices,
                             size_t count,
                             bool primitiveRestartEnabled);

// Get the primitive restart index value for the given index type.
inline GLuint GetPrimitiveRestartIndex(DrawElementsType indexType);

// Get the primitive restart index value with the given C++ type.
template <typename T>
constexpr T GetPrimitiveRestartIndexFromType()
{
    return std::numeric_limits<T>::max();
}

static_assert(GetPrimitiveRestartIndexFromType<uint8_t>() == 0xFF,
              "verify restart index for uint8_t values");
static_assert(GetPrimitiveRestartIndexFromType<uint16_t>() == 0xFFFF,
              "verify restart index for uint8_t values");
static_assert(GetPrimitiveRestartIndexFromType<uint32_t>() == 0xFFFFFFFF,
              "verify restart index for uint8_t values");

inline bool IsTriangleMode(PrimitiveMode drawMode);
inline bool IsPolygonMode(PrimitiveMode mode);

namespace priv
{
extern const angle::PackedEnumMap<PrimitiveMode, bool> gLineModes;
}  // namespace priv

inline bool IsLineMode(PrimitiveMode primitiveMode);

inline bool IsIntegerFormat(GLenum unsizedFormat);

// Returns the product of the sizes in the vector, or 1 if the vector is empty. Doesn't currently
// perform overflow checks.
inline unsigned int ArraySizeProduct(const std::vector<unsigned int> &arraySizes);
// Returns the product of the sizes in the vector except for the outermost dimension, or 1 if the
// vector is empty.
inline unsigned int InnerArraySizeProduct(const std::vector<unsigned int> &arraySizes);
// Returns the outermost array dimension, or 1 if the vector is empty.
inline unsigned int OutermostArraySize(const std::vector<unsigned int> &arraySizes);

// Return the array index at the end of name, and write the length of name before the final array
// index into nameLengthWithoutArrayIndexOut. In case name doesn't include an array index, return
// GL_INVALID_INDEX and write the length of the original string.
unsigned int ParseArrayIndex(const std::string &name, size_t *nameLengthWithoutArrayIndexOut);

enum class SamplerFormat : uint8_t
{
    Float    = 0,
    Unsigned = 1,
    Signed   = 2,
    Shadow   = 3,

    InvalidEnum = 4,
    EnumCount   = 4,
};

struct UniformTypeInfo final : angle::NonCopyable
{
    inline constexpr UniformTypeInfo(GLenum type,
                                     GLenum componentType,
                                     GLenum textureType,
                                     GLenum transposedMatrixType,
                                     GLenum boolVectorType,
                                     SamplerFormat samplerFormat,
                                     int rowCount,
                                     int columnCount,
                                     int componentCount,
                                     size_t componentSize,
                                     size_t internalSize,
                                     size_t externalSize,
                                     bool isSampler,
                                     bool isMatrixType,
                                     bool isImageType);

    GLenum type;
    GLenum componentType;
    GLenum textureType;
    GLenum transposedMatrixType;
    GLenum boolVectorType;
    SamplerFormat samplerFormat;
    int rowCount;
    int columnCount;
    int componentCount;
    size_t componentSize;
    size_t internalSize;
    size_t externalSize;
    bool isSampler;
    bool isMatrixType;
    bool isImageType;
};

inline constexpr UniformTypeInfo::UniformTypeInfo(GLenum type,
                                                  GLenum componentType,
                                                  GLenum textureType,
                                                  GLenum transposedMatrixType,
                                                  GLenum boolVectorType,
                                                  SamplerFormat samplerFormat,
                                                  int rowCount,
                                                  int columnCount,
                                                  int componentCount,
                                                  size_t componentSize,
                                                  size_t internalSize,
                                                  size_t externalSize,
                                                  bool isSampler,
                                                  bool isMatrixType,
                                                  bool isImageType)
    : type(type),
      componentType(componentType),
      textureType(textureType),
      transposedMatrixType(transposedMatrixType),
      boolVectorType(boolVectorType),
      samplerFormat(samplerFormat),
      rowCount(rowCount),
      columnCount(columnCount),
      componentCount(componentCount),
      componentSize(componentSize),
      internalSize(internalSize),
      externalSize(externalSize),
      isSampler(isSampler),
      isMatrixType(isMatrixType),
      isImageType(isImageType)
{}

struct UniformTypeIndex
{
    uint16_t value;
};
const UniformTypeInfo &GetUniformTypeInfo(GLenum uniformType);
UniformTypeIndex GetUniformTypeIndex(GLenum uniformType);

const char *GetGenericErrorMessage(GLenum error);

inline unsigned int ElementTypeSize(GLenum elementType);

inline bool IsMipmapFiltered(GLenum minFilterMode);

template <typename T>
inline T GetClampedVertexCount(size_t vertexCount)
{
    static constexpr size_t kMax = static_cast<size_t>(std::numeric_limits<T>::max());
    return static_cast<T>(vertexCount > kMax ? kMax : vertexCount);
}

enum class PipelineType
{
    GraphicsPipeline = 0,
    ComputePipeline  = 1,
};

inline PipelineType GetPipelineType(ShaderType shaderType);

// For use with KHR_debug.
const char *GetDebugMessageSourceString(GLenum source);
const char *GetDebugMessageTypeString(GLenum type);
const char *GetDebugMessageSeverityString(GLenum severity);

// For use with EXT_texture_sRGB_decode
// A texture may be forced to skip decoding to a linear colorspace even if its format
// is in sRGB colorspace.
//
// Default - decode data according to the image's format's colorspace
// Skip - data is not decoded during sampling
enum class SrgbDecode
{
    Default = 0,
    Skip
};

// For use with EXT_texture_format_sRGB_override
// A texture may be forced to decode data to linear colorspace even if its format
// is in linear colorspace.
//
// Default - decode data according to the image's format's colorspace
// SRGB - data will be decoded to linear colorspace irrespective of texture's format
enum class SrgbOverride
{
    Default = 0,
    SRGB
};

// For use with EXT_sRGB_write_control
// A framebuffer may be forced to not encode data to sRGB colorspace even if its format
// is in sRGB colorspace.
//
// Default - encode data according to the image's format's colorspace
// Linear - data will not be encoded into sRGB colorspace
enum class SrgbWriteControlMode
{
    Default = 0,
    Linear  = 1
};

// For use with EXT_YUV_target
// A sampler of external YUV textures may either implicitly perform RGB conversion (regular
// samplerExternalOES) or skip the conversion and sample raw YUV values (__samplerExternal2DY2Y).
enum class YuvSamplingMode
{
    Default = 0,
    Y2Y     = 1
};

inline ShaderType GetShaderTypeFromBitfield(size_t singleShaderType);
inline GLbitfield GetBitfieldFromShaderType(ShaderType shaderType);
inline bool ShaderTypeSupportsTransformFeedback(ShaderType shaderType);
inline ShaderType GetLastPreFragmentStage(ShaderBitSet shaderTypes);

// ---------- 内联函数实现 ----------

inline int VariableComponentCount(GLenum type)
{
    return VariableRowCount(type) * VariableColumnCount(type);
}

inline GLenum VariableComponentType(GLenum type)
{
    switch (type)
    {
        case GL_BOOL:
        case GL_BOOL_VEC2:
        case GL_BOOL_VEC3:
        case GL_BOOL_VEC4:
            return GL_BOOL;
        case GL_FLOAT:
        case GL_FLOAT_VEC2:
        case GL_FLOAT_VEC3:
        case GL_FLOAT_VEC4:
        case GL_FLOAT_MAT2:
        case GL_FLOAT_MAT3:
        case GL_FLOAT_MAT4:
        case GL_FLOAT_MAT2x3:
        case GL_FLOAT_MAT3x2:
        case GL_FLOAT_MAT2x4:
        case GL_FLOAT_MAT4x2:
        case GL_FLOAT_MAT3x4:
        case GL_FLOAT_MAT4x3:
            return GL_FLOAT;
        case GL_INT:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_2D_RECT_ANGLE:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_EXTERNAL_OES:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_BUFFER:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_INT_VEC2:
        case GL_INT_VEC3:
        case GL_INT_VEC4:
        case GL_IMAGE_2D:
        case GL_INT_IMAGE_2D:
        case GL_UNSIGNED_INT_IMAGE_2D:
        case GL_IMAGE_3D:
        case GL_INT_IMAGE_3D:
        case GL_UNSIGNED_INT_IMAGE_3D:
        case GL_IMAGE_2D_ARRAY:
        case GL_INT_IMAGE_2D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
        case GL_IMAGE_CUBE:
        case GL_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_IMAGE_CUBE:
        case GL_IMAGE_CUBE_MAP_ARRAY:
        case GL_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_IMAGE_BUFFER:
        case GL_INT_IMAGE_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_IMAGE_BUFFER:
        case GL_UNSIGNED_INT_ATOMIC_COUNTER:
        case GL_SAMPLER_EXTERNAL_2D_Y2Y_EXT:
            return GL_INT;
        case GL_UNSIGNED_INT:
        case GL_UNSIGNED_INT_VEC2:
        case GL_UNSIGNED_INT_VEC3:
        case GL_UNSIGNED_INT_VEC4:
            return GL_UNSIGNED_INT;
        default:
            UNREACHABLE();
    }

    return GL_NONE;
}

inline size_t VariableComponentSize(GLenum type)
{
    switch (type)
    {
        case GL_BOOL:
            return sizeof(GLint);
        case GL_FLOAT:
            return sizeof(GLfloat);
        case GL_INT:
            return sizeof(GLint);
        case GL_UNSIGNED_INT:
            return sizeof(GLuint);
        default:
            UNREACHABLE();
    }

    return 0;
}

inline size_t VariableInternalSize(GLenum type)
{
    // Expanded to 4-element vectors
    return VariableComponentSize(VariableComponentType(type)) * VariableRowCount(type) * 4;
}

inline size_t VariableExternalSize(GLenum type)
{
    return VariableComponentSize(VariableComponentType(type)) * VariableComponentCount(type);
}

inline std::string GetGLSLTypeString(GLenum type)
{
    switch (type)
    {
        case GL_BOOL:
            return "bool";
        case GL_INT:
            return "int";
        case GL_UNSIGNED_INT:
            return "uint";
        case GL_FLOAT:
            return "float";
        case GL_BOOL_VEC2:
            return "bvec2";
        case GL_BOOL_VEC3:
            return "bvec3";
        case GL_BOOL_VEC4:
            return "bvec4";
        case GL_INT_VEC2:
            return "ivec2";
        case GL_INT_VEC3:
            return "ivec3";
        case GL_INT_VEC4:
            return "ivec4";
        case GL_FLOAT_VEC2:
            return "vec2";
        case GL_FLOAT_VEC3:
            return "vec3";
        case GL_FLOAT_VEC4:
            return "vec4";
        case GL_UNSIGNED_INT_VEC2:
            return "uvec2";
        case GL_UNSIGNED_INT_VEC3:
            return "uvec3";
        case GL_UNSIGNED_INT_VEC4:
            return "uvec4";
        case GL_FLOAT_MAT2:
            return "mat2";
        case GL_FLOAT_MAT3:
            return "mat3";
        case GL_FLOAT_MAT4:
            return "mat4";
        default:
            UNREACHABLE();
            return "";
    }
}

inline GLenum VariableBoolVectorType(GLenum type)
{
    switch (type)
    {
        case GL_FLOAT:
        case GL_INT:
        case GL_UNSIGNED_INT:
            return GL_BOOL;
        case GL_FLOAT_VEC2:
        case GL_INT_VEC2:
        case GL_UNSIGNED_INT_VEC2:
            return GL_BOOL_VEC2;
        case GL_FLOAT_VEC3:
        case GL_INT_VEC3:
        case GL_UNSIGNED_INT_VEC3:
            return GL_BOOL_VEC3;
        case GL_FLOAT_VEC4:
        case GL_INT_VEC4:
        case GL_UNSIGNED_INT_VEC4:
            return GL_BOOL_VEC4;

        default:
            UNREACHABLE();
            return GL_NONE;
    }
}

inline int VariableRowCount(GLenum type)
{
    switch (type)
    {
        case GL_NONE:
            return 0;
        case GL_BOOL:
        case GL_FLOAT:
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_BOOL_VEC2:
        case GL_FLOAT_VEC2:
        case GL_INT_VEC2:
        case GL_UNSIGNED_INT_VEC2:
        case GL_BOOL_VEC3:
        case GL_FLOAT_VEC3:
        case GL_INT_VEC3:
        case GL_UNSIGNED_INT_VEC3:
        case GL_BOOL_VEC4:
        case GL_FLOAT_VEC4:
        case GL_INT_VEC4:
        case GL_UNSIGNED_INT_VEC4:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_EXTERNAL_OES:
        case GL_SAMPLER_2D_RECT_ANGLE:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
        case GL_IMAGE_2D:
        case GL_INT_IMAGE_2D:
        case GL_UNSIGNED_INT_IMAGE_2D:
        case GL_IMAGE_2D_ARRAY:
        case GL_INT_IMAGE_2D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
        case GL_IMAGE_3D:
        case GL_INT_IMAGE_3D:
        case GL_UNSIGNED_INT_IMAGE_3D:
        case GL_IMAGE_CUBE:
        case GL_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_ATOMIC_COUNTER:
        case GL_IMAGE_CUBE_MAP_ARRAY:
        case GL_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_IMAGE_BUFFER:
        case GL_INT_IMAGE_BUFFER:
        case GL_UNSIGNED_INT_IMAGE_BUFFER:
        case GL_SAMPLER_EXTERNAL_2D_Y2Y_EXT:
            return 1;
        case GL_FLOAT_MAT2:
        case GL_FLOAT_MAT3x2:
        case GL_FLOAT_MAT4x2:
            return 2;
        case GL_FLOAT_MAT3:
        case GL_FLOAT_MAT2x3:
        case GL_FLOAT_MAT4x3:
            return 3;
        case GL_FLOAT_MAT4:
        case GL_FLOAT_MAT2x4:
        case GL_FLOAT_MAT3x4:
            return 4;
        default:
            UNREACHABLE();
    }

    return 0;
}

inline int VariableColumnCount(GLenum type)
{
    switch (type)
    {
        case GL_NONE:
            return 0;
        case GL_BOOL:
        case GL_FLOAT:
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_SAMPLER_EXTERNAL_OES:
        case GL_SAMPLER_2D_RECT_ANGLE:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
        case GL_IMAGE_2D:
        case GL_INT_IMAGE_2D:
        case GL_UNSIGNED_INT_IMAGE_2D:
        case GL_IMAGE_3D:
        case GL_INT_IMAGE_3D:
        case GL_UNSIGNED_INT_IMAGE_3D:
        case GL_IMAGE_2D_ARRAY:
        case GL_INT_IMAGE_2D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
        case GL_IMAGE_CUBE_MAP_ARRAY:
        case GL_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_IMAGE_BUFFER:
        case GL_INT_IMAGE_BUFFER:
        case GL_UNSIGNED_INT_IMAGE_BUFFER:
        case GL_IMAGE_CUBE:
        case GL_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_ATOMIC_COUNTER:
        case GL_SAMPLER_EXTERNAL_2D_Y2Y_EXT:
            return 1;
        case GL_BOOL_VEC2:
        case GL_FLOAT_VEC2:
        case GL_INT_VEC2:
        case GL_UNSIGNED_INT_VEC2:
        case GL_FLOAT_MAT2:
        case GL_FLOAT_MAT2x3:
        case GL_FLOAT_MAT2x4:
            return 2;
        case GL_BOOL_VEC3:
        case GL_FLOAT_VEC3:
        case GL_INT_VEC3:
        case GL_UNSIGNED_INT_VEC3:
        case GL_FLOAT_MAT3:
        case GL_FLOAT_MAT3x2:
        case GL_FLOAT_MAT3x4:
            return 3;
        case GL_BOOL_VEC4:
        case GL_FLOAT_VEC4:
        case GL_INT_VEC4:
        case GL_UNSIGNED_INT_VEC4:
        case GL_FLOAT_MAT4:
        case GL_FLOAT_MAT4x2:
        case GL_FLOAT_MAT4x3:
            return 4;
        default:
            UNREACHABLE();
    }

    return 0;
}

inline bool IsSamplerType(GLenum type)
{
    switch (type)
    {
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_EXTERNAL_OES:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_RECT_ANGLE:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
        case GL_SAMPLER_EXTERNAL_2D_Y2Y_EXT:
            return true;
    }

    return false;
}

inline bool IsSamplerCubeType(GLenum type)
{
    switch (type)
    {
        case GL_SAMPLER_CUBE:
        case GL_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_SAMPLER_CUBE_SHADOW:
            return true;
    }

    return false;
}

inline bool IsSamplerYUVType(GLenum type)
{
    switch (type)
    {
        case GL_SAMPLER_EXTERNAL_2D_Y2Y_EXT:
            return true;

        default:
            return false;
    }
}

inline bool IsImageType(GLenum type)
{
    switch (type)
    {
        case GL_IMAGE_2D:
        case GL_INT_IMAGE_2D:
        case GL_UNSIGNED_INT_IMAGE_2D:
        case GL_IMAGE_3D:
        case GL_INT_IMAGE_3D:
        case GL_UNSIGNED_INT_IMAGE_3D:
        case GL_IMAGE_2D_ARRAY:
        case GL_INT_IMAGE_2D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
        case GL_IMAGE_CUBE_MAP_ARRAY:
        case GL_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_IMAGE_BUFFER:
        case GL_INT_IMAGE_BUFFER:
        case GL_UNSIGNED_INT_IMAGE_BUFFER:
        case GL_IMAGE_CUBE:
        case GL_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_IMAGE_CUBE:
            return true;
    }
    return false;
}

inline bool IsImage2DType(GLenum type)
{
    switch (type)
    {
        case GL_IMAGE_2D:
        case GL_INT_IMAGE_2D:
        case GL_UNSIGNED_INT_IMAGE_2D:
            return true;
        case GL_IMAGE_3D:
        case GL_INT_IMAGE_3D:
        case GL_UNSIGNED_INT_IMAGE_3D:
        case GL_IMAGE_2D_ARRAY:
        case GL_INT_IMAGE_2D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
        case GL_IMAGE_CUBE_MAP_ARRAY:
        case GL_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_IMAGE_CUBE:
        case GL_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_IMAGE_CUBE:
        case GL_IMAGE_BUFFER:
        case GL_INT_IMAGE_BUFFER:
        case GL_UNSIGNED_INT_IMAGE_BUFFER:
            return false;
        default:
            UNREACHABLE();
            return false;
    }
}

inline bool IsAtomicCounterType(GLenum type)
{
    return type == GL_UNSIGNED_INT_ATOMIC_COUNTER;
}

inline bool IsOpaqueType(GLenum type)
{
    // ESSL 3.10 section 4.1.7 defines opaque types as: samplers, images and atomic counters.
    return IsImageType(type) || IsSamplerType(type) || IsAtomicCounterType(type);
}

inline bool IsMatrixType(GLenum type)
{
    return VariableRowCount(type) > 1;
}

inline bool IsFloatScalarAndVectorType(GLenum type)
{
    switch (type)
    {
        case GL_FLOAT:
        case GL_FLOAT_VEC2:
        case GL_FLOAT_VEC3:
        case GL_FLOAT_VEC4:
            return true;
        default:
            return false;
    }
}

inline bool IsFloatVectorType(GLenum type)
{
    switch (type)
    {
        case GL_FLOAT_VEC2:
        case GL_FLOAT_VEC3:
        case GL_FLOAT_VEC4:
            return true;
        default:
            return false;
    }
}

inline GLenum TransposeMatrixType(GLenum type)
{
    if (!IsMatrixType(type))
    {
        return type;
    }

    switch (type)
    {
        case GL_FLOAT_MAT2:
            return GL_FLOAT_MAT2;
        case GL_FLOAT_MAT3:
            return GL_FLOAT_MAT3;
        case GL_FLOAT_MAT4:
            return GL_FLOAT_MAT4;
        case GL_FLOAT_MAT2x3:
            return GL_FLOAT_MAT3x2;
        case GL_FLOAT_MAT3x2:
            return GL_FLOAT_MAT2x3;
        case GL_FLOAT_MAT2x4:
            return GL_FLOAT_MAT4x2;
        case GL_FLOAT_MAT4x2:
            return GL_FLOAT_MAT2x4;
        case GL_FLOAT_MAT3x4:
            return GL_FLOAT_MAT4x3;
        case GL_FLOAT_MAT4x3:
            return GL_FLOAT_MAT3x4;
        default:
            UNREACHABLE();
            return GL_NONE;
    }
}

inline int MatrixRegisterCount(GLenum type, bool isRowMajorMatrix)
{
    ASSERT(IsMatrixType(type));
    return isRowMajorMatrix ? VariableRowCount(type) : VariableColumnCount(type);
}

inline int MatrixComponentCount(GLenum type, bool isRowMajorMatrix)
{
    ASSERT(IsMatrixType(type));
    return isRowMajorMatrix ? VariableColumnCount(type) : VariableRowCount(type);
}

inline int VariableRegisterCount(GLenum type)
{
    return IsMatrixType(type) ? VariableColumnCount(type) : 1;
}

inline int VariableSortOrder(GLenum type)
{
    switch (type)
    {
        // 1. Arrays of mat4 and mat4
        // Non-square matrices of type matCxR consume the same space as a square
        // matrix of type matN where N is the greater of C and R
        case GL_FLOAT_MAT4:
        case GL_FLOAT_MAT2x4:
        case GL_FLOAT_MAT3x4:
        case GL_FLOAT_MAT4x2:
        case GL_FLOAT_MAT4x3:
            return 0;

        // 2. Arrays of mat2 and mat2 (since they occupy full rows)
        case GL_FLOAT_MAT2:
            return 1;

        // 3. Arrays of vec4 and vec4
        case GL_FLOAT_VEC4:
        case GL_INT_VEC4:
        case GL_BOOL_VEC4:
        case GL_UNSIGNED_INT_VEC4:
            return 2;

        // 4. Arrays of mat3 and mat3
        case GL_FLOAT_MAT3:
        case GL_FLOAT_MAT2x3:
        case GL_FLOAT_MAT3x2:
            return 3;

        // 5. Arrays of vec3 and vec3
        case GL_FLOAT_VEC3:
        case GL_INT_VEC3:
        case GL_BOOL_VEC3:
        case GL_UNSIGNED_INT_VEC3:
            return 4;

        // 6. Arrays of vec2 and vec2
        case GL_FLOAT_VEC2:
        case GL_INT_VEC2:
        case GL_BOOL_VEC2:
        case GL_UNSIGNED_INT_VEC2:
            return 5;

        // 7. Single component types
        case GL_FLOAT:
        case GL_INT:
        case GL_BOOL:
        case GL_UNSIGNED_INT:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_EXTERNAL_OES:
        case GL_SAMPLER_2D_RECT_ANGLE:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_3D:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_IMAGE_2D:
        case GL_INT_IMAGE_2D:
        case GL_UNSIGNED_INT_IMAGE_2D:
        case GL_IMAGE_3D:
        case GL_INT_IMAGE_3D:
        case GL_UNSIGNED_INT_IMAGE_3D:
        case GL_IMAGE_2D_ARRAY:
        case GL_INT_IMAGE_2D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
        case GL_IMAGE_CUBE:
        case GL_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_ATOMIC_COUNTER:
        case GL_SAMPLER_EXTERNAL_2D_Y2Y_EXT:
            return 6;

        default:
            UNREACHABLE();
            return 0;
    }
}

inline bool IsBuiltInName(const char *name)
{
    return angle::BeginsWith(name, "gl_");
}

inline bool IsBuiltInName(const std::string &name)
{
    return IsBuiltInName(name.c_str());
}

inline GLuint GetPrimitiveRestartIndex(DrawElementsType indexType)
{
    switch (indexType)
    {
        case DrawElementsType::UnsignedByte:
            return 0xFF;
        case DrawElementsType::UnsignedShort:
            return 0xFFFF;
        case DrawElementsType::UnsignedInt:
            return 0xFFFFFFFF;
        default:
            UNREACHABLE();
            return 0;
    }
}

inline bool IsTriangleMode(PrimitiveMode drawMode)
{
    switch (drawMode)
    {
        case PrimitiveMode::Triangles:
        case PrimitiveMode::TriangleFan:
        case PrimitiveMode::TriangleStrip:
            return true;
        case PrimitiveMode::Points:
        case PrimitiveMode::Lines:
        case PrimitiveMode::LineLoop:
        case PrimitiveMode::LineStrip:
            return false;
        default:
            UNREACHABLE();
    }

    return false;
}

inline bool IsPolygonMode(PrimitiveMode mode)
{
    switch (mode)
    {
        case PrimitiveMode::Points:
        case PrimitiveMode::Lines:
        case PrimitiveMode::LineStrip:
        case PrimitiveMode::LineLoop:
        case PrimitiveMode::LinesAdjacency:
        case PrimitiveMode::LineStripAdjacency:
            return false;
        default:
            break;
    }

    return true;
}

inline bool IsLineMode(PrimitiveMode primitiveMode)
{
    return priv::gLineModes[primitiveMode];
}

inline bool IsIntegerFormat(GLenum unsizedFormat)
{
    switch (unsizedFormat)
    {
        case GL_RGBA_INTEGER:
        case GL_RGB_INTEGER:
        case GL_RG_INTEGER:
        case GL_RED_INTEGER:
            return true;

        default:
            return false;
    }
}

inline unsigned int ArraySizeProduct(const std::vector<unsigned int> &arraySizes)
{
    unsigned int arraySizeProduct = 1u;
    for (unsigned int arraySize : arraySizes)
    {
        arraySizeProduct *= arraySize;
    }
    return arraySizeProduct;
}

inline unsigned int InnerArraySizeProduct(const std::vector<unsigned int> &arraySizes)
{
    unsigned int arraySizeProduct = 1u;
    for (size_t index = 0; index + 1 < arraySizes.size(); ++index)
    {
        arraySizeProduct *= arraySizes[index];
    }
    return arraySizeProduct;
}

inline unsigned int OutermostArraySize(const std::vector<unsigned int> &arraySizes)
{
    return arraySizes.empty() || arraySizes.back() == 0 ? 1 : arraySizes.back();
}

inline unsigned int ElementTypeSize(GLenum elementType)
{
    switch (elementType)
    {
        case GL_UNSIGNED_BYTE:
            return sizeof(GLubyte);
        case GL_UNSIGNED_SHORT:
            return sizeof(GLushort);
        case GL_UNSIGNED_INT:
            return sizeof(GLuint);
        default:
            UNREACHABLE();
            return 0;
    }
}

inline bool IsMipmapFiltered(GLenum minFilterMode)
{
    switch (minFilterMode)
    {
        case GL_NEAREST:
        case GL_LINEAR:
            return false;
        case GL_NEAREST_MIPMAP_NEAREST:
        case GL_LINEAR_MIPMAP_NEAREST:
        case GL_NEAREST_MIPMAP_LINEAR:
        case GL_LINEAR_MIPMAP_LINEAR:
            return true;
        default:
            UNREACHABLE();
            return false;
    }
}

inline PipelineType GetPipelineType(ShaderType type)
{
    switch (type)
    {
        case ShaderType::Vertex:
        case ShaderType::Fragment:
        case ShaderType::Geometry:
            return PipelineType::GraphicsPipeline;
        case ShaderType::Compute:
            return PipelineType::ComputePipeline;
        default:
            UNREACHABLE();
            return PipelineType::GraphicsPipeline;
    }
}

inline ShaderType GetShaderTypeFromBitfield(size_t singleShaderType)
{
    switch (singleShaderType)
    {
        case GL_VERTEX_SHADER_BIT:
            return ShaderType::Vertex;
        case GL_FRAGMENT_SHADER_BIT:
            return ShaderType::Fragment;
        case GL_COMPUTE_SHADER_BIT:
            return ShaderType::Compute;
        case GL_GEOMETRY_SHADER_BIT:
            return ShaderType::Geometry;
        case GL_TESS_CONTROL_SHADER_BIT:
            return ShaderType::TessControl;
        case GL_TESS_EVALUATION_SHADER_BIT:
            return ShaderType::TessEvaluation;
        default:
            return ShaderType::InvalidEnum;
    }
}

inline GLbitfield GetBitfieldFromShaderType(ShaderType shaderType)
{
    switch (shaderType)
    {
        case ShaderType::Vertex:
            return GL_VERTEX_SHADER_BIT;
        case ShaderType::Fragment:
            return GL_FRAGMENT_SHADER_BIT;
        case ShaderType::Compute:
            return GL_COMPUTE_SHADER_BIT;
        case ShaderType::Geometry:
            return GL_GEOMETRY_SHADER_BIT;
        case ShaderType::TessControl:
            return GL_TESS_CONTROL_SHADER_BIT;
        case ShaderType::TessEvaluation:
            return GL_TESS_EVALUATION_SHADER_BIT;
        default:
            UNREACHABLE();
            return GL_ZERO;
    }
}

inline bool ShaderTypeSupportsTransformFeedback(ShaderType shaderType)
{
    switch (shaderType)
    {
        case ShaderType::Vertex:
        case ShaderType::Geometry:
        case ShaderType::TessEvaluation:
            return true;
        default:
            return false;
    }
}

inline ShaderType GetLastPreFragmentStage(ShaderBitSet shaderTypes)
{
    shaderTypes.reset(ShaderType::Fragment);
    shaderTypes.reset(ShaderType::Compute);
    return shaderTypes.any() ? shaderTypes.last() : ShaderType::InvalidEnum;
}

}  // namespace gl

namespace egl
{
// For use with EGL_EXT_image_gl_colorspace
// An EGLImage can be created with attributes that override the color space of underlying image data
// when rendering to the image, or sampling from the image. The possible values are -
//  Default - EGLImage source's colorspace should be preserved
//  sRGB - EGLImage targets will assume sRGB colorspace
//  Linear - EGLImage targets will assume linear colorspace
enum class ImageColorspace
{
    Default = 0,
    SRGB,
    Linear
};

static const EGLenum FirstCubeMapTextureTarget = EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X_KHR;
static const EGLenum LastCubeMapTextureTarget  = EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_KHR;
bool IsCubeMapTextureTarget(EGLenum target);
size_t CubeMapTextureTargetToLayerIndex(EGLenum target);
EGLenum LayerIndexToCubeMapTextureTarget(size_t index);
bool IsTextureTarget(EGLenum target);
bool IsRenderbufferTarget(EGLenum target);
bool IsExternalImageTarget(EGLenum target);

const char *GetGenericErrorMessage(EGLint error);
}  // namespace egl

namespace egl_gl
{
GLuint EGLClientBufferToGLObjectHandle(EGLClientBuffer buffer);
}

namespace gl_egl
{
EGLenum GLComponentTypeToEGLColorComponentType(GLenum glComponentType);
EGLClientBuffer GLObjectHandleToEGLClientBuffer(GLuint handle);
}  // namespace gl_egl

namespace angle
{

// All state that modify attachment's colorspace
struct ColorspaceState
{
  public:
    ColorspaceState() { reset(); }
    void reset()
    {
        hasStaticTexelFetchAccess = false;
        srgbDecode                = gl::SrgbDecode::Default;
        srgbOverride              = gl::SrgbOverride::Default;
        srgbWriteControl          = gl::SrgbWriteControlMode::Default;
        eglImageColorspace        = egl::ImageColorspace::Default;
    }

    // States that affect read operations
    bool hasStaticTexelFetchAccess;
    gl::SrgbDecode srgbDecode;
    gl::SrgbOverride srgbOverride;

    // States that affect write operations
    gl::SrgbWriteControlMode srgbWriteControl;

    // States that affect both read and write operations
    egl::ImageColorspace eglImageColorspace;
};

template <typename T>
constexpr size_t ConstStrLen(T s)
{
    if (s == nullptr)
    {
        return 0;
    }
    return std::char_traits<char>::length(s);
}

bool IsDrawEntryPoint(EntryPoint entryPoint);
bool IsDispatchEntryPoint(EntryPoint entryPoint);
bool IsClearEntryPoint(EntryPoint entryPoint);
bool IsQueryEntryPoint(EntryPoint entryPoint);

template <typename T>
void FillWithNullptr(T *array)
{
    // std::array::fill(nullptr) yields unoptimized, unrolled loop over array items
    ANGLE_UNSAFE_TODO(memset(array->data(), 0, array->size() * sizeof(*array->data())));
    // sanity check for non-0 nullptr
    ASSERT(array->data()[0] == nullptr);
}
}  // namespace angle

void writeFile(const char *path, std::string_view content);

// Get the underlying type. Useful for indexing into arrays with enum values by avoiding the clutter
// of the extraneous static_cast<>() calls.
// https://stackoverflow.com/a/8357462
template <typename E>
constexpr typename std::underlying_type<E>::type ToUnderlying(E e) noexcept
{
    return static_cast<typename std::underlying_type<E>::type>(e);
}

#endif  // COMMON_UTILITIES_H_
