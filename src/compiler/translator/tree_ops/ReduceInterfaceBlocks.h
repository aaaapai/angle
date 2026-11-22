//
// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#ifndef COMPILER_TRANSLATOR_TREEOPS_MSL_REDUCEINTERFACEBLOCKS_H_
#define COMPILER_TRANSLATOR_TREEOPS_MSL_REDUCEINTERFACEBLOCKS_H_

#include <functional>

#include "common/angleutils.h"
#include "compiler/translator/Compiler.h"

namespace sh
{
class TSymbolTable;

using InterfaceBlockInstanceVarNameGen = std::function<ImmutableString()>;

// This rewrites interface block declarations only.
//
// Example:
//  uniform Foo { int x; } foo;
// Becomes:
//  struct Foo { int x; }; uniform Foo x;
//

// How shader storage blocks (GLSL `buffer` blocks) are handled.  Backends whose translator cannot
// lower them use Reject, which fails the translation with a diagnostic instead of emitting
// invalid code.
enum class ShaderStorageBlockPolicy
{
    Reduce,
    Reject,
};

[[nodiscard]] bool ReduceInterfaceBlocks(
    TCompiler &compiler,
    TIntermBlock &root,
    InterfaceBlockInstanceVarNameGen nameGen,
    ShaderStorageBlockPolicy storageBlockPolicy = ShaderStorageBlockPolicy::Reduce);

}  // namespace sh

#endif  // COMPILER_TRANSLATOR_TREEOPS_MSL_REDUCEINTERFACEBLOCKS_H_
