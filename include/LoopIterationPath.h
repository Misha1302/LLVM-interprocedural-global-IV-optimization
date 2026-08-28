#pragma once

#include <optional>

#include "llvm/ADT/SmallVector.h"

namespace llvm {
    class BasicBlock;
    class Loop;
} // namespace llvm

namespace global_iv {

    using LoopIterationPath = llvm::SmallVector<const llvm::BasicBlock *>;

    std::optional<LoopIterationPath> build_loop_iteration_path(const llvm::Loop &loop);

} // namespace global_iv
