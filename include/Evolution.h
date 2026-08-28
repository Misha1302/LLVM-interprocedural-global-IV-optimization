#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"

namespace llvm {
    class Function;
    class GlobalVariable;
} // namespace llvm

namespace global_iv {

    enum class EvolutionType {
        Unknown,
        Linear
    };

    struct LinearEvolution {
        // b + k * x, modulo 2^N
        llvm::APInt b;
        llvm::APInt k;
    };

    struct Evolution {
        EvolutionType type;
        LinearEvolution linear;
    };

    struct EvaluatedValue {
        const llvm::GlobalVariable *global;
        Evolution evolution;
    };

    using FunctionGlobalEvolution
        = llvm::DenseMap<const llvm::Function *, llvm::DenseMap<const llvm::GlobalVariable *, Evolution>>;

    Evolution make_unknown_evolution();
    Evolution make_identity_evolution(const llvm::GlobalVariable &global);

} // namespace global_iv
