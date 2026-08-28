#include "EvolutionAnalyzer.h"

#include <unordered_set>

#include "GlobalEligibility.h"
#include "LoopIterationPath.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace global_iv {

    EvolutionAnalyzer::EvolutionAnalyzer(const llvm::Module &module) : module(module) {
    }

    FunctionGlobalEvolution EvolutionAnalyzer::analyze_functions() {
        FunctionGlobalEvolution result;

        for (const auto &function: module.functions()) {
            if (function.isDeclaration()) {
                continue;
            }

            FunctionEvolutionState state;
            state.push_function(function);
            analyze_function(state);

            for (const auto &global: module.globals()) {
                if (not can_analyze_iv_global(global)) {
                    continue;
                }

                if (const auto *evolution = state.get_variable_evolution(global)) {
                    result[&function][&global] = *evolution;
                } else {
                    result[&function][&global] = make_identity_evolution(global);
                }
            }
        }

        return result;
    }

    std::optional<llvm::DenseMap<const llvm::GlobalVariable *, Evolution>> EvolutionAnalyzer::analyze_loop_iteration(
        const llvm::Loop &loop, const FunctionGlobalEvolution &function_evolutions
    ) {
        if (not loop.isInnermost()) {
            return std::nullopt;
        }

        const auto path = build_loop_iteration_path(loop);
        if (not path) {
            return std::nullopt;
        }

        FunctionEvolutionState state;

        for (const auto *block: *path) {
            for (const auto &instruction: *block) {
                if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                    process_load(state, *load);
                    continue;
                }

                if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    const auto *callee = call->getCalledFunction();
                    if (not callee) {
                        set_all_globals_unknown(state);
                        continue;
                    }

                    const auto function_it = function_evolutions.find(callee);
                    if (function_it == function_evolutions.end()) {
                        set_all_globals_unknown(state);
                        continue;
                    }

                    for (const auto &[global, evolution]: function_it->second) {
                        state.compose_variable_evolution(*global, evolution);
                    }

                    continue;
                }

                if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    process_store(state, *store);
                }
            }
        }

        llvm::DenseMap<const llvm::GlobalVariable *, Evolution> result;

        for (const auto &global: module.globals()) {
            if (not can_analyze_iv_global(global)) {
                continue;
            }

            if (const auto *evolution = state.get_variable_evolution(global)) {
                result[&global] = *evolution;
            } else {
                result[&global] = make_identity_evolution(global);
            }
        }

        return result;
    }

    void EvolutionAnalyzer::analyze_function(FunctionEvolutionState &state) {
        if (state.is_current_function_recursive()) {
            set_all_globals_unknown(state);
            state.pop_function();
            return;
        }

        const auto *function = state.get_current_function();
        const auto *block = &function->getEntryBlock();

        std::unordered_set<const llvm::BasicBlock *> visited;

        while (true) {
            if (not visited.insert(block).second) {
                set_all_globals_unknown(state);
                state.pop_function();
                return;
            }

            const auto *terminator = block->getTerminator();

            if (terminator->getNumSuccessors() > 1) {
                set_all_globals_unknown(state);
                state.pop_function();
                return;
            }

            if (block != &function->getEntryBlock() and llvm::pred_size(block) != 1) {
                set_all_globals_unknown(state);
                state.pop_function();
                return;
            }

            for (const auto &instruction: *block) {
                if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
                    process_load(state, *load);
                    continue;
                }

                if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    const auto *callee = call->getCalledFunction();

                    if (not callee or callee->isDeclaration()) {
                        set_all_globals_unknown(state);
                        continue;
                    }

                    state.push_function(*callee);
                    analyze_function(state);
                    continue;
                }

                if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    process_store(state, *store);
                }
            }

            if (terminator->getNumSuccessors() == 0) {
                break;
            }

            block = terminator->getSuccessor(0);
        }

        state.pop_function();
    }

    void EvolutionAnalyzer::process_load(FunctionEvolutionState &state, const llvm::LoadInst &load) {
        const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(load.getPointerOperand());
        if (not global or not can_analyze_iv_global(*global)) {
            return;
        }

        state.remember_load(load, *global);
    }

    void EvolutionAnalyzer::process_store(FunctionEvolutionState &state, const llvm::StoreInst &store) {
        const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(store.getPointerOperand());
        if (not global or not can_analyze_iv_global(*global)) {
            return;
        }

        const auto evaluated = evaluate_linear_value(store.getValueOperand(), state);
        if (not evaluated or (evaluated->global and evaluated->global != global)) {
            state.set_variable_unknown(*global);
            return;
        }

        state.set_variable_evolution(*global, evaluated->evolution);
    }

    void EvolutionAnalyzer::set_all_globals_unknown(FunctionEvolutionState &state) {
        for (const auto &global: module.globals()) {
            if (can_analyze_iv_global(global)) {
                state.set_variable_unknown(global);
            }
        }
    }

    std::optional<EvaluatedValue> EvolutionAnalyzer::evaluate_linear_value(
        const llvm::Value *value, const FunctionEvolutionState &state
    ) {
        if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
            const auto &constant_value = constant->getValue();

            return EvaluatedValue{
                nullptr, {EvolutionType::Linear, {constant_value, llvm::APInt::getZero(constant_value.getBitWidth())}}
            };
        }

        if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
            const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(load->getPointerOperand());
            if (not global or not can_analyze_iv_global(*global)) {
                return std::nullopt;
            }

            const auto *evolution = state.get_load_evolution(*load);
            if (not evolution or evolution->type != EvolutionType::Linear) {
                return std::nullopt;
            }

            return EvaluatedValue{global, *evolution};
        }

        const auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value);
        if (not binary) {
            return std::nullopt;
        }

        const auto lhs = evaluate_linear_value(binary->getOperand(0), state);
        const auto rhs = evaluate_linear_value(binary->getOperand(1), state);

        if (not lhs or not rhs) {
            return std::nullopt;
        }

        if (lhs->global and rhs->global and lhs->global != rhs->global) {
            return std::nullopt;
        }

        const auto *global = lhs->global ? lhs->global : rhs->global;
        const auto &lhs_linear = lhs->evolution.linear;
        const auto &rhs_linear = rhs->evolution.linear;

        switch (binary->getOpcode()) {
            case llvm::Instruction::Add:
                return EvaluatedValue{
                    global, {EvolutionType::Linear, {lhs_linear.b + rhs_linear.b, lhs_linear.k + rhs_linear.k}}
                };

            case llvm::Instruction::Sub:
                return EvaluatedValue{
                    global, {EvolutionType::Linear, {lhs_linear.b - rhs_linear.b, lhs_linear.k - rhs_linear.k}}
                };

            case llvm::Instruction::Mul:
                if (lhs_linear.k.isZero()) {
                    return EvaluatedValue{
                        global, {EvolutionType::Linear, {lhs_linear.b * rhs_linear.b, lhs_linear.b * rhs_linear.k}}
                    };
                }

                if (rhs_linear.k.isZero()) {
                    return EvaluatedValue{
                        global, {EvolutionType::Linear, {rhs_linear.b * lhs_linear.b, rhs_linear.b * lhs_linear.k}}
                    };
                }

                return std::nullopt;

            default:
                return std::nullopt;
        }
    }

} // namespace global_iv
