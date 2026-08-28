#pragma once

#include <optional>

#include "Evolution.h"
#include "FunctionEvolutionState.h"

namespace llvm {
    class LoadInst;
    class Loop;
    class Module;
    class StoreInst;
    class Value;
} // namespace llvm

namespace global_iv {

    /// Computes affine global evolution for functions and supported loop iterations.
    class EvolutionAnalyzer {
        const llvm::Module &module;

    public:
        explicit EvolutionAnalyzer(const llvm::Module &module);

        FunctionGlobalEvolution analyze_functions();
        std::optional<llvm::DenseMap<const llvm::GlobalVariable *, Evolution>> analyze_loop_iteration(
            const llvm::Loop &loop, const FunctionGlobalEvolution &function_evolutions
        );

    private:
        void analyze_function(FunctionEvolutionState &state);
        void process_load(FunctionEvolutionState &state, const llvm::LoadInst &load);
        void process_store(FunctionEvolutionState &state, const llvm::StoreInst &store);
        void set_all_globals_unknown(FunctionEvolutionState &state);
        std::optional<EvaluatedValue> evaluate_linear_value(
            const llvm::Value *value, const FunctionEvolutionState &state
        );
    };

} // namespace global_iv
