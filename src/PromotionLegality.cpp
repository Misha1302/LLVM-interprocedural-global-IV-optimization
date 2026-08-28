#include "PromotionLegality.h"

#include "LoopIterationPath.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace global_iv {

    PromotionLegality::PromotionLegality(const llvm::Module &module, const ModuleEffects &effects)
        : module(module), effects(effects) {
    }

    std::optional<PromotionPlan> PromotionLegality::build_plan(
        llvm::Function &function, llvm::Loop &loop, const llvm::GlobalVariable &global,
        const llvm::DominatorTree &dom_tree
    ) const {
        if (not effects.loop_writes_global(loop, global)) {
            return std::nullopt;
        }

        if (loop_has_unsupported_global_call(loop, global) or loop_has_may_throw_call(loop)
            or effects.global_has_atomic_access(global) or effects.loop_may_synchronize(loop)) {
            return std::nullopt;
        }

        auto *preheader = loop.getLoopPreheader();
        auto *exit_block = loop.getUniqueExitBlock();

        if (not preheader or not exit_block or not loop.hasDedicatedExits()) {
            return std::nullopt;
        }

        if (not function.getEntryBlock().hasInsertionPt() or not preheader->hasInsertionPt()
            or not exit_block->hasInsertionPt()) {
            return std::nullopt;
        }

        if (global.getAddressSpace() != module.getDataLayout().getAllocaAddrSpace()) {
            return std::nullopt;
        }

        const auto needs_initial_value = loop_needs_initial_value(loop, global);
        if (not needs_initial_value) {
            return std::nullopt;
        }

        if (not *needs_initial_value and not loop_has_guaranteed_store(loop, global, dom_tree)) {
            return std::nullopt;
        }

        return PromotionPlan{
            &loop, const_cast<llvm::GlobalVariable *>(&global), preheader, exit_block, *needs_initial_value
        };
    }

    bool PromotionLegality::loop_has_unsupported_global_call(
        const llvm::Loop &loop, const llvm::GlobalVariable &global
    ) const {
        for (const auto *block: loop.blocks()) {
            for (const auto &instruction: *block) {
                const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (not call) {
                    continue;
                }

                if (call->hasFnAttr(llvm::Attribute::ReturnsTwice)) {
                    return true;
                }

                const auto access = effects.get_call_global_access(*call, global);
                if (not access) {
                    return true;
                }

                if (not access->reads and not access->writes) {
                    continue;
                }

                const auto *call_inst = llvm::dyn_cast<llvm::CallInst>(call);
                if (not call_inst or call_inst->isTailCall()) {
                    return true;
                }
            }
        }

        return false;
    }

    bool PromotionLegality::loop_has_may_throw_call(const llvm::Loop &loop) const {
        for (const auto *block: loop.blocks()) {
            for (const auto &instruction: *block) {
                const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (call and call->mayThrow()) {
                    return true;
                }
            }
        }

        return false;
    }

    bool PromotionLegality::loop_has_guaranteed_store(
        const llvm::Loop &loop, const llvm::GlobalVariable &global, const llvm::DominatorTree &dom_tree
    ) const {
        const auto *accesses = effects.get_global_accesses(global);
        if (not accesses) {
            return false;
        }

        llvm::SimpleLoopSafetyInfo safety_info;
        safety_info.computeLoopSafetyInfo(&loop);

        for (const auto &[_, stores]: accesses->stores) {
            for (const auto *store: stores) {
                if (not loop.contains(store)) {
                    continue;
                }

                if (safety_info.isGuaranteedToExecute(*store, &dom_tree, &loop)) {
                    return true;
                }
            }
        }

        return false;
    }

    std::optional<bool> PromotionLegality::loop_needs_initial_value(
        const llvm::Loop &loop, const llvm::GlobalVariable &global
    ) const {
        const auto path = build_loop_iteration_path(loop);
        if (not path) {
            return std::nullopt;
        }

        for (const auto *block: *path) {
            for (const auto &instruction: *block) {
                if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                    if (load->getPointerOperand() == &global) {
                        return true;
                    }

                    continue;
                }

                if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    if (store->getPointerOperand() == &global) {
                        return false;
                    }

                    continue;
                }

                const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (not call) {
                    continue;
                }

                const auto access = effects.get_call_global_access(*call, global);
                if (not access) {
                    return std::nullopt;
                }

                if (access->reads or access->writes) {
                    return true;
                }
            }
        }

        return std::nullopt;
    }

} // namespace global_iv
