#pragma once

#include "llvm/IR/PassManager.h"

namespace global_iv {

    /// Orchestrates analysis, legality checking, and IR transformation.
    class GlobalIVPass : public llvm::PassInfoMixin<GlobalIVPass> {
    public:
        llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &mam);
    };

} // namespace global_iv
