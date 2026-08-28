#pragma once

#include <optional>

#include "ModuleEffects.h"

namespace llvm {
    class BasicBlock;
    class DominatorTree;
    class Function;
    class GlobalVariable;
    class Loop;
    class Module;
} // namespace llvm

namespace global_iv {

    struct PromotionPlan {
        llvm::Loop *loop;
        llvm::GlobalVariable *global;
        llvm::BasicBlock *preheader;
        llvm::BasicBlock *exit_block;
        bool needs_initial_value;
    };

    /// Converts a candidate into a promotion plan only when every legality check succeeds.
    class PromotionLegality {
        const llvm::Module &module;
        const ModuleEffects &effects;

    public:
        PromotionLegality(const llvm::Module &module, const ModuleEffects &effects);

        std::optional<PromotionPlan> build_plan(
            llvm::Function &function, llvm::Loop &loop, const llvm::GlobalVariable &global,
            const llvm::DominatorTree &dom_tree
        ) const;

    private:
        bool loop_has_unsupported_global_call(const llvm::Loop &loop, const llvm::GlobalVariable &global) const;
        bool loop_has_may_throw_call(const llvm::Loop &loop) const;
        bool loop_has_guaranteed_store(
            const llvm::Loop &loop, const llvm::GlobalVariable &global, const llvm::DominatorTree &dom_tree
        ) const;
        std::optional<bool> loop_needs_initial_value(const llvm::Loop &loop, const llvm::GlobalVariable &global) const;
    };

} // namespace global_iv
