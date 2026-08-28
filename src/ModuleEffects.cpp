#include "ModuleEffects.h"

#include <utility>

#include "GlobalEligibility.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/AtomicOrdering.h"
#include "llvm/Support/ErrorHandling.h"

namespace {

    bool instruction_may_synchronize(const llvm::Instruction &instruction) {
        if (llvm::isa<llvm::FenceInst>(instruction) or llvm::isa<llvm::AtomicRMWInst>(instruction)
            or llvm::isa<llvm::AtomicCmpXchgInst>(instruction)) {
            return true;
        }

        if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
            return llvm::isStrongerThanUnordered(load->getOrdering());
        }

        if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            return llvm::isStrongerThanUnordered(store->getOrdering());
        }

        return false;
    }

} // namespace

namespace global_iv {

    ModuleEffects::ModuleEffects(
        GlobalAccessMap global_accesses, FunctionGlobalAccessSummary access_summary,
        FunctionSynchronizationSummary synchronization_summary
    )
        : global_accesses(std::move(global_accesses)), access_summary(std::move(access_summary)),
          synchronization_summary(std::move(synchronization_summary)) {
    }

    const GlobalAccesses *ModuleEffects::get_global_accesses(const llvm::GlobalVariable &global) const {
        const auto it = global_accesses.find(&global);
        return it == global_accesses.end() ? nullptr : &it->second;
    }

    std::optional<GlobalAccessSummary> ModuleEffects::get_call_global_access(
        const llvm::CallBase &call, const llvm::GlobalVariable &global
    ) const {
        const auto *callee = call.getCalledFunction();
        if (not callee or callee->isDeclaration()) {
            return std::nullopt;
        }

        const auto function_it = access_summary.find(callee);
        if (function_it == access_summary.end()) {
            return GlobalAccessSummary{};
        }

        const auto global_it = function_it->second.find(&global);
        if (global_it == function_it->second.end()) {
            return GlobalAccessSummary{};
        }

        return global_it->second;
    }

    bool ModuleEffects::global_has_atomic_access(const llvm::GlobalVariable &global) const {
        const auto *accesses = get_global_accesses(global);
        if (not accesses) {
            return false;
        }

        for (const auto &[_, loads]: accesses->loads) {
            for (const auto *load: loads) {
                if (load->isAtomic()) {
                    return true;
                }
            }
        }

        for (const auto &[_, stores]: accesses->stores) {
            for (const auto *store: stores) {
                if (store->isAtomic()) {
                    return true;
                }
            }
        }

        return false;
    }

    bool ModuleEffects::loop_may_synchronize(const llvm::Loop &loop) const {
        for (const auto *block: loop.blocks()) {
            for (const auto &instruction: *block) {
                if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    const auto *callee = call->getCalledFunction();

                    if (not callee or callee->isDeclaration()) {
                        if (not call->hasFnAttr(llvm::Attribute::NoSync)) {
                            return true;
                        }

                        continue;
                    }

                    if (synchronization_summary.lookup(callee)) {
                        return true;
                    }

                    continue;
                }

                if (instruction_may_synchronize(instruction)) {
                    return true;
                }
            }
        }

        return false;
    }

    bool ModuleEffects::loop_writes_global(const llvm::Loop &loop, const llvm::GlobalVariable &global) const {
        const auto *accesses = get_global_accesses(global);
        if (not accesses) {
            return false;
        }

        for (const auto &[_, stores]: accesses->stores) {
            for (const auto *store: stores) {
                if (loop.contains(store)) {
                    return true;
                }
            }
        }

        return false;
    }

    ModuleEffectsAnalyzer::ModuleEffectsAnalyzer(llvm::Module &module) : module(module) {
    }

    ModuleEffects ModuleEffectsAnalyzer::analyze() {
        auto global_accesses = collect_global_accesses();

        llvm::SmallVector<const llvm::GlobalVariable *> tracked_globals;
        tracked_globals.reserve(global_accesses.size());

        for (const auto &[global, _]: global_accesses) {
            tracked_globals.push_back(global);
        }

        const auto direct_access_summary = build_direct_access_summary(global_accesses);
        auto access_summary = build_transitive_access_summary(direct_access_summary, tracked_globals);

        const auto direct_synchronization_summary = build_direct_synchronization_summary();
        auto synchronization_summary = build_transitive_synchronization_summary(direct_synchronization_summary);

        return ModuleEffects{std::move(global_accesses), std::move(access_summary), std::move(synchronization_summary)};
    }

    GlobalAccessMap ModuleEffectsAnalyzer::collect_global_accesses() {
        GlobalAccessMap result;

        for (auto &global: module.globals()) {
            if (not can_analyze_iv_global(global)) {
                continue;
            }

            auto &accesses = result[&global];

            for (auto *user: global.users()) {
                if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(user)) {
                    accesses.loads[load->getFunction()].push_back(load);
                    continue;
                }

                if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
                    accesses.stores[store->getFunction()].push_back(store);
                    continue;
                }

                llvm_unreachable(
                    "canTrackGlobalVariableInterprocedurally guaranteed "
                    "load/store-only users"
                );
            }
        }

        return result;
    }

    FunctionGlobalAccessSummary ModuleEffectsAnalyzer::build_direct_access_summary(
        const GlobalAccessMap &global_accesses
    ) {
        FunctionGlobalAccessSummary result;

        for (const auto &[global, accesses]: global_accesses) {
            for (const auto &[function, loads]: accesses.loads) {
                if (not loads.empty()) {
                    result[function][global].reads = true;
                }
            }

            for (const auto &[function, stores]: accesses.stores) {
                if (not stores.empty()) {
                    result[function][global].writes = true;
                }
            }
        }

        return result;
    }

    FunctionGlobalAccessSummary ModuleEffectsAnalyzer::build_transitive_access_summary(
        const FunctionGlobalAccessSummary &direct_summary, llvm::ArrayRef<const llvm::GlobalVariable *> globals
    ) {
        auto result = direct_summary;

        bool changed;
        do {
            changed = false;

            for (const auto &caller: module) {
                if (caller.isDeclaration()) {
                    continue;
                }

                for (const auto &instruction: llvm::instructions(caller)) {
                    const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (not call) {
                        continue;
                    }

                    const auto *callee = call->getCalledFunction();

                    if (not callee or callee->isDeclaration()) {
                        for (const auto *global: globals) {
                            changed |= merge_access_summary(result[&caller][global], {.reads = true, .writes = true});
                        }

                        continue;
                    }

                    const auto callee_it = result.find(callee);
                    if (callee_it == result.end()) {
                        continue;
                    }

                    for (const auto &[global, callee_summary]: callee_it->second) {
                        changed |= merge_access_summary(result[&caller][global], callee_summary);
                    }
                }
            }
        } while (changed);

        return result;
    }

    FunctionSynchronizationSummary ModuleEffectsAnalyzer::build_direct_synchronization_summary() {
        FunctionSynchronizationSummary result;

        for (const auto &function: module) {
            if (function.isDeclaration()) {
                continue;
            }

            bool may_synchronize = false;

            for (const auto &instruction: llvm::instructions(function)) {
                if (llvm::isa<llvm::CallBase>(instruction)) {
                    continue;
                }

                if (instruction_may_synchronize(instruction)) {
                    may_synchronize = true;
                    break;
                }
            }

            result[&function] = may_synchronize;
        }

        return result;
    }

    FunctionSynchronizationSummary ModuleEffectsAnalyzer::build_transitive_synchronization_summary(
        const FunctionSynchronizationSummary &direct_summary
    ) {
        auto result = direct_summary;

        bool changed;
        do {
            changed = false;

            for (const auto &caller: module) {
                if (caller.isDeclaration()) {
                    continue;
                }

                for (const auto &instruction: llvm::instructions(caller)) {
                    const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (not call) {
                        continue;
                    }

                    const auto *callee = call->getCalledFunction();
                    const bool call_may_synchronize = not callee or callee->isDeclaration()
                                                          ? not call->hasFnAttr(llvm::Attribute::NoSync)
                                                          : result.lookup(callee);

                    if (call_may_synchronize and not result.lookup(&caller)) {
                        result[&caller] = true;
                        changed = true;
                    }
                }
            }
        } while (changed);

        return result;
    }

    bool ModuleEffectsAnalyzer::merge_access_summary(GlobalAccessSummary &dst, const GlobalAccessSummary &src) {
        const bool old_reads = dst.reads;
        const bool old_writes = dst.writes;

        dst.reads |= src.reads;
        dst.writes |= src.writes;

        return old_reads != dst.reads or old_writes != dst.writes;
    }

} // namespace global_iv
