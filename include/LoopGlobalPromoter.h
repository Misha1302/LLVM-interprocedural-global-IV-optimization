#pragma once

#include "ModuleEffects.h"
#include "ModuleEffects.h"
#include "PromotionLegality.h"

#include "llvm/Support/Alignment.h"

namespace llvm {
    class AllocaInst;
    class Function;
    class GlobalVariable;
} // namespace llvm

namespace global_iv {

    /// Applies a previously validated promotion plan to LLVM IR.
    class LoopGlobalPromoter {
        const ModuleEffects &effects;

    public:
        explicit LoopGlobalPromoter(const ModuleEffects &effects);

        void promote(llvm::Function &function, const PromotionPlan &plan) const;

    private:
        llvm::Align get_safe_global_access_alignment(const llvm::GlobalVariable &global) const;
        llvm::Align get_required_local_alignment(const PromotionPlan &plan) const;
        llvm::AllocaInst *create_local_slot(llvm::Function &function, const PromotionPlan &plan) const;
        void initialize_local_slot(const PromotionPlan &plan, llvm::AllocaInst &local_slot) const;
        void synchronize_global_around_calls(const PromotionPlan &plan, llvm::AllocaInst &local_slot) const;
        void redirect_loop_global_accesses(const PromotionPlan &plan, llvm::AllocaInst &local_slot) const;
        void write_back_local_slot(const PromotionPlan &plan, llvm::AllocaInst &local_slot) const;
    };

} // namespace global_iv
