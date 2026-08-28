#pragma once

#include <optional>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
    class CallBase;
    class Function;
    class GlobalVariable;
    class LoadInst;
    class Loop;
    class Module;
    class StoreInst;
} // namespace llvm

namespace global_iv {

    struct GlobalAccesses {
        llvm::DenseMap<const llvm::Function *, llvm::SmallVector<const llvm::LoadInst *>> loads;
        llvm::DenseMap<const llvm::Function *, llvm::SmallVector<const llvm::StoreInst *>> stores;
    };

    struct GlobalAccessSummary {
        bool reads = false;
        bool writes = false;
    };

    using GlobalAccessMap = llvm::DenseMap<const llvm::GlobalVariable *, GlobalAccesses>;
    using FunctionGlobalAccessSummary
        = llvm::DenseMap<const llvm::Function *, llvm::DenseMap<const llvm::GlobalVariable *, GlobalAccessSummary>>;
    using FunctionSynchronizationSummary = llvm::DenseMap<const llvm::Function *, bool>;

    /// Immutable module-effect summaries consumed by legality and transformation.
    class ModuleEffects {
        GlobalAccessMap global_accesses;
        FunctionGlobalAccessSummary access_summary;
        FunctionSynchronizationSummary synchronization_summary;

    public:
        ModuleEffects(
            GlobalAccessMap global_accesses, FunctionGlobalAccessSummary access_summary,
            FunctionSynchronizationSummary synchronization_summary
        );

        [[nodiscard]] const GlobalAccesses *get_global_accesses(const llvm::GlobalVariable &global) const;
        [[nodiscard]] std::optional<GlobalAccessSummary> get_call_global_access(
            const llvm::CallBase &call, const llvm::GlobalVariable &global
        ) const;
        [[nodiscard]] bool global_has_atomic_access(const llvm::GlobalVariable &global) const;
        [[nodiscard]] bool loop_may_synchronize(const llvm::Loop &loop) const;
        [[nodiscard]] bool loop_writes_global(const llvm::Loop &loop, const llvm::GlobalVariable &global) const;
    };

    /// Builds direct and transitive global-access and synchronization summaries.
    class ModuleEffectsAnalyzer {
        llvm::Module &module;

    public:
        explicit ModuleEffectsAnalyzer(llvm::Module &module);

        ModuleEffects analyze();

    private:
        GlobalAccessMap collect_global_accesses();
        FunctionGlobalAccessSummary build_direct_access_summary(const GlobalAccessMap &global_accesses);
        FunctionGlobalAccessSummary build_transitive_access_summary(
            const FunctionGlobalAccessSummary &direct_summary, llvm::ArrayRef<const llvm::GlobalVariable *> globals
        );
        FunctionSynchronizationSummary build_direct_synchronization_summary();
        FunctionSynchronizationSummary build_transitive_synchronization_summary(
            const FunctionSynchronizationSummary &direct_summary
        );
        bool merge_access_summary(GlobalAccessSummary &dst, const GlobalAccessSummary &src);
    };

} // namespace global_iv
