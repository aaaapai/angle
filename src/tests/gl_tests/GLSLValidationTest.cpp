//
// Copyright 2025 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#ifdef UNSAFE_BUFFERS_BUILD
#    pragma allow_unsafe_buffers
#endif

#include "test_utils/CompilerTest.h"

#include "test_utils/angle_test_configs.h"
#include "test_utils/gl_raii.h"

using namespace angle;

namespace
{
class GLSLValidationTest : public CompilerTest
{
  protected:
    GLSLValidationTest() {}

    // Helper to create a shader, then verify that it fails to compile with the given reason.  It's
    // given:
    //
    // * The type of shader.
    // * The shader source itself.
    // * An error string to look for in the compile logs.
    void validateError(GLenum shaderType, const char *shaderSource, const char *expectedError)
    {
        const CompiledShader &shader = compile(shaderType, shaderSource);
        EXPECT_FALSE(shader.success());

        EXPECT_TRUE(shader.hasError(expectedError)) << expectedError;
        reset();
    }

    // Helper to create a shader, then verify that compilation succeeded.
    void validateSuccess(GLenum shaderType, const char *shaderSource)
    {
        const CompiledShader &shader = compile(shaderType, shaderSource);
        EXPECT_TRUE(shader.success());
        reset();
    }
};

class GLSLValidationTest_ES3 : public GLSLValidationTest
{};

class GLSLValidationTest_ES31 : public GLSLValidationTest
{};

class GLSLValidationTestNoValidation : public GLSLValidationTest
{
  public:
    GLSLValidationTestNoValidation() { setNoErrorEnabled(true); }
};

class WebGLGLSLValidationTest : public GLSLValidationTest
{
  protected:
    WebGLGLSLValidationTest() { setWebGLCompatibilityEnabled(true); }
};

class WebGL2GLSLValidationTest : public GLSLValidationTest_ES3
{
  protected:
    WebGL2GLSLValidationTest() { setWebGLCompatibilityEnabled(true); }

    void testInfiniteLoop(const char *fs)
    {
        const CompiledShader &shader = compile(GL_FRAGMENT_SHADER, fs);
        if (getEGLWindow()->isFeatureEnabled(Feature::RejectWebglShadersWithUndefinedBehavior))
        {
            EXPECT_FALSE(shader.success());
        }
        else
        {
            EXPECT_TRUE(shader.success());
        }
        reset();
    }
};

// Test that an empty shader fails to compile
TEST_P(GLSLValidationTest, EmptyShader)
{
    constexpr char kFS[] = "";
    validateError(GL_FRAGMENT_SHADER, kFS, "syntax error");
}

// Test that a shader with no main in it fails to compile
TEST_P(GLSLValidationTest, MissingMain)
{
    constexpr char kFS[] = R"(precision mediump float;)";
    validateError(GL_FRAGMENT_SHADER, kFS, "Missing main()");
}

// Test that a shader with only a main prototype in it fails to compile
TEST_P(GLSLValidationTest, MainPrototypeOnly)
{
    constexpr char kFS[] = R"(precision mediump float;
void main();
)";
    validateError(GL_FRAGMENT_SHADER, kFS, "Missing main()");
}

// Test relational operations between bools is rejected.
TEST_P(GLSLValidationTest, BoolLessThan)
{
    constexpr char kFS[] = R"(uniform mediump vec4 u;
void main() {
  bool a = bool(u.x);
  bool b = bool(u.y);
  bool c = a < b;
  gl_FragColor = vec4(c, !c, c, !c);
}
)";
    validateError(GL_FRAGMENT_SHADER, kFS, "'<' : comparison operator not defined for booleans");
}

// Verify that using maximum size as atomic counter offset results in compilation failure.
TEST_P(GLSLValidationTest_ES31, CompileWithMaxAtomicCounterOffsetFails)
{
    GLint maxSize;
    glGetIntegerv(GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE, &maxSize);

    std::ostringstream fs;
    fs << R"(#version 310 es
layout(location = 0) out uvec4 color;
layout(binding = 0, offset = )"
       << maxSize << R"() uniform atomic_uint a_counter;
void main() {
color = uvec4(atomicCounterIncrement(a_counter));
})";
    validateError(
        GL_FRAGMENT_SHADER, fs.str().c_str(),
        "'atomic counter' : Offset must not exceed the maximum atomic counter buffer size");
}

// Check that having an invalid char after the "." doesn't cause an assert.
TEST_P(GLSLValidationTest, InvalidFieldFirstChar)
{
    const char kVS[] = "void main() {vec4 x; x.}";
    validateError(GL_VERTEX_SHADER, kVS, ": '}' : Illegal character at fieldname start");
}

// Tests that bad index expressions don't crash ANGLE's translator.
// http://anglebug.com/42266998
TEST_P(GLSLValidationTest, BadIndexBugVec)
{
    constexpr char kFS[] = R"(precision mediump float;
uniform vec4 uniformVec;
void main()
{
    gl_FragColor = vec4(uniformVec[int()]);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'constructor' : constructor does not have any arguments");
}

// Tests that bad index expressions don't crash ANGLE's translator.
// http://anglebug.com/42266998
TEST_P(GLSLValidationTest, BadIndexBugMat)
{
    constexpr char kFS[] = R"(precision mediump float;
uniform mat4 uniformMat;
void main()
{
    gl_FragColor = vec4(uniformMat[int()]);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'constructor' : constructor does not have any arguments");
}

// Tests that bad index expressions don't crash ANGLE's translator.
// http://anglebug.com/42266998
TEST_P(GLSLValidationTest, BadIndexBugArray)
{
    constexpr char kFS[] = R"(precision mediump float;
uniform vec4 uniformArray;
void main()
{
    gl_FragColor = vec4(uniformArray[int()]);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'constructor' : constructor does not have any arguments");
}

// Test that GLSL error on gl_DepthRange does not crash.
TEST_P(GLSLValidationTestNoValidation, DepthRangeError)
{
    constexpr char kFS[] = R"(precision mediump float;
void main()
{
    gl_DepthRange + 1;
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'+' : Invalid operation for structs");
}

// Test that an inout value in a location beyond the MaxDrawBuffer limit when using the shader
// framebuffer fetch extension results in a compilation error.
// (Based on a fuzzer-discovered issue)
TEST_P(GLSLValidationTest_ES3, CompileFSWithInoutLocBeyondMaxDrawBuffers)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_shader_framebuffer_fetch"));

    GLint maxDrawBuffers;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);

    const std::string fs = R"(#version 300 es
#extension GL_EXT_shader_framebuffer_fetch : require
precision highp float;
layout(location = )" + std::to_string(maxDrawBuffers) +
                           R"() inout vec4 inoutArray[1];
void main()
{
    vec4 val = inoutArray[0];
    inoutArray[0] = val + vec4(0.1, 0.2, 0.3, 0.4);
})";
    validateError(GL_FRAGMENT_SHADER, fs.c_str(),
                  "'inoutArray' : output location must be < MAX_DRAW_BUFFERS");
}

// Test that structs with samplers are not allowed in interface blocks.  This is forbidden per
// GLES3:
//
// > Types and declarators are the same as for other uniform variable declarations outside blocks,
// > with these exceptions:
// > * opaque types are not allowed
TEST_P(GLSLValidationTest_ES3, StructWithSamplersDisallowedInInterfaceBlock)
{
    const char kFS[] = R"(#version 300 es
precision mediump float;
struct S { sampler2D samp; bool b; };

layout(std140) uniform Buffer { S s; } buffer;

out vec4 color;

void main()
{
    color = texture(buffer.s.samp, vec2(0));
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'Buffer' : Opaque types are not allowed in interface blocks");
}

// Test that *= on boolean vectors fails compilation
TEST_P(GLSLValidationTest, BVecMultiplyAssign)
{
    constexpr char kFS[] = R"(bvec4 c,s;void main(){s*=c;})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'assign' : cannot convert from '4-component vector of bool' to '4-component "
                  "vector of bool'");
}

// Test that packing of excessive 3-column variables does not overflow the count of 3-column
// variables in VariablePacker
TEST_P(WebGL2GLSLValidationTest, ExcessiveMat3UniformPacking)
{
    std::ostringstream vs;

    vs << R"(#version 300 es
precision mediump float;
out vec4 finalColor;
in vec4 color;
uniform mat4 r[254];

uniform mat3 )";

    constexpr size_t kNumUniforms = 10000;
    for (size_t i = 0; i < kNumUniforms; ++i)
    {
        if (i > 0)
        {
            vs << ", ";
        }
        vs << "m3a_" << i << "[256]";
    }
    vs << R"(;
void main(void) { finalColor = color; })";
    validateError(GL_VERTEX_SHADER, vs.str().c_str(), "too many uniforms");
}

// Test that infinite loop with while(true) is rejected
TEST_P(WebGL2GLSLValidationTest, InfiniteLoopWhileTrue)
{
    testInfiniteLoop(R"(#version 300 es
precision highp float;
uniform uint zero;
out vec4 color;

void main()
{
    float r = 0.;
    float g = 1.;
    float b = 0.;

    // Infinite loop
    while (true)
    {
        r += 0.1;
        if (r > 0.)
        {
            continue;
        }
    }

    color = vec4(r, g, b, 1);
})");
}

// Test that infinite loop with for(;true;) is rejected
TEST_P(WebGL2GLSLValidationTest, InfiniteLoopForTrue)
{
    testInfiniteLoop(R"(#version 300 es
precision highp float;
uniform uint zero;
out vec4 color;

void main()
{
    float r = 0.;
    float g = 1.;
    float b = 0.;

    // Infinite loop
    for (;!false;)
    {
        r += 0.1;
    }

    color = vec4(r, g, b, 1);
})");
}

// Test that infinite loop with do{} while(true) is rejected
TEST_P(WebGL2GLSLValidationTest, InfiniteLoopDoWhileTrue)
{
    testInfiniteLoop(R"(#version 300 es
precision highp float;
uniform uint zero;
out vec4 color;

void main()
{
    float r = 0.;
    float g = 1.;
    float b = 0.;

    // Infinite loop
    do
    {
        r += 0.1;
        switch (uint(r))
        {
            case 0:
                g += 0.1;
                break;
            default:
                b += 0.1;
                continue;
        }
    } while (true);

    color = vec4(r, g, b, 1);
})");
}

// Test that infinite loop with constant local variable is rejected
TEST_P(WebGL2GLSLValidationTest, InfiniteLoopLocalVariable)
{
    testInfiniteLoop(R"(#version 300 es
precision highp float;
uniform uint zero;
out vec4 color;

void main()
{
    float r = 0.;
    float g = 1.;
    float b = 0.;

    bool localConstTrue = true;

    // Infinite loop
    do
    {
        r += 0.1;
        switch (uint(r))
        {
            case 0:
                g += 0.1;
                break;
            default:
                b += 0.1;
                continue;
        }
    } while (localConstTrue);

    color = vec4(r, g, b, 1);
})");
}

// Test that infinite loop with global variable is rejected
TEST_P(WebGL2GLSLValidationTest, InfiniteLoopGlobalVariable)
{
    testInfiniteLoop(R"(#version 300 es
precision highp float;
uniform uint zero;
out vec4 color;

bool globalConstTrue = true;

void main()
{
    float r = 0.;
    float g = 1.;
    float b = 0.;

    // Infinite loop
    do
    {
        r += 0.1;
        switch (uint(r))
        {
            case 0:
                g += 0.1;
                break;
            default:
                b += 0.1;
                continue;
        }
    } while (globalConstTrue);

    color = vec4(r, g, b, 1);
})");
}

// Test that indexing swizzles out of bounds fails
TEST_P(GLSLValidationTest_ES3, OutOfBoundsIndexingOfSwizzle)
{
    constexpr char kFS[] = R"(#version 300 es
precision mediump float;
out vec4 colorOut;
uniform vec3 colorIn;

void main()
{
    colorOut = vec4(colorIn.yx[2], 0, 0, 1);
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'[]' : vector field selection out of range");
}

// Regression test for a validation bug in the translator where func(void, int) was accepted even
// though it's illegal, and the function was callable as if the void parameter isn't there.
TEST_P(GLSLValidationTest, NoParameterAfterVoid)
{
    constexpr char kVS[] = R"(void f(void, int a){}
void main(){f(1);})";
    validateError(GL_VERTEX_SHADER, kVS, "'void' : cannot be a parameter type except for '(void)'");
}

// Similar to NoParameterAfterVoid, but tests func(void, void).
TEST_P(GLSLValidationTest, NoParameterAfterVoid2)
{
    constexpr char kVS[] = R"(void f(void, void){}
void main(){f();})";
    validateError(GL_VERTEX_SHADER, kVS, "'void' : cannot be a parameter type except for '(void)'");
}

// Test that structs with too many fields are rejected.  In SPIR-V, the instruction that defines the
// struct lists the fields which means the length of the instruction is a function of the field
// count.  Since SPIR-V instruction sizes are limited to 16 bits, structs with more fields cannot be
// represented.
TEST_P(GLSLValidationTest_ES3, TooManyFieldsInStruct)
{
    std::ostringstream fs;
    fs << R"(#version 300 es
precision highp float;
struct TooManyFields
{
)";
    for (uint32_t i = 0; i < (1 << 16); ++i)
    {
        fs << "    float field" << i << ";\n";
    }
    fs << R"(};
uniform B { TooManyFields s; };
out vec4 color;
void main() {
    color = vec4(s.field0, 0.0, 0.0, 1.0);
})";
    validateError(GL_FRAGMENT_SHADER, fs.str().c_str(),
                  "'TooManyFields' : Too many fields in the struct");
}

// Same as TooManyFieldsInStruct, but with samplers in the struct.
TEST_P(GLSLValidationTest_ES3, TooManySamplerFieldsInStruct)
{
    std::ostringstream fs;
    fs << R"(#version 300 es
precision highp float;
struct TooManyFields
{
)";
    for (uint32_t i = 0; i < (1 << 16); ++i)
    {
        fs << "    sampler2D field" << i << ";\n";
    }
    fs << R"(};
uniform TooManyFields s;
out vec4 color;
void main() {
    color = texture(s.field0, vec2(0));
})";
    validateError(GL_FRAGMENT_SHADER, fs.str().c_str(),
                  "'TooManyFields' : Too many fields in the struct");
}

// Test having many samplers in nested structs.
TEST_P(GLSLValidationTest_ES3, ManySamplerFieldsInStructComplex)
{
    // D3D and OpenGL may be more restrictive about this many samplers.
    ANGLE_SKIP_TEST_IF(IsD3D() || IsOpenGL());

    const char kFS[] = R"(#version 300 es
precision highp float;

struct X {
    mediump sampler2D a[0xf00];
    mediump sampler2D b[0xf00];
    mediump sampler2D c[0xf000];
    mediump sampler2D d[0xf00];
};

struct Y {
  X s1;
  mediump sampler2D a[0xf00];
  mediump sampler2D b[0xf000];
  mediump sampler2D c[0x14000];
};

struct S {
    Y s1;
};

struct structBuffer { S s; };

uniform structBuffer b;

out vec4 color;
void main()
{
    color = texture(b.s.s1.s1.c[0], vec2(0));
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Make sure a large array of samplers works.
TEST_P(GLSLValidationTest, ManySamplers)
{
    // D3D and OpenGL may be more restrictive about this many samplers.
    ANGLE_SKIP_TEST_IF(IsD3D() || IsOpenGL());

    const char kFS[] = R"(precision highp float;

uniform mediump sampler2D c[0x12000];

void main()
{
    gl_FragColor = texture2D(c[0], vec2(0));
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Make sure a large array of samplers works when declared in a struct.
TEST_P(GLSLValidationTest, ManySamplersInStruct)
{
    // D3D and OpenGL may be more restrictive about this many samplers.
    ANGLE_SKIP_TEST_IF(IsD3D() || IsOpenGL());

    const char kFS[] = R"(precision highp float;

struct X {
    mediump sampler2D c[0x12000];
};

uniform X x;

void main()
{
    gl_FragColor = texture2D(x.c[0], vec2(0));
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that passing large arrays to functions are compiled correctly.  Regression test for the
// SPIR-V generator that made a copy of the array to pass to the function, by decomposing and
// reconstructing it (in the absence of OpCopyLogical), but the reconstruction instruction has a
// length higher than can fit in SPIR-V.
TEST_P(GLSLValidationTest_ES3, LargeInterfaceBlockArrayPassedToFunction)
{
    constexpr char kFS[] = R"(#version 300 es
precision highp float;
uniform Large { float a[65536]; };
float f(float b[65536])
{
    b[0] = 1.0;
    return b[0] + b[1];
}
out vec4 color;
void main() {
    color = vec4(f(a), 0.0, 0.0, 1.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'b' : Size of declared private variable exceeds implementation-defined limit");
}

// Similar to LargeInterfaceBlockArrayPassedToFunction, but the array is nested in a struct.
TEST_P(GLSLValidationTest_ES3, LargeInterfaceBlockNestedArrayPassedToFunction)
{
    constexpr char kFS[] = R"(#version 300 es
precision highp float;
struct S { float a[65536]; };
uniform Large { S s; };
float f(float b[65536])
{
    b[0] = 1.0;
    return b[0] + b[1];
}
out vec4 color;
void main() {
    color = vec4(f(s.a), 0.0, 0.0, 1.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'b' : Size of declared private variable exceeds implementation-defined limit");
}

// Similar to LargeInterfaceBlockArrayPassedToFunction, but the large array is copied to a local
// variable instead.
TEST_P(GLSLValidationTest_ES3, LargeInterfaceBlockArrayCopiedToLocal)
{
    constexpr char kFS[] = R"(#version 300 es
precision highp float;
uniform Large { float a[65536]; };
out vec4 color;
void main() {
    float b[65536] = a;
    color = vec4(b[0], 0.0, 0.0, 1.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'b' : Size of declared private variable exceeds implementation-defined limit");
}

// Similar to LargeInterfaceBlockArrayCopiedToLocal, but the array is nested in a struct
TEST_P(GLSLValidationTest_ES3, LargeInterfaceBlockNestedArrayCopiedToLocal)
{
    constexpr char kFS[] = R"(#version 300 es
precision highp float;
struct S { float a[65536]; };
uniform Large { S s; };
out vec4 color;
void main() {
    S s2 = s;
    color = vec4(s2.a[0], 0.0, 0.0, 1.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'s2' : Size of declared private variable exceeds implementation-defined limit");
}

// Test that too large varyings are rejected.
TEST_P(GLSLValidationTest_ES3, LargeArrayVarying)
{
    constexpr char kFS[] = R"(#version 300 es
precision highp float;
in float a[65536];
out vec4 color;
void main() {
    color = vec4(a[0], 0.0, 0.0, 1.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'a' : Size of declared private variable exceeds implementation-defined limit");
}

// Test that too large color outputs are rejected
TEST_P(GLSLValidationTest_ES3, LargeColorOutput)
{
    GLint maxDrawBuffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    ANGLE_SKIP_TEST_IF(maxDrawBuffers >= 32);

    constexpr char kFS[] = R"(#version 300 es
precision mediump float;
out vec4 colorOut[32];

void main()
{
    colorOut[0] = vec4(0, 0, 0, 1);
    colorOut[31] = vec4(0, 0, 0, 1);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'colorOut' : output array locations would exceed MAX_DRAW_BUFFERS");
}

// Test that too large color outputs are rejected
TEST_P(GLSLValidationTest_ES3, LargeColorOutputWithLocation)
{
    GLint maxDrawBuffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    ANGLE_SKIP_TEST_IF(maxDrawBuffers >= 10);

    constexpr char kFS[] = R"(#version 300 es
precision mediump float;
layout(location = 0) out vec4 colorOut[4];
layout(location = 4) out vec4 colorOut2[6];

void main()
{
    colorOut[0] = vec4(0, 0, 0, 1);
    colorOut[3] = vec4(0, 0, 0, 1);
    colorOut2[0] = vec4(0, 0, 0, 1);
    colorOut2[5] = vec4(0, 0, 0, 1);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'colorOut2' : output array locations would exceed MAX_DRAW_BUFFERS");
}

// Test that marking a built-in as invariant and then redeclaring it is an error.
TEST_P(GLSLValidationTest_ES3, FragDepthInvariantThenRedeclare)
{
    constexpr char kFS[] = R"(#version 300 es
#extension GL_EXT_conservative_depth:enable
precision mediump float;
invariant gl_FragDepth;
out float gl_FragDepth;
void main() {
    gl_FragDepth = 0.5;
}
)";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'gl_FragDepth' : built-ins cannot be redeclared after being qualified as "
                  "invariant or precise");
}

// Make sure gl_PerVertex is not accepted other than as `out` and with no name in vertex shader
TEST_P(GLSLValidationTest_ES31, ValidatePerVertexVertexShader)
{
    {
        // Cannot use gl_PerVertex with attribute
        constexpr char kVS[] = R"(attribute gl_PerVertex{vec4 gl_Position;};
void main() {})";
        validateError(GL_VERTEX_SHADER, kVS,
                      "'gl_PerVertex' : interface blocks supported in GLSL ES 3.00 and above only");
    }

    {
        // Cannot use gl_PerVertex with a name (without EXT_shader_io_blocks)
        constexpr char kVS[] = R"(#version 300 es
out gl_PerVertex{vec4 gl_Position;} name;
void main() {})";
        validateError(GL_VERTEX_SHADER, kVS,
                      "'out' : invalid qualifier: interface blocks must be uniform in version "
                      "lower than GLSL ES 3.10");
    }

    {
        // Cannot use gl_PerVertex (without EXT_shader_io_blocks)
        constexpr char kVS[] = R"(#version 310 es
out gl_PerVertex{vec4 gl_Position;};
void main() {})";
        validateError(GL_VERTEX_SHADER, kVS,
                      "'out' : invalid qualifier: shader IO blocks need shader io block extension");
    }

    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_shader_io_blocks"));

    {
        // Cannot use gl_PerVertex with a name
        constexpr char kVS[] = R"(#version 310 es
#extension GL_EXT_shader_io_blocks : require
out gl_PerVertex{vec4 gl_Position;} name;
void main() {})";
        validateError(GL_VERTEX_SHADER, kVS,
                      "'name' : out gl_PerVertex instance name must be empty in this shader");
    }

    {
        // out gl_PerVertex without a name is ok.
        constexpr char kVS[] = R"(#version 310 es
#extension GL_EXT_shader_io_blocks : require
out gl_PerVertex{vec4 gl_Position;};
void main() {})";
        validateSuccess(GL_VERTEX_SHADER, kVS);
    }
}

// Make sure gl_PerVertex is not accepted other than as `out .. gl_out[]`, or `in .. gl_in[]` in
// tessellation control shader.
TEST_P(GLSLValidationTest_ES31, ValidatePerVertexTessellationControlShader)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_tessellation_shader"));

    {
        // Cannot use out gl_PerVertex with a name (without EXT_shader_io_blocks)
        constexpr char kTCS[] = R"(#version 300 es
out gl_PerVertex{vec4 gl_Position;} name[];
void main() {})";
        validateError(GL_TESS_CONTROL_SHADER, kTCS,
                      "'out' : invalid qualifier: interface blocks must be uniform in version "
                      "lower than GLSL ES 3.10");
    }

    {
        // Cannot use in gl_PerVertex with a name (without EXT_shader_io_blocks)
        constexpr char kTCS[] = R"(#version 300 es
in gl_PerVertex{vec4 gl_Position;} name[];
void main() {})";
        validateError(GL_TESS_CONTROL_SHADER, kTCS,
                      "'in' : invalid qualifier: interface blocks must be uniform in version lower "
                      "than GLSL ES 3.10");
    }

    {
        // Cannot use out gl_PerVertex (without EXT_shader_io_blocks)
        constexpr char kTCS[] = R"(#version 310 es
out gl_PerVertex{vec4 gl_Position;} gl_out[];
void main() {})";
        validateError(GL_TESS_CONTROL_SHADER, kTCS,
                      "'out' : invalid qualifier: shader IO blocks need shader io block extension");
    }

    {
        // Cannot use in gl_PerVertex (without EXT_shader_io_blocks)
        constexpr char kTCS[] = R"(#version 310 es
in gl_PerVertex{vec4 gl_Position;} gl_in[];
void main() {})";
        validateError(GL_TESS_CONTROL_SHADER, kTCS,
                      "'in' : invalid qualifier: shader IO blocks need shader io block extension");
    }

    {
        // Cannot use out gl_PerVertex with a name
        constexpr char kTCS[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (vertices=4) out;
out gl_PerVertex{vec4 gl_Position;} name[];
void main() {})";
        validateError(GL_TESS_CONTROL_SHADER, kTCS,
                      "'name' : out gl_PerVertex instance name must be gl_out in this shader");
    }

    {
        // Cannot use in gl_PerVertex with a name
        constexpr char kTCS[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (vertices=4) out;
in gl_PerVertex{vec4 gl_Position;} name[];
void main() {})";
        validateError(GL_TESS_CONTROL_SHADER, kTCS,
                      "'name' : in gl_PerVertex instance name must be gl_in");
    }

    {
        // Cannot use out gl_PerVertex if not arrayed
        constexpr char kTCS[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (vertices=4) out;
out gl_PerVertex{vec4 gl_Position;} gl_out;
void main() {})";
        validateError(GL_TESS_CONTROL_SHADER, kTCS, "'gl_PerVertex' : type must be an array");
    }

    {
        // Cannot use in gl_PerVertex if not arrayed
        constexpr char kTCS[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (vertices=4) out;
in gl_PerVertex{vec4 gl_Position;} gl_in;
void main() {})";
        validateError(GL_TESS_CONTROL_SHADER, kTCS, "'gl_PerVertex' : type must be an array");
    }

    {
        // out gl_PerVertex with gl_out, and in gl_PerVertex with gl_in are ok.
        constexpr char kTCS[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (vertices=4) out;
out gl_PerVertex{vec4 gl_Position;} gl_out[];
in gl_PerVertex{vec4 gl_Position;} gl_in[];
void main() {})";
        validateSuccess(GL_TESS_CONTROL_SHADER, kTCS);
    }
}

// Make sure gl_PerVertex is not accepted other than as `out .. gl_out`, or `in .. gl_in[]` in
// tessellation evaluation shader.
TEST_P(GLSLValidationTest_ES31, ValidatePerVertexTessellationEvaluationShader)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_tessellation_shader"));

    {
        // Cannot use out gl_PerVertex with a name (without EXT_shader_io_blocks)
        constexpr char kTES[] = R"(#version 300 es
out gl_PerVertex{vec4 gl_Position;} name;
void main() {})";
        validateError(GL_TESS_EVALUATION_SHADER, kTES,
                      "'out' : invalid qualifier: interface blocks must be uniform in version "
                      "lower than GLSL ES 3.10");
    }

    {
        // Cannot use in gl_PerVertex with a name (without EXT_shader_io_blocks)
        constexpr char kTES[] = R"(#version 300 es
in gl_PerVertex{vec4 gl_Position;} name[];
void main() {})";
        validateError(GL_TESS_EVALUATION_SHADER, kTES,
                      "'in' : invalid qualifier: interface blocks must be uniform in version lower "
                      "than GLSL ES 3.10");
    }

    {
        // Cannot use out gl_PerVertex (without EXT_shader_io_blocks)
        constexpr char kTES[] = R"(#version 310 es
out gl_PerVertex{vec4 gl_Position;};
void main() {})";
        validateError(GL_TESS_EVALUATION_SHADER, kTES,
                      "'out' : invalid qualifier: shader IO blocks need shader io block extension");
    }

    {
        // Cannot use in gl_PerVertex (without EXT_shader_io_blocks)
        constexpr char kTES[] = R"(#version 310 es
in gl_PerVertex{vec4 gl_Position;} gl_in[];
void main() {})";
        validateError(GL_TESS_EVALUATION_SHADER, kTES,
                      "'in' : invalid qualifier: shader IO blocks need shader io block extension");
    }

    {
        // Cannot use out gl_PerVertex with a name
        constexpr char kTES[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (isolines, point_mode) in;
out gl_PerVertex{vec4 gl_Position;} name;
void main() {})";
        validateError(GL_TESS_EVALUATION_SHADER, kTES,
                      "'name' : out gl_PerVertex instance name must be empty in this shader");
    }

    {
        // Cannot use in gl_PerVertex with a name
        constexpr char kTES[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (isolines, point_mode) in;
in gl_PerVertex{vec4 gl_Position;} name[];
void main() {})";
        validateError(GL_TESS_EVALUATION_SHADER, kTES,
                      "'name' : in gl_PerVertex instance name must be gl_in");
    }

    {
        // Cannot use out gl_PerVertex if arrayed
        constexpr char kTES[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (isolines, point_mode) in;
out gl_PerVertex{vec4 gl_Position;} gl_out[];
void main() {})";
        validateError(GL_TESS_EVALUATION_SHADER, kTES,
                      "'gl_out' : out gl_PerVertex instance name must be empty in this shader");
    }

    {
        // Cannot use in gl_PerVertex if not arrayed
        constexpr char kTES[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (isolines, point_mode) in;
in gl_PerVertex{vec4 gl_Position;} gl_in;
void main() {})";
        validateError(GL_TESS_EVALUATION_SHADER, kTES, "'gl_PerVertex' : type must be an array");
    }

    {
        // out gl_PerVertex without a name, and in gl_PerVertex with gl_in are ok.
        constexpr char kTES[] = R"(#version 310 es
#extension GL_EXT_tessellation_shader : require
layout (isolines, point_mode) in;
out gl_PerVertex{vec4 gl_Position;};
in gl_PerVertex{vec4 gl_Position;} gl_in[];
void main() {})";
        validateSuccess(GL_TESS_EVALUATION_SHADER, kTES);
    }
}

// Make sure gl_PerVertex is not accepted other than as `out .. gl_out`, or `in .. gl_in[]` in
// geometry shader.
TEST_P(GLSLValidationTest_ES31, ValidatePerVertexGeometryShader)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_geometry_shader"));

    {
        // Cannot use out gl_PerVertex with a name (without EXT_shader_io_blocks)
        constexpr char kGS[] = R"(#version 300 es
out gl_PerVertex{vec4 gl_Position;} name;
void main() {})";
        validateError(GL_GEOMETRY_SHADER, kGS,
                      "'out' : invalid qualifier: interface blocks must be uniform in version "
                      "lower than GLSL ES 3.10");
    }

    {
        // Cannot use in gl_PerVertex with a name (without EXT_shader_io_blocks)
        constexpr char kGS[] = R"(#version 300 es
in gl_PerVertex{vec4 gl_Position;} name[];
void main() {})";
        validateError(GL_GEOMETRY_SHADER, kGS,
                      "'in' : invalid qualifier: interface blocks must be uniform in version lower "
                      "than GLSL ES 3.10");
    }

    {
        // Cannot use out gl_PerVertex (without EXT_shader_io_blocks)
        constexpr char kGS[] = R"(#version 310 es
out gl_PerVertex{vec4 gl_Position;};
void main() {})";
        validateError(GL_GEOMETRY_SHADER, kGS,
                      "'out' : invalid qualifier: shader IO blocks need shader io block extension");
    }

    {
        // Cannot use in gl_PerVertex (without EXT_shader_io_blocks)
        constexpr char kGS[] = R"(#version 310 es
in gl_PerVertex{vec4 gl_Position;} gl_in[];
void main() {})";
        validateError(GL_GEOMETRY_SHADER, kGS,
                      "'in' : invalid qualifier: shader IO blocks need shader io block extension");
    }

    {
        // Cannot use out gl_PerVertex with a name
        constexpr char kGS[] = R"(#version 310 es
#extension GL_EXT_geometry_shader : require
layout (triangles) in;
layout (points, max_vertices = 1) out;
out gl_PerVertex{vec4 gl_Position;} name;
void main() {})";
        validateError(GL_GEOMETRY_SHADER, kGS,
                      "'name' : out gl_PerVertex instance name must be empty in this shader");
    }

    {
        // Cannot use in gl_PerVertex with a name
        constexpr char kGS[] = R"(#version 310 es
#extension GL_EXT_geometry_shader : require
layout (triangles) in;
layout (points, max_vertices = 1) out;
in gl_PerVertex{vec4 gl_Position;} name[];
void main() {})";
        validateError(GL_GEOMETRY_SHADER, kGS,
                      "'name' : in gl_PerVertex instance name must be gl_in");
    }

    {
        // Cannot use out gl_PerVertex if arrayed
        constexpr char kGS[] = R"(#version 310 es
#extension GL_EXT_geometry_shader : require
layout (triangles) in;
layout (points, max_vertices = 1) out;
out gl_PerVertex{vec4 gl_Position;} gl_out[];
void main() {})";
        validateError(GL_GEOMETRY_SHADER, kGS,
                      "'gl_out' : out gl_PerVertex instance name must be empty in this shader");
    }

    {
        // Cannot use in gl_PerVertex if not arrayed
        constexpr char kGS[] = R"(#version 310 es
#extension GL_EXT_geometry_shader : require
layout (triangles) in;
layout (points, max_vertices = 1) out;
in gl_PerVertex{vec4 gl_Position;} gl_in;
void main() {})";
        validateError(GL_GEOMETRY_SHADER, kGS, "'gl_PerVertex' : type must be an array");
    }

    {
        // out gl_PerVertex without a name, and in gl_PerVertex with gl_in are ok.
        constexpr char kGS[] = R"(#version 310 es
#extension GL_EXT_geometry_shader : require
layout (triangles) in;
layout (points, max_vertices = 1) out;
out gl_PerVertex{vec4 gl_Position;};
in gl_PerVertex{vec4 gl_Position;} gl_in[];
void main() {})";
        validateSuccess(GL_GEOMETRY_SHADER, kGS);
    }
}

// Regression test case of unary + constant folding of a void struct member.
TEST_P(GLSLValidationTest, UnaryPlusOnVoidStructMemory)
{
    constexpr char kFS[] = R"(uniform mediump vec4 u;
struct U
{
    void t;
};
void main() {
  +U().t;
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'t' : illegal use of type 'void'");
}

// Test compiling shaders using the GL_EXT_shader_texture_lod extension
TEST_P(GLSLValidationTest, TextureLOD)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_shader_texture_lod"));

    constexpr char kFS[] = R"(#extension GL_EXT_shader_texture_lod : require
uniform sampler2D u_texture;
void main() {
    gl_FragColor = texture2DGradEXT(u_texture, vec2(0.0, 0.0), vec2(0.0, 0.0), vec2(0.0, 0.0));
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Verify that using a struct as both invariant and non-invariant output works.
TEST_P(GLSLValidationTest_ES31, StructBothInvariantAndNot)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_shader_io_blocks"));

    constexpr char kVS[] = R"(#version 310 es
#extension GL_EXT_shader_io_blocks : require

struct S
{
    vec4 s;
};

out Output
{
    vec4 x;
    invariant S s;
};

out S s2;

void main(){
    x = vec4(0);
    s.s = vec4(1);
    s2.s = vec4(2);
    S s3 = s;
    s.s = s3.s;
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that using a struct as both invariant and non-invariant output works.
// The shader interface block has a variable name in this variant.
TEST_P(GLSLValidationTest_ES31, StructBothInvariantAndNot2)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_shader_io_blocks"));

    constexpr char kVS[] = R"(#version 310 es
#extension GL_EXT_shader_io_blocks : require

struct S
{
    vec4 s;
};

out Output
{
    vec4 x;
    invariant S s;
} o;

out S s2;

void main(){
    o.x = vec4(0);
    o.s.s = vec4(1);
    s2.s = vec4(2);
    S s3 = o.s;
    o.s.s = s3.s;
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest, MissingReturnFloat)
{
    constexpr char kVS[] = R"(varying float v_varying;
float f();
void main() { gl_Position = vec4(f(), 0, 0, 1); }
float f() { if (v_varying > 0.0) return 1.0; })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest, MissingReturnVec2)
{
    constexpr char kVS[] = R"(varying float v_varying;
vec2 f() { if (v_varying > 0.0) return vec2(1.0, 1.0); }
void main() { gl_Position = vec4(f().x, 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest, MissingReturnVec3)
{
    constexpr char kVS[] = R"(varying float v_varying;
vec3 f() { if (v_varying > 0.0) return vec3(1.0, 1.0, 1.0); }
void main() { gl_Position = vec4(f().x, 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest, MissingReturnVec4)
{
    constexpr char kVS[] = R"(varying float v_varying;
vec4 f() { if (v_varying > 0.0) return vec4(1.0, 1.0, 1.0, 1.0); }
void main() { gl_Position = vec4(f().x, 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest, MissingReturnIVec4)
{
    constexpr char kVS[] = R"(varying float v_varying;
ivec4 f() { if (v_varying > 0.0) return ivec4(1, 1, 1, 1); }
void main() { gl_Position = vec4(f().x, 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest, MissingReturnMat4)
{
    constexpr char kVS[] = R"(varying float v_varying;
mat4 f() { if (v_varying > 0.0) return mat4(1.0); }
void main() { gl_Position = vec4(f()[0][0], 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest, MissingReturnStruct)
{
    constexpr char kVS[] = R"(varying float v_varying;
struct s { float a; int b; vec2 c; };
s f() { if (v_varying > 0.0) return s(1.0, 1, vec2(1.0, 1.0)); }
void main() { gl_Position = vec4(f().a, 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest_ES3, MissingReturnArray)
{
    constexpr char kVS[] = R"(#version 300 es
in float v_varying;
vec2[2] f() { if (v_varying > 0.0) { return vec2[2](vec2(1.0, 1.0), vec2(1.0, 1.0)); } }
void main() { gl_Position = vec4(f()[0].x, 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest_ES3, MissingReturnArrayOfStructs)
{
    constexpr char kVS[] = R"(#version 300 es
in float v_varying;
struct s { float a; int b; vec2 c; };
s[2] f() { if (v_varying > 0.0) { return s[2](s(1.0, 1, vec2(1.0, 1.0)), s(1.0, 1, vec2(1.0, 1.0))); } }
void main() { gl_Position = vec4(f()[0].a, 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that functions without return statements still compile
TEST_P(GLSLValidationTest_ES3, MissingReturnStructOfArrays)
{
    // TODO(crbug.com/998505): Test failing on Android FYI Release (NVIDIA Shield TV)
    ANGLE_SKIP_TEST_IF(IsNVIDIAShield());

    constexpr char kVS[] = R"(#version 300 es
in float v_varying;
struct s { float a[2]; int b[2]; vec2 c[2]; };
s f() { if (v_varying > 0.0) { return s(float[2](1.0, 1.0), int[2](1, 1), vec2[2](vec2(1.0, 1.0), vec2(1.0, 1.0))); } }
void main() { gl_Position = vec4(f().a[0], 0, 0, 1); })";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify that non-const index used on an array returned by a function compiles
TEST_P(GLSLValidationTest_ES3, ReturnArrayOfStructsThenNonConstIndex)
{
    constexpr char kVS[] = R"(#version 300 es
in float v_varying;
struct s { float a; int b; vec2 c; };
s[2] f()
{
    return s[2](s(v_varying, 1, vec2(1.0, 1.0)), s(v_varying / 2.0, 1, vec2(1.0, 1.0)));
}
void main()
{
    gl_Position = vec4(f()[uint(v_varying)].a, 0, 0, 1);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Verify shader source with a fixed length that is less than the null-terminated length will
// compile.
TEST_P(GLSLValidationTest, FixedShaderLength)
{
    GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);

    const std::string appendGarbage = "abcdefghijklmnopqrstuvwxyz";
    const std::string source   = "void main() { gl_FragColor = vec4(0, 0, 0, 0); }" + appendGarbage;
    const char *sourceArray[1] = {source.c_str()};
    GLint lengths[1]           = {static_cast<GLint>(source.length() - appendGarbage.length())};
    glShaderSource(shader, static_cast<GLsizei>(ArraySize(sourceArray)), sourceArray, lengths);
    glCompileShader(shader);

    GLint compileResult;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileResult);
    EXPECT_NE(compileResult, 0);
}

// Verify that a negative shader source length is treated as a null-terminated length.
TEST_P(GLSLValidationTest, NegativeShaderLength)
{
    GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);

    const char *sourceArray[1] = {essl1_shaders::fs::Red()};
    GLint lengths[1]           = {-10};
    glShaderSource(shader, static_cast<GLsizei>(ArraySize(sourceArray)), sourceArray, lengths);
    glCompileShader(shader);

    GLint compileResult;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileResult);
    EXPECT_NE(compileResult, 0);
}

// Verify that a length array with mixed positive and negative values compiles.
TEST_P(GLSLValidationTest, MixedShaderLengths)
{
    GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);

    const char *sourceArray[] = {
        "void main()",
        "{",
        "    gl_FragColor = vec4(0, 0, 0, 0);",
        "}",
    };
    GLint lengths[] = {
        -10,
        1,
        static_cast<GLint>(strlen(sourceArray[2])),
        -1,
    };
    ASSERT_EQ(ArraySize(sourceArray), ArraySize(lengths));

    glShaderSource(shader, static_cast<GLsizei>(ArraySize(sourceArray)), sourceArray, lengths);
    glCompileShader(shader);

    GLint compileResult;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileResult);
    EXPECT_NE(compileResult, 0);
}

// Verify that zero-length shader source does not affect shader compilation.
TEST_P(GLSLValidationTest, ZeroShaderLength)
{
    GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);

    const char *sourceArray[] = {
        "abcdefg", "34534", "void main() { gl_FragColor = vec4(0, 0, 0, 0); }", "", "abcdefghijklm",
    };
    GLint lengths[] = {
        0, 0, -1, 0, 0,
    };
    ASSERT_EQ(ArraySize(sourceArray), ArraySize(lengths));

    glShaderSource(shader, static_cast<GLsizei>(ArraySize(sourceArray)), sourceArray, lengths);
    glCompileShader(shader);

    GLint compileResult;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileResult);
    EXPECT_NE(compileResult, 0);
}

// Test that structs defined in uniforms are translated correctly.
TEST_P(GLSLValidationTest, StructSpecifiersUniforms)
{
    constexpr char kFS[] = R"(precision mediump float;

uniform struct S { float field; } s;

void main()
{
    gl_FragColor = vec4(1, 0, 0, 1);
    gl_FragColor.a += s.field;
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that if a non-preprocessor token is seen in a disabled if-block then it does not disallow
// extension pragmas later
TEST_P(GLSLValidationTest, NonPreprocessorTokensInIfBlocks)
{
    constexpr const char *kFS = R"(
#if __VERSION__ >= 300
    inout mediump vec4 fragData;
#else
    #extension GL_EXT_shader_texture_lod :enable
#endif

void main()
{
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that two constructors which have vec4 and mat2 parameters get disambiguated (issue in
// HLSL).
TEST_P(GLSLValidationTest_ES3, AmbiguousConstructorCall2x2)
{
    constexpr char kVS[] = R"(#version 300 es
precision highp float;
in vec4 a_vec;
in mat2 a_mat;
void main()
{
    gl_Position = vec4(a_vec) + vec4(a_mat);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test that two constructors which have mat2x3 and mat3x2 parameters get disambiguated.
// This was suspected to be an issue in HLSL, but HLSL seems to be able to natively choose between
// the function signatures in this case.
TEST_P(GLSLValidationTest_ES3, AmbiguousConstructorCall2x3)
{
    constexpr char kVS[] = R"(#version 300 es
precision highp float;
in mat3x2 a_matA;
in mat2x3 a_matB;
void main()
{
    gl_Position = vec4(a_matA) + vec4(a_matB);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test that two functions which have vec4 and mat2 parameters get disambiguated (issue in HLSL).
TEST_P(GLSLValidationTest_ES3, AmbiguousFunctionCall2x2)
{
    constexpr char kVS[] = R"(#version 300 es
precision highp float;
in vec4 a_vec;
in mat2 a_mat;
vec4 foo(vec4 a)
{
    return a;
}
vec4 foo(mat2 a)
{
    return vec4(a[0][0]);
}
void main()
{
    gl_Position = foo(a_vec) + foo(a_mat);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test that an user-defined function with a large number of float4 parameters doesn't fail due to
// the function name being too long.
TEST_P(GLSLValidationTest_ES3, LargeNumberOfFloat4Parameters)
{
    std::stringstream vs;
    // Note: SPIR-V doesn't allow more than 255 parameters to a function.
    const unsigned int paramCount = (IsVulkan() || IsMetal()) ? 255u : 1024u;

    vs << R"(#version 300 es
precision highp float;
in vec4 a_vec;
vec4 lotsOfVec4Parameters()";
    for (unsigned int i = 0; i < paramCount - 1; ++i)
    {
        vs << "vec4 a" << i << ", ";
    }
    vs << R"(vec4 aLast)
{
    vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);)";
    for (unsigned int i = 0; i < paramCount - 1; ++i)
    {
        vs << "    sum += a" << i << ";\n";
    }
    vs << R"(    sum += aLast;
    return sum;
}
void main()
{
    gl_Position = lotsOfVec4Parameters()";
    for (unsigned int i = 0; i < paramCount - 1; ++i)
    {
        vs << "a_vec, ";
    }
    vs << R"(a_vec);
})";
    validateSuccess(GL_VERTEX_SHADER, vs.str().c_str());
}

// This test was written specifically to stress DeferGlobalInitializers AST transformation.
// Test a shader where a global constant array is initialized with an expression containing array
// indexing. This initializer is tricky to constant fold, so if it's not constant folded it needs to
// be handled in a way that doesn't generate statements in the global scope in HLSL output.
// Also includes multiple array initializers in one declaration, where only the second one has
// array indexing. This makes sure that the qualifier for the declaration is set correctly if
// transformations are applied to the declaration also in the case of ESSL output.
TEST_P(GLSLValidationTest_ES3, InitGlobalArrayWithArrayIndexing)
{
    // TODO(ynovikov): re-enable once root cause of http://anglebug.com/42260423 is fixed
    ANGLE_SKIP_TEST_IF(IsAndroid() && IsAdreno() && IsOpenGLES());

    constexpr char kFS[] = R"(#version 300 es
precision highp float;
out vec4 my_FragColor;
const highp float f[2] = float[2](0.1, 0.2);
const highp float[2] g = float[2](0.3, 0.4), h = float[2](0.5, f[1]);
void main()
{
    my_FragColor = vec4(h[1]);
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that index-constant sampler array indexing is supported.
TEST_P(GLSLValidationTest, IndexConstantSamplerArrayIndexing)
{
    constexpr char kFS[] = R"(
precision mediump float;
uniform sampler2D uni[2];

float zero(int x)
{
    return float(x) - float(x);
}

void main()
{
    vec4 c = vec4(0,0,0,0);
    for (int ii = 1; ii < 3; ++ii) {
        if (c.x > 255.0) {
            c.x = 255.0 + zero(ii);
            break;
        }
        // Index the sampler array with a predictable loop index (index-constant) as opposed to
        // a true constant. This is valid in OpenGL ES but isn't in many Desktop OpenGL versions,
        // without an extension.
        c += texture2D(uni[ii - 1], vec2(0.5, 0.5));
    }
    gl_FragColor = c;
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that the #pragma directive is supported and doesn't trigger a compilation failure on the
// native driver. The only pragma that gets passed to the OpenGL driver is "invariant" but we don't
// want to test its behavior, so don't use any varyings.
TEST_P(GLSLValidationTest, PragmaDirective)
{
    constexpr char kVS[] = R"(#pragma STDGL invariant(all)
void main()
{
    gl_Position = vec4(1.0, 0.0, 0.0, 1.0);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Tests that using a constant declaration as the only statement in a for loop without curly braces
// doesn't crash.
TEST_P(GLSLValidationTest, ConstantStatementInForLoop)
{
    constexpr char kVS[] = R"(void main()
{
    for (int i = 0; i < 10; ++i)
        const int b = 0;
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Tests that using a constant declaration as a loop init expression doesn't crash. Note that this
// test doesn't work on D3D9 due to looping limitations, so it is only run on ES3.
TEST_P(GLSLValidationTest_ES3, ConstantStatementAsLoopInit)
{
    constexpr char kVS[] = R"(void main()
{
    for (const int i = 0; i < 0;) {}
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Tests that using a constant condition guarding a discard works
// Covers a failing case in the Vulkan backend: http://anglebug.com/42265506
TEST_P(GLSLValidationTest_ES3, ConstantConditionGuardingDiscard)
{
    constexpr char kFS[] = R"(#version 300 es
void main()
{
    if (true)
    {
        discard;
    }
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Tests that nesting a discard in unconditional blocks works
// Covers a failing case in the Vulkan backend: http://anglebug.com/42265506
TEST_P(GLSLValidationTest_ES3, NestedUnconditionalDiscards)
{
    constexpr char kFS[] = R"(#version 300 es
out mediump vec4 c;
void main()
{
    {
        c = vec4(0);
        {
            discard;
        }
    }
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Tests that rewriting samplers in structs works when passed as function argument.  In this test,
// the function references another struct, which is not being modified.  Regression test for AST
// validation applied to a multipass transformation, where references to declarations were attempted
// to be validated without having the entire shader.  In this case, the reference to S2 was flagged
// as invalid because S2's declaration was not visible.
TEST_P(GLSLValidationTest, SamplerInStructAsFunctionArg)
{
    const char kFS[] = R"(precision mediump float;
struct S { sampler2D samp; bool b; };
struct S2 { float f; };

uniform S us;

float f(S s)
{
    S2 s2;
    s2.f = float(s.b);
    return s2.f;
}

void main()
{
    gl_FragColor = vec4(f(us), 0, 0, 1);
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test a fuzzer-discovered bug with the VectorizeVectorScalarArithmetic transformation.
TEST_P(GLSLValidationTest, VectorScalarArithmeticWithSideEffectInLoop)
{
    // The VectorizeVectorScalarArithmetic transformation was generating invalid code in the past
    // (notice how sbcd references i outside the for loop.  The loop condition doesn't look right
    // either):
    //
    //     #version 450
    //     void main(){
    //     (gl_Position = vec4(0.0, 0.0, 0.0, 0.0));
    //     mat3 _utmp = mat3(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    //     vec3 _ures = vec3(0.0, 0.0, 0.0);
    //     vec3 sbcd = vec3(_ures[_ui]);
    //     for (int _ui = 0; (_ures[((_utmp[_ui] += (((sbcd *= _ures[_ui]), (_ures[_ui] = sbcd.x)),
    //     sbcd)), _ui)], (_ui < 7)); )
    //     {
    //     }
    //     }

    constexpr char kVS[] = R"(
void main()
{
    mat3 tmp;
    vec3 res;
    for(int i; res[tmp[i]+=res[i]*=res[i],i],i<7;);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test that inactive output variables compile ok in combination with initOutputVariables
// (which is enabled on WebGL).
TEST_P(WebGL2GLSLValidationTest, InactiveOutput)
{
    constexpr char kFS[] = R"(#version 300 es
precision highp float;
out vec4 _cassgl_2_;
void main()
{
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that output variables declared after main work in combination with initOutputVariables
// (which is enabled on WebGL).
TEST_P(WebGLGLSLValidationTest, OutputAfterMain)
{
    constexpr char kVS[] = R"(void main(){}
varying float r;)";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test angle can handle big initial stack size with dynamic stack allocation.
TEST_P(GLSLValidationTest, MemoryExhaustedTest)
{
    constexpr int kLength = 36;

    std::stringstream fs;
    fs << "void main() {\n";
    for (int i = 0; i < kLength; i++)
    {
        fs << "  if (true) {\n";
    }
    fs << "  int temp;\n";
    for (int i = 0; i <= kLength; i++)
    {
        fs << "}";
    }
    validateSuccess(GL_FRAGMENT_SHADER, fs.str().c_str());
}

// Test that separating declarators works with structs that have been separately defined.
TEST_P(GLSLValidationTest_ES31, SeparateDeclaratorsOfStructType)
{
    constexpr char kVS[] = R"(#version 310 es
precision highp float;

struct S
{
    mat4 a;
    mat4 b;
};

S s1 = S(mat4(1), mat4(2)), s2[2][3], s3[2] = S[2](S(mat4(0), mat4(3)), S(mat4(4), mat4(5)));

void main() {
    S s4[2][3] = s2, s5 = s3[0], s6[2] = S[2](s1, s5), s7 = s5;

    gl_Position = vec4(s3[1].a[0].x, s2[0][2].b[1].y, s4[1][0].a[2].z, s6[0].b[3].w);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test that separating declarators works with structs that are simultaneously defined.
TEST_P(GLSLValidationTest_ES31, SeparateDeclaratorsOfStructTypeBeingSpecified)
{
    constexpr char kVS[] = R"(#version 310 es
precision highp float;

struct S
{
    mat4 a;
    mat4 b;
} s1 = S(mat4(1), mat4(2)), s2[2][3], s3[2] = S[2](S(mat4(0), mat4(3)), S(mat4(4), mat4(5)));

void main() {
    struct T
    {
        mat4 a;
        mat4 b;
    } s4[2][3], s5 = T(s3[0].a, s3[0].b), s6[2] = T[2](T(s1.a, s1.b), s5), s7 = s5;

    float f1 = s3[1].a[0].x, f2 = s2[0][2].b[1].y;

    gl_Position = vec4(f1, f2, s4[1][0].a[2].z, s6[0].b[3].w);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test that separating declarators works with structs that are simultaneously defined and that are
// nameless.
TEST_P(GLSLValidationTest_ES31, SeparateDeclaratorsOfNamelessStructType)
{
    constexpr char kVS[] = R"(#version 310 es
precision highp float;

struct
{
    mat4 a;
    mat4 b;
} s1, s2[2][3], s3[2];

void main() {
    struct
    {
        mat4 a;
        mat4 b;
    } s4[2][3], s5, s6[2], s7 = s5;

    float f1 = s1.a[0].x + s3[1].a[0].x, f2 = s2[0][2].b[1].y + s7.b[1].z;

    gl_Position = vec4(f1, f2, s4[1][0].a[2].z, s6[0].b[3].w);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Regression test for transformation bug which separates struct declarations from uniform
// declarations.  The bug was that the uniform variable usage in the initializer of a new
// declaration (y below) was not being processed.
TEST_P(GLSLValidationTest, UniformStructBug)
{
    constexpr char kVS[] = R"(precision highp float;

uniform struct Global
{
    float x;
} u_global;

void main() {
  float y = u_global.x;

  gl_Position = vec4(y);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Regression test for transformation bug which separates struct declarations from uniform
// declarations.  The bug was that the arrayness of the declaration was not being applied to the
// replaced uniform variable.
TEST_P(GLSLValidationTest_ES31, UniformStructBug2)
{
    constexpr char kVS[] = R"(#version 310 es
precision highp float;

uniform struct Global
{
    float x;
} u_global[2][3];

void main() {
  float y = u_global[0][0].x;

  gl_Position = vec4(y);
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Regression test based on fuzzer issue resulting in an AST validation failure.  Struct definition
// was not found in the tree.  Tests that struct declaration in function return value is visible to
// instantiations later on.
TEST_P(GLSLValidationTest, MissingStructDeclarationBug)
{
    constexpr char kVS[] = R"(
struct S
{
    vec4 i;
} p();
void main()
{
    S s;
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Regression test based on fuzzer issue resulting in an AST validation failure.  Struct definition
// was not found in the tree.  Tests that struct declaration in function return value is visible to
// other struct declarations.
TEST_P(GLSLValidationTest, MissingStructDeclarationBug2)
{
    constexpr char kVS[] = R"(
struct T
{
    vec4 I;
} p();
struct
{
    T c;
};
void main()
{
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Regression test for bug in HLSL code generation where the for loop init expression was expected
// to always have an initializer.
TEST_P(GLSLValidationTest, HandleExcessiveLoopBug)
{
    constexpr char kVS[] = R"(void main(){for(int i;i>6;);})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test that providing more components to a matrix constructor than necessary works.  Based on a
// clusterfuzz test that caught an OOB array write in glslang.
TEST_P(GLSLValidationTest, MatrixConstructor)
{
    constexpr char kVS[] = R"(attribute vec4 aPosition;
varying vec4 vColor;
void main()
{
    gl_Position = aPosition;
    vec4 color = vec4(aPosition.xy, 0, 1);
    mat4 m4 = mat4(color, color.yzwx, color.zwx, color.zwxy, color.wxyz);
    vColor = m4[0];
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test constructors without precision
TEST_P(GLSLValidationTest, ConstructFromBoolVector)
{
    constexpr char kFS[] = R"(precision mediump float;
uniform float u;
void main()
{
    mat4 m = mat4(u);
    mat2(0, bvec3(m));
    gl_FragColor = vec4(m);
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test constructing vector from matrix
TEST_P(GLSLValidationTest, VectorConstructorFromMatrix)
{
    constexpr char kFS[] = R"(precision mediump float;
uniform mat2 umat2;
void main()
{
    gl_FragColor = vec4(umat2);
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that initializing global variables with non-constant values work
TEST_P(GLSLValidationTest_ES3, InitGlobalNonConstant)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_shader_non_constant_global_initializers"));

    constexpr char kVS[] = R"(#version 300 es
#extension GL_EXT_shader_non_constant_global_initializers : require
uniform vec4 u;
out vec4 color;

vec4 global1 = u;
vec4 global2 = u + vec4(1);
vec4 global3 = global1 * global2;
void main()
{
    color = global3;
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Regression test for a crash in SPIR-V output when faced with an array of struct constant.
TEST_P(GLSLValidationTest_ES3, ArrayOfStructConstantBug)
{
    constexpr char kFS[] = R"(#version 300 es
struct S {
    int foo;
};
void main() {
    S a[3];
    a = S[3](S(0), S(1), S(2));
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Regression test for a bug in SPIR-V output where float+matrix was mishandled.
TEST_P(GLSLValidationTest_ES3, FloatPlusMatrix)
{
    constexpr char kFS[] = R"(#version 300 es

precision mediump float;

layout(location=0) out vec4 color;

uniform float f;

void main()
{
    mat3x2 m = f + mat3x2(0);
    color = vec4(m[0][0]);
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Regression test for a bug in SPIR-V output where a transformation creates float(constant) without
// folding it into a TIntermConstantUnion.  This transformation is clamping non-constant indices in
// WebGL.  The |false ? i : 5| as index caused the transformation to consider this a non-constant
// index.
TEST_P(WebGL2GLSLValidationTest, IndexClampConstantIndexBug)
{
    constexpr char kFS[] = R"(#version 300 es
precision highp float;

layout(location=0) out float f;

uniform int i;

void main()
{
    float data[10];
    f = data[false ? i : 5];
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that framebuffer fetch transforms gl_LastFragData in the presence of gl_FragCoord without
// failing validation (adapted from a Chromium test, see anglebug.com/42265427)
TEST_P(GLSLValidationTest, FramebufferFetchWithLastFragData)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_shader_framebuffer_fetch"));

    constexpr char kFS[] = R"(#version 100

#extension GL_EXT_shader_framebuffer_fetch : require
varying mediump vec4 color;
void main() {
    gl_FragColor = length(gl_FragCoord.xy) * gl_LastFragData[0];
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that loop body ending in a branch doesn't fail compilation
TEST_P(GLSLValidationTest, LoopBodyEndingInBranch1)
{
    constexpr char kFS[] = R"(void main(){for(int a,i;;gl_FragCoord)continue;})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that loop body ending in a branch doesn't fail compilation
TEST_P(GLSLValidationTest, LoopBodyEndingInBranch2)
{
    constexpr char kFS[] =
        R"(void main(){for(int a,i;bool(gl_FragCoord.x);gl_FragCoord){continue;}})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that loop body ending in a branch doesn't fail compilation
TEST_P(GLSLValidationTest, LoopBodyEndingInBranch3)
{
    constexpr char kFS[] = R"(void main(){for(int a,i;;gl_FragCoord){{continue;}}})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that loop body ending in a branch doesn't fail compilation
TEST_P(GLSLValidationTest, LoopBodyEndingInBranch4)
{
    constexpr char kFS[] = R"(void main(){for(int a,i;;gl_FragCoord){{continue;}{}{}{{}{}}}})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that loop body ending in a branch doesn't fail compilation
TEST_P(GLSLValidationTest, LoopBodyEndingInBranch5)
{
    constexpr char kFS[] = R"(void main(){while(bool(gl_FragCoord.x)){{continue;{}}{}}})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that loop body ending in a branch doesn't fail compilation
TEST_P(GLSLValidationTest, LoopBodyEndingInBranch6)
{
    constexpr char kFS[] = R"(void main(){do{{continue;{}}{}}while(bool(gl_FragCoord.x));})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Fuzzer test involving struct samplers and comma operator
TEST_P(GLSLValidationTest, StructSamplerVsComma)
{
    constexpr char kVS[] = R"(uniform struct S1
{
    samplerCube ar;
    vec2 c;
} a;

struct S2
{
    vec3 c;
} b[2];

void main (void)
{
    ++b[0].c,a;
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Regression test for a bug where the sampler-in-struct rewrite transformation did not take a
// specific pattern of side_effect,index_the_struct_to_write into account.
TEST_P(GLSLValidationTest_ES3, StructWithSamplerRHSOfCommaWithSideEffect)
{
    constexpr char kVS[] = R"(uniform struct S {
    sampler2D s;
    mat2 m;
} u[2];
void main()
{
    ++gl_Position, u[0];
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Regression test for a bug where the sampler-in-struct rewrite transformation did not take a
// specific pattern of side_effect,struct_with_only_samplers into account.
TEST_P(GLSLValidationTest_ES3, StructWithOnlySamplersRHSOfCommaWithSideEffect)
{
    constexpr char kVS[] = R"(uniform struct S {
    sampler2D s;
} u;
void main()
{
    ++gl_Position, u;
})";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test that gl_FragDepth can be marked invariant.
TEST_P(GLSLValidationTest_ES3, FragDepthInvariant)
{
    constexpr char kFS[] = R"(#version 300 es
#extension GL_EXT_conservative_depth: enable
precision mediump float;
invariant gl_FragDepth;
void main() {
    gl_FragDepth = 0.5;
}
)";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that gl_Position and gl_PointSize can be marked invariant and redeclared in separate
// statements. Note that EXT_seperate_shader_objects expects the redeclaration to come first.
TEST_P(GLSLValidationTest_ES31, PositionRedeclaredAndInvariant)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_EXT_separate_shader_objects"));

    constexpr char kVS[] = R"(#version 310 es
#extension GL_EXT_separate_shader_objects: require
precision mediump float;
out vec4 gl_Position;
out float gl_PointSize;
invariant gl_Position;
invariant gl_PointSize;
void main() {
}
)";
    validateSuccess(GL_VERTEX_SHADER, kVS);
}

// Test an invalid shader where a for loop index is used as an out parameter.
// See limitations in ESSL 1.00 Appendix A.
TEST_P(WebGLGLSLValidationTest, IndexAsFunctionOutParameter)
{
    const char kFS[] = R"(precision mediump float;
void fun(out int a)
{
   a = 2;
}
void main()
{
    for (int i = 0; i < 2; ++i)
    {
        fun(i);
    }
    gl_FragColor = vec4(0.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'i' : Loop index cannot be statically assigned to within the body of the loop");
}

// Test an invalid shader where a for loop index is used as an inout parameter.
// See limitations in ESSL 1.00 Appendix A.
TEST_P(WebGLGLSLValidationTest, IndexAsFunctionInOutParameter)
{
    const char kFS[] = R"(precision mediump float;
void fun(int b, inout int a)
{
   a += b;
}
void main()
{
    for (int i = 0; i < 2; ++i)
    {
        fun(2, i);
    }
    gl_FragColor = vec4(0.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'i' : Loop index cannot be statically assigned to within the body of the loop");
}

// Test a valid shader where a for loop index is used as an in parameter in a function that also has
// an out parameter.
// See limitations in ESSL 1.00 Appendix A.
TEST_P(WebGLGLSLValidationTest, IndexAsFunctionInParameter)
{
    const char kFS[] = R"(precision mediump float;
void fun(int b, inout int a)
{
   a += b;
}
void main()
{
    for (int i = 0; i < 2; ++i)
    {
        int a = 1;
        fun(i, a);
    }
    gl_FragColor = vec4(0.0);
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test an invalid shader where a for loop index is used as a target of assignment.
// See limitations in ESSL 1.00 Appendix A.
TEST_P(WebGLGLSLValidationTest, IndexAsTargetOfAssignment)
{
    const char kFS[] = R"(precision mediump float;
void main()
{
    for (int i = 0; i < 2; ++i)
    {
        i = 2;
    }
    gl_FragColor = vec4(0.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'i' : Loop index cannot be statically assigned to within the body of the loop");
}

// Test an invalid shader where a for loop index is incremented inside the loop.
// See limitations in ESSL 1.00 Appendix A.
TEST_P(WebGLGLSLValidationTest, IndexIncrementedInLoopBody)
{
    const char kFS[] = R"(precision mediump float;
void main()
{
    for (int i = 0; i < 2; ++i)
    {
        ++i;
    }
    gl_FragColor = vec4(0.0);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'i' : Loop index cannot be statically assigned to within the body of the loop");
}

class GLSLValidationClipDistanceTest_ES3 : public GLSLValidationTest_ES3
{};

// Extension support is required to compile properly.  Expect failure when it is not present.
TEST_P(GLSLValidationClipDistanceTest_ES3, CompileFailsWithoutExtension)
{
    ANGLE_SKIP_TEST_IF(IsGLExtensionEnabled("GL_APPLE_clip_distance"));

    {
        constexpr char kVS[] = R"(#extension GL_APPLE_clip_distance : require
uniform vec4 uPlane;

attribute vec4 aPosition;

void main()
{
    gl_Position = aPosition;
    gl_ClipDistance[1] = dot(aPosition, uPlane);
})";
        validateError(GL_VERTEX_SHADER, kVS,
                      "'GL_APPLE_clip_distance' : extension is not supported");
    }

    {
        constexpr char kVS[] = R"(#version 300 es
#extension GL_APPLE_clip_distance : require
uniform vec4 uPlane;

in vec4 aPosition;

void main()
{
    gl_Position = aPosition;
    gl_ClipDistance[1] = dot(aPosition, uPlane);
})";
        validateError(GL_VERTEX_SHADER, kVS,
                      "'GL_APPLE_clip_distance' : extension is not supported");
    }
}

// Extension directive is required to compile properly.  Expect failure when it is not present.
TEST_P(GLSLValidationClipDistanceTest_ES3, CompileFailsWithExtensionWithoutPragma)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_APPLE_clip_distance"));

    {
        constexpr char kVS[] = R"(uniform vec4 uPlane;

attribute vec4 aPosition;

void main()
{
    gl_Position = aPosition;
    gl_ClipDistance[1] = dot(aPosition, uPlane);
})";
        validateError(GL_VERTEX_SHADER, kVS, "'GL_APPLE_clip_distance' : extension is disabled");
    }

    {
        constexpr char kVS[] = R"(#version 300 es
uniform vec4 uPlane;

in vec4 aPosition;

void main()
{
    gl_Position = aPosition;
    gl_ClipDistance[1] = dot(aPosition, uPlane);
})";
        validateError(GL_VERTEX_SHADER, kVS, "'GL_APPLE_clip_distance' : extension is disabled");
    }
}

class GLSLValidationTextureRectangleTest : public GLSLValidationTest
{};

// Check that if the extension is not supported, trying to use the features without having an
// extension directive fails.
//
// If the extension is supported, check that new types and builtins are usable even with the
// #extension directive
// Issue #15 of ARB_texture_rectangle explains that the extension was specified before the
// #extension mechanism was in place so it doesn't require explicit enabling.
TEST_P(GLSLValidationTextureRectangleTest, NewTypeAndBuiltinsWithoutExtensionDirective)
{
    const char kFS[] = R"(
precision mediump float;
uniform sampler2DRect tex;
void main()
{
    vec4 color = texture2DRect(tex, vec2(1.0));
    color = texture2DRectProj(tex, vec3(1.0));
    color = texture2DRectProj(tex, vec4(1.0));
})";
    if (IsGLExtensionEnabled("GL_ANGLE_texture_rectangle"))
    {
        validateSuccess(GL_FRAGMENT_SHADER, kFS);
    }
    else
    {
        validateError(GL_FRAGMENT_SHADER, kFS,
                      "'GL_ARB_texture_rectangle' : extension is not supported");
    }
}

// Check that if the extension is not supported, trying to use the features fails.
//
// If the extension is supported, test that using the feature with the extension directive passes.
TEST_P(GLSLValidationTextureRectangleTest, NewTypeAndBuiltinsWithExtensionDirective)
{
    const char kFS[] = R"(#extension GL_ARB_texture_rectangle : enable
precision mediump float;
uniform sampler2DRect tex;
void main()
{
    vec4 color = texture2DRect(tex, vec2(1.0));
    color = texture2DRectProj(tex, vec3(1.0));
    color = texture2DRectProj(tex, vec4(1.0));
})";
    if (IsGLExtensionEnabled("GL_ANGLE_texture_rectangle"))
    {
        validateSuccess(GL_FRAGMENT_SHADER, kFS);
    }
    else
    {
        validateError(GL_FRAGMENT_SHADER, kFS,
                      "'GL_ARB_texture_rectangle' : extension is not supported");
    }
}

// Check that it is not possible to pass a sampler2DRect where sampler2D is expected, and vice versa
TEST_P(GLSLValidationTextureRectangleTest, Rect2DVs2DMismatch)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_ANGLE_texture_rectangle"));

    {
        const char kFS[] = R"(
#extension GL_ARB_texture_rectangle : require
precision mediump float;
uniform sampler2DRect tex;
void main() {
    vec4 color = texture2D(tex, vec2(1.0));"
})";
        validateError(GL_FRAGMENT_SHADER, kFS,
                      "'texture2D' : no matching overloaded function found");
    }

    {
        const char kFS[] = R"(
#extension GL_ARB_texture_rectangle : require
precision mediump float;
uniform sampler2D tex;
void main() {
    vec4 color = texture2DRect(tex, vec2(1.0));"
})";
        validateError(GL_FRAGMENT_SHADER, kFS,
                      "'texture2DRect' : no matching overloaded function found");
    }
}

// Disabling ARB_texture_rectangle in GLSL should work, even if it is enabled by default.
// See ARB_texture_rectangle spec: "a shader can still include all variations of #extension
// GL_ARB_texture_rectangle in its source code"
TEST_P(GLSLValidationTextureRectangleTest, DisableARBTextureRectangle)
{
    ANGLE_SKIP_TEST_IF(!IsGLExtensionEnabled("GL_ANGLE_texture_rectangle"));

    const char kFS[] = R"(#extension GL_ARB_texture_rectangle : disable
precision mediump float;

uniform sampler2DRect s;
void main()
{})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'GL_ARB_texture_rectangle' : extension is disabled");
}

class GLSLValidationAtomicCounterTest_ES31 : public GLSLValidationTest_ES31
{};

// Test that ESSL 3.00 doesn't support atomic_uint.
TEST_P(GLSLValidationAtomicCounterTest_ES31, InvalidShaderVersion)
{
    constexpr char kFS[] = R"(#version 300 es
layout(binding = 0, offset = 4) uniform atomic_uint a;
void main()
{
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'atomic_uint' : Illegal use of reserved word");
}

// Test that any qualifier other than uniform leads to compile-time error.
TEST_P(GLSLValidationAtomicCounterTest_ES31, InvalidQualifier)
{
    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
layout(binding = 0, offset = 4) in atomic_uint a;
void main()
{
})";
    validateError(GL_COMPUTE_SHADER, kCS, "'atomic_uint' : atomic_uints must be uniform");
}

// Test that uniform must be specified for declaration.
TEST_P(GLSLValidationAtomicCounterTest_ES31, UniformMustSpecifiedForDeclaration)
{
    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
atomic_uint a;
void main()
{
})";
    validateError(GL_COMPUTE_SHADER, kCS, "'atomic_uint' : atomic_uints must be uniform");
}

// Test that offset overlapping leads to compile-time error (ESSL 3.10 section 4.4.6).
TEST_P(GLSLValidationAtomicCounterTest_ES31, BindingOffsetOverlapping)
{
    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
layout(binding = 0, offset = 4) uniform atomic_uint a;
layout(binding = 0, offset = 6) uniform atomic_uint b;
void main()
{
})";
    validateError(GL_COMPUTE_SHADER, kCS, "'atomic counter' : Offset overlapping");
}

// Test offset inheritance for multiple variables in one same declaration.
TEST_P(GLSLValidationAtomicCounterTest_ES31, MultipleVariablesDeclaration)
{
    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
layout(binding = 0, offset = 4) uniform atomic_uint a, b;
layout(binding = 0, offset = 8) uniform atomic_uint c;
void main()
{
})";
    validateError(GL_COMPUTE_SHADER, kCS, "'atomic counter' : Offset overlapping");
}

// Test that subsequent declarations inherit the globally specified offset.
TEST_P(GLSLValidationAtomicCounterTest_ES31, GlobalBindingOffsetOverlapping)
{
    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
layout(binding = 2, offset = 4) uniform atomic_uint;
layout(binding = 2) uniform atomic_uint b;
layout(binding = 2, offset = 4) uniform atomic_uint c;
void main()
{
})";
    validateError(GL_COMPUTE_SHADER, kCS, "'atomic counter' : Offset overlapping");
}

// The spec only demands offset unique and non-overlapping. So this should be allowed.
TEST_P(GLSLValidationAtomicCounterTest_ES31, DeclarationSequenceWithDecrementalOffsetsSpecified)
{
    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
layout(binding = 2, offset = 4) uniform atomic_uint a;
layout(binding = 2, offset = 0) uniform atomic_uint b;
void main()
{
})";
    validateSuccess(GL_COMPUTE_SHADER, kCS);
}

// Test that image format qualifiers are not allowed for atomic counters.
TEST_P(GLSLValidationAtomicCounterTest_ES31, ImageFormatMustNotSpecified)
{
    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
layout(binding = 0, offset = 4, rgba32f) uniform atomic_uint a;
void main()
{
})";
    validateError(GL_COMPUTE_SHADER, kCS,
                  "'rgba32f' : invalid layout qualifier: only valid when used with images");
}

// Test that global layout qualifiers must not use 'offset'.
TEST_P(GLSLValidationAtomicCounterTest_ES31, OffsetMustNotSpecifiedForGlobalLayoutQualifier)
{
    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
layout(offset = 4) in;
void main()
{
})";
    validateError(GL_COMPUTE_SHADER, kCS,
                  "'offset' : invalid layout qualifier: only valid when used with atomic counters");
}

// Test that offset overlapping leads to compile-time error (ESSL 3.10 section 4.4.6).
// Note that there is some vagueness in the spec when it comes to this test.
TEST_P(GLSLValidationAtomicCounterTest_ES31, BindingOffsetOverlappingForArrays)
{
    GLint maxAtomicCounterBuffers = 0;
    glGetIntegerv(GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS, &maxAtomicCounterBuffers);
    ANGLE_SKIP_TEST_IF(maxAtomicCounterBuffers < 3);

    constexpr char kCS[] = R"(#version 310 es
layout(local_size_x=1) in;
layout(binding = 2, offset = 4) uniform atomic_uint[2] a;
layout(binding = 2, offset = 8) uniform atomic_uint b;
void main()
{
})";
    validateError(GL_COMPUTE_SHADER, kCS, "'atomic counter' : Offset overlapping");
}

class GLSLValidationShaderStorageBlockTest_ES31 : public GLSLValidationTest_ES31
{};

// Test that shader storage block layout qualifiers can be declared for global scope.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, LayoutQualifiersDeclaredInGlobal)
{
    constexpr char kFS[] = R"(#version 310 es
layout(shared, column_major) buffer;
void main()
{
})";
    validateSuccess(GL_FRAGMENT_SHADER, kFS);
}

// Test that it is a compile-time error to declare buffer variables at global scope (outside a
// block).
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, DeclareBufferVariableAtGlobal)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer int a;
void main()
{
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'buffer' : cannot declare buffer variables at global scope(outside a block)");
}

// Test that the buffer variable can't be opaque type.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, BufferVariableWithOpaqueType)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    int b1;
    atomic_uint b2;
};
void main()
{
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'buf' : Opaque types are not allowed in interface blocks");
}

// Test that the uniform variable can't be in shader storage block.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, UniformVariableInShaderStorageBlock)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    uniform int a;
};
void main()
{
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'uniform' : invalid qualifier on shader storage block member");
}

// Test that buffer qualifier is not supported in version lower than GLSL ES 3.10.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, BufferQualifierInESSL3)
{
    constexpr char kFS[] = R"(#version 300 es
layout(binding = 3) buffer buf {
    int b1;
    buffer int b2;
};
void main()
{
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'binding' : invalid layout qualifier: not supported");
}

// Test that can't assign to a readonly buffer variable.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, AssignToReadonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    readonly int b1;
};
void main()
{
    b1 = 5;
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  R"('assign' : l-value required (can't modify a readonly variable "b1"))");
}

// Test that can't assign to a buffer variable declared within shader storage block with readonly.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, AssignToBufferVariableWithinReadonlyBlock)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) readonly buffer buf {
    int b1;
};
void main()
{
    b1 = 5;
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  R"('assign' : l-value required (can't modify a readonly variable "b1"))");
}

// Test that can't assign to a readonly buffer variable through an instance name.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, AssignToReadonlyBufferVariableByInstanceName)
{
    constexpr char kFS[] = R"(#version 310 es
precision highp float;
layout(binding = 3) buffer buf {
    readonly float f;
} instanceBuffer;
void main()
{
    instanceBuffer.f += 0.2;
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'assign' : can't modify a readonly variable");
}

// Test that can't assign to a readonly struct buffer variable.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, AssignToReadonlyStructBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
precision highp float;
struct S {
    float f;
};
layout(binding = 3) buffer buf {
    readonly S s;
};
void main()
{
    s.f += 0.2;
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  R"('assign' : l-value required (can't modify a readonly variable "s"))");
}

// Test that can't assign to a readonly struct buffer variable through an instance name.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31,
       AssignToReadonlyStructBufferVariableByInstanceName)
{
    constexpr char kFS[] = R"(#version 310 es
precision highp float;
struct S {
    float f;
};
layout(binding = 3) buffer buf {
    readonly S s;
} instanceBuffer;
void main()
{
    instanceBuffer.s.f += 0.2;
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'assign' : can't modify a readonly variable");
}

// Test that a readonly and writeonly buffer variable should neither read or write.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, AccessReadonlyWriteonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    readonly writeonly int b1;
};
void main()
{
    b1 = 5;
    int test = b1;
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  R"('assign' : l-value required (can't modify a readonly variable "b1"))");
}

// Test that accessing a writeonly buffer variable should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, AccessWriteonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    writeonly int b1;
};
void main()
{
    int test = b1;
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'=' : Invalid operation for variables with writeonly");
}

// Test that accessing a buffer variable through an instance name inherits the writeonly qualifier
// and generates errors.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, AccessWriteonlyBufferVariableByInstanceName)
{
    constexpr char kFS[] = R"(#version 310 es
precision highp float;
layout(binding = 3) writeonly buffer buf {
    float f;
} instanceBuffer;
void main()
{
    float test = instanceBuffer.f;
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'=' : Invalid operation for variables with writeonly");
}

// Test that writeonly buffer variable as the argument of a unary operator should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, UnaryOperatorWithWriteonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    writeonly int b1;
};
void main()
{
    ++b1;
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'++' : wrong operand type - no operation '++' exists that takes an operand of "
                  "type buffer mediump writeonly int (or there is no acceptable conversion)");
}

// Test that writeonly buffer variable on the left-hand side of compound assignment should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, CompoundAssignmentToWriteonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    writeonly int b1;
};
void main()
{
    b1 += 5;
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'+=' : Invalid operation for variables with writeonly");
}

// Test that writeonly buffer variable as ternary op argument should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, TernarySelectionWithWriteonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    writeonly bool b1;
};
void main()
{
    int test = b1 ? 1 : 0;
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'?:' : ternary operator is not allowed for variables with writeonly");
}

// Test that writeonly buffer variable as array constructor argument should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, ArrayConstructorWithWriteonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
precision highp float;
layout(binding = 3) buffer buf {
    writeonly float f;
};
void main()
{
    float a[3] = float[3](f, f, f);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'constructor' : cannot convert a variable with writeonly");
}

// Test that writeonly buffer variable as structure constructor argument should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, StructureConstructorWithWriteonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
struct S {
    int a;
};
struct T {
    S b;
};
layout(binding = 3) buffer buf {
    writeonly S c;
};
void main()
{
    T t = T(c);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'constructor' : cannot convert a variable with writeonly");
}

// Test that writeonly buffer variable as built-in function argument should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, BuiltInFunctionWithWriteonlyBufferVariable)
{
    constexpr char kFS[] = R"(#version 310 es
layout(binding = 3) buffer buf {
    writeonly int a;
};
void main()
{
    int test = min(a, 1);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'min' : Writeonly value cannot be passed for 'in' or 'inout' parameters");
}

// Test that writeonly buffer variable as user-defined function in argument should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31,
       UserDefinedFunctionWithWriteonlyBufferVariableInArgument)
{
    constexpr char kFS[] = R"(#version 310 es
precision highp float;
layout(binding = 3) buffer buf {
    writeonly float f;
};
void foo(float a) {}
void main()
{
    foo(f);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "foo' : Writeonly value cannot be passed for 'in' or 'inout' parameters");
}

// Test that readonly buffer variable as user-defined function out argument should be error.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31,
       UserDefinedFunctionWithReadonlyBufferVariableOutArgument)
{
    constexpr char kFS[] = R"(#version 310 es
precision highp float;
layout(binding = 3) buffer buf {
    readonly float f;
};
void foo(out float a) {}
void main()
{
    foo(f);
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  R"('assign' : l-value required (can't modify a readonly variable "f"))");
}

// Test that buffer qualifier can't modify a function parameter.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, BufferQualifierOnFunctionParameter)
{
    constexpr char kFS[] = R"(#version 310 es
precision highp float;
void foo(buffer float a) {}
void main()
{
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'buffer' : only allowed at global scope");
}

// Test that using std430 qualifier on a uniform block will fail to compile.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, UniformBlockWithStd430)
{
    constexpr char kFS[] = R"(#version 310 es
layout(std430) uniform buf {
    int b1;
    int b2;
};
void main()
{
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'std430' : The std430 layout is supported only for shader storage blocks");
}

// Test that indexing a runtime-sized array with a negative constant index does not compile.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, IndexRuntimeSizedArrayWithNegativeIndex)
{
    constexpr char kFS[] = R"(#version 310 es
layout(std430) buffer buf
{
    int arr[];
};

void main()
{
    arr[-1];
})";
    validateError(GL_FRAGMENT_SHADER, kFS, "'[]' : index expression is negative");
}

// Test that only the last member of a buffer can be runtime-sized.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, RuntimeSizedVariableInNotLastInBuffer)
{
    constexpr char kFS[] = R"(#version 310 es
layout(std430) buffer buf
{
    int arr[];
    int i;
};

void main()
{
})";
    validateError(GL_FRAGMENT_SHADER, kFS,
                  "'arr' : array members of interface blocks must specify a size");
}

// Test that memory qualifiers are output.
TEST_P(GLSLValidationShaderStorageBlockTest_ES31, MemoryQualifiers)
{
    constexpr char kFS[]         = R"(#version 310 es
precision highp float;
precision highp int;
layout(std430) coherent buffer buf
{
    int defaultCoherent;
    coherent ivec2 specifiedCoherent;
    volatile ivec3 specifiedVolatile;
    restrict ivec4 specifiedRestrict;
    readonly float specifiedReadOnly;
    writeonly vec2 specifiedWriteOnly;
    volatile readonly vec3 specifiedMultiple;
};

void main()
{
})";
    const CompiledShader &shader = compile(GL_FRAGMENT_SHADER, kFS);
    EXPECT_TRUE(shader.success());
    if (IsOpenGLES())
    {
        // The following are GLSL qualifiers, so only valid with GLSL translation.
        EXPECT_TRUE(shader.verifyInTranslatedSource("coherent highp int"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("coherent highp ivec2"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("coherent volatile highp ivec3"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("coherent restrict highp ivec4"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("readonly coherent highp float"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("writeonly coherent highp vec2"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("readonly coherent volatile highp vec3"));
    }
    else if (IsOpenGL())
    {
        // The following are GLSL qualifiers, so only valid with GLSL translation.
        EXPECT_TRUE(shader.verifyInTranslatedSource("coherent int"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("coherent ivec2"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("coherent volatile ivec3"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("coherent restrict ivec4"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("readonly coherent float"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("writeonly coherent vec2"));
        EXPECT_TRUE(shader.verifyInTranslatedSource("readonly coherent volatile vec3"));
    }
    reset();
}
}  // namespace

ANGLE_INSTANTIATE_TEST_ES2_AND_ES3(GLSLValidationTest);
ANGLE_INSTANTIATE_TEST_ES2_AND_ES3(GLSLValidationTestNoValidation);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GLSLValidationTest_ES3);
ANGLE_INSTANTIATE_TEST_ES3(GLSLValidationTest_ES3);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GLSLValidationTest_ES31);
ANGLE_INSTANTIATE_TEST_ES31(GLSLValidationTest_ES31);

ANGLE_INSTANTIATE_TEST_ES2(WebGLGLSLValidationTest);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(WebGL2GLSLValidationTest);
ANGLE_INSTANTIATE_TEST_ES3(WebGL2GLSLValidationTest);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GLSLValidationClipDistanceTest_ES3);
ANGLE_INSTANTIATE_TEST_ES3_AND(GLSLValidationClipDistanceTest_ES3,
                               ES3_VULKAN().disable(Feature::SupportsAppleClipDistance));

ANGLE_INSTANTIATE_TEST_ES2(GLSLValidationTextureRectangleTest);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GLSLValidationAtomicCounterTest_ES31);
ANGLE_INSTANTIATE_TEST_ES31(GLSLValidationAtomicCounterTest_ES31);

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GLSLValidationShaderStorageBlockTest_ES31);
ANGLE_INSTANTIATE_TEST_ES31(GLSLValidationShaderStorageBlockTest_ES31);
