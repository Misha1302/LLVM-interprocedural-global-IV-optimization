#include "FunctionEvolutionState.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

namespace global_iv {

    const Evolution *FunctionEvolutionState::get_variable_evolution(const llvm::GlobalVariable &global) const {
        const auto it = variable_evolutions.find(&global);
        if (it == variable_evolutions.end()) {
            return nullptr;
        }

        return &it->second;
    }

    const Evolution *FunctionEvolutionState::get_load_evolution(const llvm::LoadInst &load) const {
        const auto it = load_evolutions.find(&load);
        if (it == load_evolutions.end()) {
            return nullptr;
        }

        return &it->second;
    }

    const llvm::Function *FunctionEvolutionState::get_current_function() const {
        return analyzing_stack.back();
    }

    bool FunctionEvolutionState::is_current_function_recursive() const {
        return analyzing_functions.count(get_current_function()) >= 2;
    }

    void FunctionEvolutionState::push_function(const llvm::Function &function) {
        analyzing_functions.insert(&function);
        analyzing_stack.push_back(&function);
    }

    void FunctionEvolutionState::pop_function() {
        const auto *function = analyzing_stack.back();
        analyzing_functions.erase(analyzing_functions.find(function));
        analyzing_stack.pop_back();
    }

    void FunctionEvolutionState::set_variable_evolution(
        const llvm::GlobalVariable &global, const Evolution &evolution
    ) {
        variable_evolutions[&global] = evolution;
    }

    void FunctionEvolutionState::set_variable_unknown(const llvm::GlobalVariable &global) {
        variable_evolutions[&global] = make_unknown_evolution();
    }

    void FunctionEvolutionState::compose_variable_evolution(
        const llvm::GlobalVariable &global, const Evolution &after
    ) {
        if (after.type == EvolutionType::Unknown) {
            set_variable_unknown(global);
            return;
        }

        const auto it = variable_evolutions.find(&global);
        if (it == variable_evolutions.end()) {
            variable_evolutions[&global] = after;
            return;
        }

        const auto &before = it->second;
        if (before.type == EvolutionType::Unknown) {
            return;
        }

        const auto &before_linear = before.linear;
        const auto &after_linear = after.linear;

        variable_evolutions[&global]
                = {EvolutionType::Linear,
                {after_linear.b + after_linear.k * before_linear.b, after_linear.k * before_linear.k}};
    }

    void FunctionEvolutionState::remember_load(const llvm::LoadInst &load, const llvm::GlobalVariable &global) {
        if (variable_evolutions.contains(&global)) {
            load_evolutions[&load] = variable_evolutions[&global];
        } else {
            load_evolutions[&load] = make_identity_evolution(global);
        }
    }

} // namespace global_iv
