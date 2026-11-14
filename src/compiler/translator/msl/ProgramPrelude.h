//
// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#ifndef COMPILER_TRANSLATOR_MSL_PROGRAMPRELUDE_H_
#define COMPILER_TRANSLATOR_MSL_PROGRAMPRELUDE_H_

#include <array>
#include <unordered_set>

#include "common/angleutils.h"

namespace sh
{

class TInfoSinkBase;
class TIntermBlock;

// Name of the MSL constant that carries gl_WorkGroupSize.  It is emitted by ProgramPrelude for
// compute programs and referenced by EmitMetal so that the value remains a constant expression.
constexpr const char kWorkGroupSizeConstName[] = "ANGLE_WorkGroupSize";

enum class MetalShaderType
{
    None,
    Vertex,
    Fragment,
    Compute,
    Count,
};

struct ProgramPreludeConfig
{
  public:
    ProgramPreludeConfig() {}
    explicit ProgramPreludeConfig(MetalShaderType shaderType) : shaderType(shaderType) {}
    bool usesDerivatives       = false;
    MetalShaderType shaderType = MetalShaderType::None;
    // Only meaningful for compute; must match TCompiler::getComputeShaderLocalSize().
    std::array<int, 3> computeLocalSize = {1, 1, 1};
};

// This emits fixed helper Metal code directly without adding code to the AST. This walks the AST to
// figure out the required what prelude MSL code is needed for various things in the AST. You can
// think of this as effectively inlining various portions of a helper library into the emitted
// Metal program.
[[nodiscard]] bool EmitProgramPrelude(TIntermBlock &root,
                                      TInfoSinkBase &out,
                                      const ProgramPreludeConfig &ppc);

}  // namespace sh

#endif  // COMPILER_TRANSLATOR_MSL_PROGRAMPRELUDE_H_
