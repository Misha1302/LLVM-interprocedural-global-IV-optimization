#include "LoopGlobalPromoter.h"

#include <cassert>

#include "ModuleEffects.h"
#include "PromotionLegality.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace global_iv {

    LoopGlobalPromoter::LoopGlobalPromoter(const ModuleEffects &effects) : effects(effects) {
    }

    void LoopGlobalPromoter::promote(llvm::Function &function, const PromotionPlan &plan) const {
        auto *local_slot = create_local_slot(function, plan);

        initialize_local_slot(plan, *local_slot);
        synchronize_global_around_calls(plan, *local_slot);
        redirect_loop_global_accesses(plan, *local_slot);
        write_back_local_slot(plan, *local_slot);
    }

    llvm::Align LoopGlobalPromoter::get_safe_global_access_alignment(const llvm::GlobalVariable &global) const {
        if (const llvm::MaybeAlign alignment = global.getAlign()) {
            return *alignment;
        }

        // No explicit alignment is part of the IR contract here.
        // Using 1 avoids introducing a stronger alignment assumption for the global.
        return llvm::Align(1);
    }

    llvm::Align LoopGlobalPromoter::get_required_local_alignment(const PromotionPlan &plan) const {
        const auto &data_layout = plan.global->getParent()->getDataLayout();
        auto required_alignment = data_layout.getABITypeAlign(plan.global->getValueType());

        if (const llvm::MaybeAlign global_alignment = plan.global->getAlign()) {
            if (*global_alignment > required_alignment) {
                required_alignment = *global_alignment;
            }
        }

        const auto *accesses = effects.get_global_accesses(*plan.global);
        if (not accesses) {
            return required_alignment;
        }

        for (const auto &[_, loads]: accesses->loads) {
            for (const auto *load: loads) {
                if (plan.loop->contains(load) and load->getAlign() > required_alignment) {
                    required_alignment = load->getAlign();
                }
            }
        }

        for (const auto &[_, stores]: accesses->stores) {
            for (const auto *store: stores) {
                if (plan.loop->contains(store) and store->getAlign() > required_alignment) {
                    required_alignment = store->getAlign();
                }
            }
        }

        return required_alignment;
    }

    llvm::AllocaInst *LoopGlobalPromoter::create_local_slot(llvm::Function &function, const PromotionPlan &plan) const {
        auto &entry_block = function.getEntryBlock();

        llvm::IRBuilder<> builder(function.getContext());
        builder.SetInsertPoint(&entry_block, entry_block.getFirstInsertionPt());

        auto *local_slot = builder.CreateAlloca(
            plan.global->getValueType(), nullptr, llvm::Twine(plan.global->getName()) + ".local");

        local_slot->setAlignment(get_required_local_alignment(plan));
        return local_slot;
    }

    void LoopGlobalPromoter::initialize_local_slot(const PromotionPlan &plan, llvm::AllocaInst &local_slot) const {
        if (not plan.needs_initial_value) {
            return;
        }

        llvm::IRBuilder<> builder(plan.preheader->getTerminator());

        auto *initial_value = builder.CreateAlignedLoad(
            plan.global->getValueType(), plan.global, get_safe_global_access_alignment(*plan.global),
            llvm::Twine(plan.global->getName()) + ".initial");

        builder.CreateAlignedStore(initial_value, &local_slot, local_slot.getAlign());
    }

    void LoopGlobalPromoter::synchronize_global_around_calls(
        const PromotionPlan &plan, llvm::AllocaInst &local_slot
    ) const {
        llvm::SmallVector<llvm::CallInst *> calls;

        for (auto *block: plan.loop->blocks()) {
            for (auto &instruction: *block) {
                auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                if (not call) {
                    continue;
                }

                const auto access = effects.get_call_global_access(*call, *plan.global);
                if (not access or (not access->reads and not access->writes)) {
                    continue;
                }

                calls.push_back(call);
            }
        }

        auto *global_type = plan.global->getValueType();

        for (auto *call: calls) {
            const auto access = effects.get_call_global_access(*call, *plan.global);
            assert(access and "unsupported call passed legality checks");

            if (access->reads or access->writes) {
                llvm::IRBuilder<> before_builder(call);

                auto *local_value = before_builder.CreateAlignedLoad(
                    global_type, &local_slot, local_slot.getAlign(),
                    llvm::Twine(plan.global->getName()) + ".before.call");

                before_builder.CreateAlignedStore(
                    local_value, plan.global, get_safe_global_access_alignment(*plan.global));
            }

            if (access->writes) {
                auto *next = call->getNextNode();
                assert(next and "CallInst must have a following instruction");

                llvm::IRBuilder<> after_builder(next);

                auto *global_value = after_builder.CreateAlignedLoad(
                    global_type, plan.global, get_safe_global_access_alignment(*plan.global),
                    llvm::Twine(plan.global->getName()) + ".after.call");

                after_builder.CreateAlignedStore(global_value, &local_slot, local_slot.getAlign());
            }
        }
    }

    void LoopGlobalPromoter::redirect_loop_global_accesses(
        const PromotionPlan &plan, llvm::AllocaInst &local_slot
    ) const {
        const auto *accesses = effects.get_global_accesses(*plan.global);
        if (not accesses) {
            return;
        }

        for (const auto &[_, loads]: accesses->loads) {
            for (const auto *load: loads) {
                if (not plan.loop->contains(load)) {
                    continue;
                }

                auto *mutable_load = const_cast<llvm::LoadInst *>(load);
                mutable_load->setOperand(0, &local_slot);
            }
        }

        for (const auto &[_, stores]: accesses->stores) {
            for (const auto *store: stores) {
                if (not plan.loop->contains(store)) {
                    continue;
                }

                auto *mutable_store = const_cast<llvm::StoreInst *>(store);
                mutable_store->setOperand(1, &local_slot);
            }
        }
    }

    void LoopGlobalPromoter::write_back_local_slot(const PromotionPlan &plan, llvm::AllocaInst &local_slot) const {
        llvm::IRBuilder<> builder(plan.global->getContext());
        builder.SetInsertPoint(plan.exit_block, plan.exit_block->getFirstInsertionPt());

        auto *final_value = builder.CreateAlignedLoad(
            plan.global->getValueType(), &local_slot, local_slot.getAlign(),
            llvm::Twine(plan.global->getName()) + ".final");

        builder.CreateAlignedStore(final_value, plan.global, get_safe_global_access_alignment(*plan.global));
    }

} // namespace global_iv
