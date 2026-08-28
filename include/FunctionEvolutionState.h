#pragma once

#include <set>

#include "Evolution.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
    class Function;
    class GlobalVariable;
    class LoadInst;
} // namespace llvm

namespace global_iv {

    /// Mutable analysis state shared across one interprocedural evolution walk.
    class FunctionEvolutionState {
        std::multiset<const llvm::Function *> analyzing_functions;
        llvm::SmallVector<const llvm::Function *> analyzing_stack;

        llvm::DenseMap<const llvm::GlobalVariable *, Evolution> variable_evolutions;
        llvm::DenseMap<const llvm::LoadInst *, Evolution> load_evolutions;

    public:
        [[nodiscard]] const Evolution *get_variable_evolution(const llvm::GlobalVariable &global) const;
        [[nodiscard]] const Evolution *get_load_evolution(const llvm::LoadInst &load) const;
        [[nodiscard]] const llvm::Function *get_current_function() const;
        [[nodiscard]] bool is_current_function_recursive() const;

        void push_function(const llvm::Function &function);
        void pop_function();

        void set_variable_evolution(const llvm::GlobalVariable &global, const Evolution &evolution);
        void set_variable_unknown(const llvm::GlobalVariable &global);
        void compose_variable_evolution(const llvm::GlobalVariable &global, const Evolution &after);
        void remember_load(const llvm::LoadInst &load, const llvm::GlobalVariable &global);
    };

} // namespace global_iv
