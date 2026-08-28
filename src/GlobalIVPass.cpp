#include <optional>
#include <set>
#include <unordered_set>
#include <utility>

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/Analysis/ValueLatticeUtils.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

#include "llvm/Support/Alignment.h"
#include "llvm/Support/AtomicOrdering.h"

using namespace llvm;

namespace {
    struct GlobalAccesses {
        DenseMap<const Function *, SmallVector<const LoadInst *>> loads;
        DenseMap<const Function *, SmallVector<const StoreInst *>> stores;
    };

    struct GlobalAccessSummary {
        bool reads = false;
        bool writes = false;
    };

    enum class EvolutionType {
        Unknown,
        Linear
    };

    struct LinearEvolution {
        // b + k * x, modulo 2^N
        APInt b;
        APInt k;
    };

    struct Evolution {
        EvolutionType function_evolution_type;
        LinearEvolution function_linear_evolution;
    };

    Evolution make_unknown_evolution() {
        return {EvolutionType::Unknown, {APInt::getZero(1), APInt::getZero(1)}};
    }

    Evolution make_identity_evolution(const GlobalVariable &global) {
        const auto *integer_type = cast<IntegerType>(global.getValueType());
        const auto bit_width = integer_type->getBitWidth();

        return {EvolutionType::Linear, {APInt::getZero(bit_width), APInt(bit_width, 1)}};
    }

    struct GlobalEvolutionCandidate {
        const Loop *loop;
        const GlobalVariable *global;
    };

    class FuncDfsState {
        std::multiset<const Function *> analyzing_functions_multiset;
        SmallVector<const Function *> analyzing_functions_stack;

        DenseMap<const GlobalVariable *, Evolution> variable_to_evolution;
        DenseMap<const LoadInst *, Evolution> load_to_evolution;

    public:
        [[nodiscard]] const Evolution *get_variable_evolution(const GlobalVariable &global) const {
            const auto it = variable_to_evolution.find(&global);
            if (it == variable_to_evolution.end()) {
                return nullptr;
            }

            return &it->second;
        }

        const Function *get_current_function() {
            return analyzing_functions_stack.back();
        }

        void pop_function() {
            analyzing_functions_multiset.erase(analyzing_functions_multiset.find(analyzing_functions_stack.back()));

            analyzing_functions_stack.pop_back();
        }

        void push_function(const Function *func) {
            analyzing_functions_multiset.insert(func);
            analyzing_functions_stack.push_back(func);
        }

        void set_variable_evolution(const GlobalVariable &global, const Evolution &evolution) {
            variable_to_evolution[&global] = evolution;
        }

        void set_variable_unknown(const GlobalVariable &global) {
            variable_to_evolution[&global] = make_unknown_evolution();
        }

        void compose_variable_evolution(const GlobalVariable &global, const Evolution &after) {
            if (after.function_evolution_type == EvolutionType::Unknown) {
                set_variable_unknown(global);
                return;
            }

            const auto it = variable_to_evolution.find(&global);

            if (it == variable_to_evolution.end()) {
                variable_to_evolution[&global] = after;
                return;
            }

            const auto &before = it->second;

            if (before.function_evolution_type == EvolutionType::Unknown) {
                return;
            }

            const auto &before_linear = before.function_linear_evolution;
            const auto &after_linear = after.function_linear_evolution;

            variable_to_evolution[&global] = {EvolutionType::Linear,
                {after_linear.b + after_linear.k * before_linear.b, after_linear.k * before_linear.k}};
        }

        void remember_load(const LoadInst &load, const GlobalVariable &global) {
            if (variable_to_evolution.contains(&global)) {
                load_to_evolution[&load] = variable_to_evolution[&global];
            } else {
                load_to_evolution[&load] = make_identity_evolution(global);
            }
        }

        [[nodiscard]] const Evolution *get_load_evolution(const LoadInst &load) const {
            const auto it = load_to_evolution.find(&load);
            if (it == load_to_evolution.end()) {
                return nullptr;
            }

            return &it->second;
        }

        bool is_func_in_analyzing_stack_double(const Function *function) const {
            return analyzing_functions_multiset.count(function) >= 2;
        }
    };

    class GlobalIVPass : public PassInfoMixin<GlobalIVPass> {
        using GlobalAccessMap = DenseMap<const GlobalVariable *, GlobalAccesses>;

        using FunctionGlobalAccessSummary
            = DenseMap<const Function *, DenseMap<const GlobalVariable *, GlobalAccessSummary>>;

        using FunctionSynchronizationSummary = DenseMap<const Function *, bool>;

        bool can_analyze_iv_global(const GlobalVariable &global) {
            return global.getValueType()->isIntegerTy()
                   and canTrackGlobalVariableInterprocedurally(const_cast<GlobalVariable *>(&global));
        }

        GlobalAccessMap collect_global_accesses(Module &module) {
            GlobalAccessMap result;

            for (auto &global: module.globals()) {
                if (not can_analyze_iv_global(global)) {
                    continue;
                }

                auto &accesses = result[&global];

                for (User *user: global.users()) {
                    if (const auto *load = dyn_cast<LoadInst>(user)) {
                        const auto *function = load->getFunction();
                        accesses.loads[function].push_back(load);
                        continue;
                    }

                    if (const auto *store = dyn_cast<StoreInst>(user)) {
                        const auto *function = store->getFunction();
                        accesses.stores[function].push_back(store);
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

        FunctionGlobalAccessSummary build_direct_access_summary(const GlobalAccessMap &global_accesses) {
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

        bool merge_access_summary(GlobalAccessSummary &dst, const GlobalAccessSummary &src) {
            const bool old_reads = dst.reads;
            const bool old_writes = dst.writes;

            dst.reads |= src.reads;
            dst.writes |= src.writes;

            return old_reads != dst.reads or old_writes != dst.writes;
        }

        FunctionGlobalAccessSummary build_transitive_access_summary(
            const Module &module, const FunctionGlobalAccessSummary &direct_summary,
            const ArrayRef<const GlobalVariable *> &globals
        ) {
            auto result = direct_summary;

            bool changed;

            do {
                changed = false;

                for (const Function &caller: module) {
                    if (caller.isDeclaration()) {
                        continue;
                    }

                    for (const Instruction &instruction: instructions(caller)) {
                        const auto *call = dyn_cast<CallBase>(&instruction);
                        if (not call) {
                            continue;
                        }

                        const auto *callee = call->getCalledFunction();

                        if (not callee or callee->isDeclaration()) {
                            for (const GlobalVariable *global: globals) {
                                auto &summary = result[&caller][global];

                                changed |= merge_access_summary(summary, {.reads = true, .writes = true});
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

        std::optional<GlobalAccessSummary> get_call_global_access_summary(
            const CallBase &call, const GlobalVariable &global, const FunctionGlobalAccessSummary &summary
        ) {
            const auto *callee = call.getCalledFunction();

            if (not callee or callee->isDeclaration()) {
                return std::nullopt;
            }

            const auto function_it = summary.find(callee);
            if (function_it == summary.end()) {
                return GlobalAccessSummary{};
            }

            const auto global_it = function_it->second.find(&global);
            if (global_it == function_it->second.end()) {
                return GlobalAccessSummary{};
            }

            return global_it->second;
        }

        bool loop_has_unsupported_global_call(
            const GlobalEvolutionCandidate &candidate, const FunctionGlobalAccessSummary &summary
        ) {
            for (const BasicBlock *block: candidate.loop->blocks()) {
                for (const Instruction &instruction: *block) {
                    const auto *call = dyn_cast<CallBase>(&instruction);
                    if (not call) {
                        continue;
                    }

                    if (call->hasFnAttr(Attribute::ReturnsTwice)) {
                        return true;
                    }

                    const auto access = get_call_global_access_summary(*call, *candidate.global, summary);

                    if (not access) {
                        return true;
                    }

                    if (not access->reads and not access->writes) {
                        continue;
                    }

                    const auto *call_inst = dyn_cast<CallInst>(call);
                    if (not call_inst or call_inst->isTailCall()) {
                        return true;
                    }
                }
            }

            return false;
        }

        bool loop_has_may_throw_call(const Loop &loop) {
            for (const BasicBlock *block: loop.blocks()) {
                for (const Instruction &instruction: *block) {
                    const auto *call = dyn_cast<CallBase>(&instruction);
                    if (not call) {
                        continue;
                    }

                    if (call->mayThrow()) {
                        return true;
                    }
                }
            }

            return false;
        }

        std::optional<std::pair<const GlobalVariable *, Evolution>> evaluate_linear_value(
            const Value *value, const FuncDfsState &state
        ) {
            if (const auto *constant = dyn_cast<ConstantInt>(value)) {
                const auto &constant_value = constant->getValue();

                return std::pair{
                    nullptr, Evolution{EvolutionType::Linear,
                                 LinearEvolution{constant_value, APInt::getZero(constant_value.getBitWidth())}}};
            }

            if (const auto *load = dyn_cast<LoadInst>(value)) {
                if (const auto *global = dyn_cast<GlobalVariable>(load->getPointerOperand())) {
                    if (can_analyze_iv_global(*global)) {
                        const auto *evolution = state.get_load_evolution(*load);

                        if (not evolution) {
                            return std::nullopt;
                        }

                        if (evolution->function_evolution_type != EvolutionType::Linear) {
                            return std::nullopt;
                        }

                        return std::pair{global, *evolution};
                    }
                }

                return std::nullopt;
            }

            const auto *binary = dyn_cast<BinaryOperator>(value);
            if (not binary) {
                return std::nullopt;
            }

            const auto lhs = evaluate_linear_value(binary->getOperand(0), state);
            const auto rhs = evaluate_linear_value(binary->getOperand(1), state);

            if (not lhs or not rhs) {
                return std::nullopt;
            }

            if (lhs->first and rhs->first and lhs->first != rhs->first) {
                return std::nullopt;
            }

            const auto *global_ptr = lhs->first ? lhs->first : rhs->first;

            const auto &l_linear_ev = lhs->second.function_linear_evolution;
            const auto &r_linear_ev = rhs->second.function_linear_evolution;

            switch (binary->getOpcode()) {
                case Instruction::Add:
                    return std::pair{
                        global_ptr, Evolution{EvolutionType::Linear,
                                        LinearEvolution{l_linear_ev.b + r_linear_ev.b, l_linear_ev.k + r_linear_ev.k}}};

                case Instruction::Sub:
                    return std::pair{
                        global_ptr, Evolution{EvolutionType::Linear,
                                        LinearEvolution{l_linear_ev.b - r_linear_ev.b, l_linear_ev.k - r_linear_ev.k}}};

                case Instruction::Mul:
                    if (l_linear_ev.k.isZero()) {
                        return std::pair{global_ptr,
                            Evolution{EvolutionType::Linear,
                                LinearEvolution{l_linear_ev.b * r_linear_ev.b, l_linear_ev.b * r_linear_ev.k}}};
                    }

                    if (r_linear_ev.k.isZero()) {
                        return std::pair{global_ptr,
                            Evolution{EvolutionType::Linear,
                                LinearEvolution{r_linear_ev.b * l_linear_ev.b, r_linear_ev.b * l_linear_ev.k}}};
                    }

                    return std::nullopt;

                default:
                    return std::nullopt;
            }
        }

        void set_all_globals_unknown(FuncDfsState &state, const Module &module) {
            for (const auto &global: module.globals()) {
                if (not can_analyze_iv_global(global)) {
                    continue;
                }

                state.set_variable_unknown(global);
            }
        }

    public:
        // TODO: optimize: add memorization
        void func_dfs(FuncDfsState &state, const Module &module) {
            if (state.is_func_in_analyzing_stack_double(state.get_current_function())) {
                set_all_globals_unknown(state, module);
                state.pop_function();
                return;
            }

            const auto *function = state.get_current_function();
            const auto *bb = &function->getEntryBlock();

            std::unordered_set<const BasicBlock *> visited;

            while (true) {
                if (not visited.insert(bb).second) {
                    set_all_globals_unknown(state, module);
                    state.pop_function();
                    return;
                }

                const auto *terminator = bb->getTerminator();

                if (terminator->getNumSuccessors() > 1) {
                    set_all_globals_unknown(state, module);
                    state.pop_function();
                    return;
                }

                if (bb != &state.get_current_function()->getEntryBlock() and pred_size(bb) != 1) {
                    set_all_globals_unknown(state, module);
                    state.pop_function();
                    return;
                }

                for (const Instruction &instr: *bb) {
                    if (const auto *load = dyn_cast<LoadInst>(&instr)) {
                        const auto *global = dyn_cast<GlobalVariable>(load->getPointerOperand());

                        if (not global) {
                            continue;
                        }

                        if (not can_analyze_iv_global(*global)) {
                            continue;
                        }

                        state.remember_load(*load, *global);
                    } else if (const auto *call_base = dyn_cast<CallBase>(&instr)) {
                        const Function *callee = call_base->getCalledFunction();

                        if (not callee or callee->isDeclaration()) {
                            set_all_globals_unknown(state, module);
                            continue;
                        }

                        state.push_function(callee);
                        func_dfs(state, module);
                    } else if (const auto *store = dyn_cast<StoreInst>(&instr)) {
                        const auto *global = dyn_cast<GlobalVariable>(store->getPointerOperand());

                        if (not global) {
                            continue;
                        }

                        if (not can_analyze_iv_global(*global)) {
                            continue;
                        }

                        const auto evolution = evaluate_linear_value(store->getValueOperand(), state);

                        if (not evolution) {
                            state.set_variable_unknown(*global);
                            continue;
                        }

                        if (evolution->first and evolution->first != global) {
                            state.set_variable_unknown(*global);
                            continue;
                        }

                        state.set_variable_evolution(*global, evolution->second);
                    }
                }

                if (terminator->getNumSuccessors() == 0) {
                    break;
                }

                bb = terminator->getSuccessor(0);
            }

            state.pop_function();
        }

        bool global_has_atomic_access(const GlobalVariable &global, const GlobalAccessMap &global_accesses) {
            const auto global_it = global_accesses.find(&global);

            if (global_it == global_accesses.end()) {
                return false;
            }

            for (const auto &[_, loads]: global_it->second.loads) {
                for (const LoadInst *load: loads) {
                    if (load->isAtomic()) {
                        return true;
                    }
                }
            }

            for (const auto &[_, stores]: global_it->second.stores) {
                for (const StoreInst *store: stores) {
                    if (store->isAtomic()) {
                        return true;
                    }
                }
            }

            return false;
        }

        std::optional<DenseMap<const GlobalVariable *, Evolution>> analyze_loop_iteration(
            const Loop &loop,
            const DenseMap<const Function *, DenseMap<const GlobalVariable *, Evolution>> &func_global_evolution,
            const Module &module
        ) {
            if (not loop.isInnermost()) {
                return std::nullopt;
            }

            FuncDfsState state;

            const auto *header = loop.getHeader();
            const auto *bb = header;

            std::unordered_set<const BasicBlock *> visited;

            while (true) {
                if (not visited.insert(bb).second) {
                    return std::nullopt;
                }

                for (const Instruction &instr: *bb) {
                    if (const auto *load = dyn_cast<LoadInst>(&instr)) {
                        const auto *global = dyn_cast<GlobalVariable>(load->getPointerOperand());

                        if (not global) {
                            continue;
                        }

                        if (not can_analyze_iv_global(*global)) {
                            continue;
                        }

                        state.remember_load(*load, *global);
                    } else if (const auto *call = dyn_cast<CallBase>(&instr)) {
                        const auto *callee = call->getCalledFunction();

                        if (not callee) {
                            set_all_globals_unknown(state, module);
                            continue;
                        }

                        const auto func_it = func_global_evolution.find(callee);

                        if (func_it == func_global_evolution.end()) {
                            set_all_globals_unknown(state, module);
                            continue;
                        }

                        for (const auto &[global, evolution]: func_it->second) {
                            state.compose_variable_evolution(*global, evolution);
                        }
                    } else if (const auto *store = dyn_cast<StoreInst>(&instr)) {
                        const auto *global = dyn_cast<GlobalVariable>(store->getPointerOperand());

                        if (not global) {
                            continue;
                        }

                        if (not can_analyze_iv_global(*global)) {
                            continue;
                        }

                        const auto evolution = evaluate_linear_value(store->getValueOperand(), state);

                        if (not evolution) {
                            state.set_variable_unknown(*global);
                            continue;
                        }

                        if (evolution->first and evolution->first != global) {
                            state.set_variable_unknown(*global);
                            continue;
                        }

                        state.set_variable_evolution(*global, evolution->second);
                    }
                }

                const auto *terminator = bb->getTerminator();

                SmallVector<const BasicBlock *> successors_inside_loop;

                for (unsigned i = 0; i < terminator->getNumSuccessors(); ++i) {
                    const auto *successor = terminator->getSuccessor(i);

                    if (loop.contains(successor)) {
                        successors_inside_loop.push_back(successor);
                    }
                }

                if (successors_inside_loop.size() != 1) {
                    return std::nullopt;
                }

                const auto *next = successors_inside_loop.front();

                if (next == header) {
                    break;
                }

                bb = next;
            }

            DenseMap<const GlobalVariable *, Evolution> result;

            for (const auto &global: module.globals()) {
                if (not can_analyze_iv_global(global)) {
                    continue;
                }

                if (const Evolution *evolution = state.get_variable_evolution(global)) {
                    result[&global] = *evolution;
                } else {
                    result[&global] = make_identity_evolution(global);
                }
            }

            return result;
        }

        bool instruction_may_synchronize(const Instruction &instruction) {
            if (isa<FenceInst>(instruction)) {
                return true;
            }

            if (isa<AtomicRMWInst>(instruction)) {
                return true;
            }

            if (isa<AtomicCmpXchgInst>(instruction)) {
                return true;
            }

            if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
                return isStrongerThanUnordered(load->getOrdering());
            }

            if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
                return isStrongerThanUnordered(store->getOrdering());
            }

            return false;
        }

        FunctionSynchronizationSummary build_direct_synchronization_summary(const Module &module) {
            FunctionSynchronizationSummary result;

            for (const Function &function: module) {
                if (function.isDeclaration()) {
                    continue;
                }

                bool may_synchronize = false;

                for (const Instruction &instruction: instructions(function)) {
                    if (isa<CallBase>(instruction)) {
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

        FunctionSynchronizationSummary build_transitive_synchronization_summary(
            const Module &module, const FunctionSynchronizationSummary &direct_summary
        ) {
            auto result = direct_summary;

            bool changed;

            do {
                changed = false;

                for (const Function &caller: module) {
                    if (caller.isDeclaration()) {
                        continue;
                    }

                    for (const Instruction &instruction: instructions(caller)) {
                        const auto *call = dyn_cast<CallBase>(&instruction);
                        if (not call) {
                            continue;
                        }

                        const auto *callee = call->getCalledFunction();

                        bool call_may_synchronize;

                        if (not callee or callee->isDeclaration()) {
                            call_may_synchronize = not call->hasFnAttr(Attribute::NoSync);
                        } else {
                            call_may_synchronize = result.lookup(callee);
                        }

                        if (call_may_synchronize and not result.lookup(&caller)) {
                            result[&caller] = true;
                            changed = true;
                        }
                    }
                }
            } while (changed);

            return result;
        }

        bool loop_has_may_synchronize(const Loop &loop, const FunctionSynchronizationSummary &summary) {
            for (const BasicBlock *block: loop.blocks()) {
                for (const Instruction &instruction: *block) {
                    if (const auto *call = dyn_cast<CallBase>(&instruction)) {
                        const auto *callee = call->getCalledFunction();

                        if (not callee or callee->isDeclaration()) {
                            if (not call->hasFnAttr(Attribute::NoSync)) {
                                return true;
                            }

                            continue;
                        }

                        if (summary.lookup(callee)) {
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

        bool loop_writes_global(const GlobalEvolutionCandidate &candidate, const GlobalAccessMap &global_accesses) {
            const auto global_it = global_accesses.find(candidate.global);

            if (global_it == global_accesses.end()) {
                return false;
            }

            for (const auto &[_, stores]: global_it->second.stores) {
                for (const StoreInst *store: stores) {
                    if (candidate.loop->contains(store)) {
                        return true;
                    }
                }
            }

            return false;
        }

        bool loop_has_guaranteed_store(
            const GlobalEvolutionCandidate &candidate, const GlobalAccessMap &global_accesses,
            const DominatorTree &dom_tree
        ) {
            const auto global_it = global_accesses.find(candidate.global);

            if (global_it == global_accesses.end()) {
                return false;
            }

            SimpleLoopSafetyInfo safety_info;
            safety_info.computeLoopSafetyInfo(candidate.loop);

            for (const auto &[_, stores]: global_it->second.stores) {
                for (const StoreInst *store: stores) {
                    if (not candidate.loop->contains(store)) {
                        continue;
                    }

                    if (safety_info.isGuaranteedToExecute(*store, &dom_tree, candidate.loop)) {
                        return true;
                    }
                }
            }

            return false;
        }

        std::optional<bool> loop_needs_initial_value(
            const GlobalEvolutionCandidate &candidate, const FunctionGlobalAccessSummary &summary
        ) {
            const auto &loop = *candidate.loop;
            const auto *global = candidate.global;

            const auto *header = loop.getHeader();
            const auto *bb = header;

            std::unordered_set<const BasicBlock *> visited;

            while (true) {
                if (not visited.insert(bb).second) {
                    return std::nullopt;
                }

                for (const Instruction &instruction: *bb) {
                    if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
                        if (load->getPointerOperand() == global) {
                            return true;
                        }

                        continue;
                    }

                    if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
                        if (store->getPointerOperand() == global) {
                            return false;
                        }

                        continue;
                    }

                    const auto *call = dyn_cast<CallBase>(&instruction);
                    if (not call) {
                        continue;
                    }

                    const auto access = get_call_global_access_summary(*call, *global, summary);

                    if (not access) {
                        return std::nullopt;
                    }

                    if (access->reads or access->writes) {
                        return true;
                    }
                }

                const auto *terminator = bb->getTerminator();

                SmallVector<const BasicBlock *> successors_inside_loop;

                for (unsigned i = 0; i < terminator->getNumSuccessors(); ++i) {
                    const auto *successor = terminator->getSuccessor(i);

                    if (loop.contains(successor)) {
                        successors_inside_loop.push_back(successor);
                    }
                }

                if (successors_inside_loop.size() != 1) {
                    return std::nullopt;
                }

                const auto *next = successors_inside_loop.front();

                if (next == header) {
                    break;
                }

                bb = next;
            }

            return std::nullopt;
        }

        Align get_safe_global_access_alignment(const GlobalVariable &global) {
            if (const MaybeAlign alignment = global.getAlign()) {
                return *alignment;
            }

            // No explicit alignment is part of the IR contract here.
            // Using 1 avoids introducing a stronger alignment assumption for the
            // global.
            return Align(1);
        }

        Align get_required_local_alignment(
            const GlobalEvolutionCandidate &candidate, const GlobalAccessMap &global_accesses
        ) {
            const auto &data_layout = candidate.global->getParent()->getDataLayout();

            Align required_alignment = data_layout.getABITypeAlign(candidate.global->getValueType());

            if (const MaybeAlign global_alignment = candidate.global->getAlign()) {
                if (*global_alignment > required_alignment) {
                    required_alignment = *global_alignment;
                }
            }

            const auto global_it = global_accesses.find(candidate.global);
            if (global_it == global_accesses.end()) {
                return required_alignment;
            }

            for (const auto &[_, loads]: global_it->second.loads) {
                for (const LoadInst *load: loads) {
                    if (not candidate.loop->contains(load)) {
                        continue;
                    }

                    if (load->getAlign() > required_alignment) {
                        required_alignment = load->getAlign();
                    }
                }
            }

            for (const auto &[_, stores]: global_it->second.stores) {
                for (const StoreInst *store: stores) {
                    if (not candidate.loop->contains(store)) {
                        continue;
                    }

                    if (store->getAlign() > required_alignment) {
                        required_alignment = store->getAlign();
                    }
                }
            }

            return required_alignment;
        }

        AllocaInst *create_local_slot(
            Function &function, const GlobalEvolutionCandidate &candidate, const GlobalAccessMap &global_accesses
        ) {
            auto &entry_block = function.getEntryBlock();

            IRBuilder<> builder(function.getContext());
            builder.SetInsertPoint(&entry_block, entry_block.getFirstInsertionPt());

            AllocaInst *local_slot = builder.CreateAlloca(
                candidate.global->getValueType(), nullptr, Twine(candidate.global->getName()) + ".local");

            local_slot->setAlignment(get_required_local_alignment(candidate, global_accesses));

            return local_slot;
        }

        void initialize_local_slot(
            const GlobalEvolutionCandidate &candidate, AllocaInst &local_slot, bool needs_initial_value
        ) {
            if (not needs_initial_value) {
                return;
            }

            auto *global = const_cast<GlobalVariable *>(candidate.global);
            auto *preheader = const_cast<BasicBlock *>(candidate.loop->getLoopPreheader());

            IRBuilder<> builder(preheader->getTerminator());

            LoadInst *initial_value = builder.CreateAlignedLoad(
                candidate.global->getValueType(), global, get_safe_global_access_alignment(*candidate.global),
                Twine(candidate.global->getName()) + ".initial");

            builder.CreateAlignedStore(initial_value, &local_slot, local_slot.getAlign());
        }

        void synchronize_global_around_calls(
            const GlobalEvolutionCandidate &candidate, const FunctionGlobalAccessSummary &summary,
            AllocaInst &local_slot
        ) {
            SmallVector<CallInst *> calls;

            for (BasicBlock *block: candidate.loop->blocks()) {
                for (Instruction &instruction: *block) {
                    auto *call = dyn_cast<CallInst>(&instruction);
                    if (not call) {
                        continue;
                    }

                    const auto access = get_call_global_access_summary(*call, *candidate.global, summary);

                    if (not access or (not access->reads and not access->writes)) {
                        continue;
                    }

                    calls.push_back(call);
                }
            }

            auto *global = const_cast<GlobalVariable *>(candidate.global);
            auto *global_type = candidate.global->getValueType();

            for (CallInst *call: calls) {
                const auto access = get_call_global_access_summary(*call, *candidate.global, summary);

                assert(access and "unsupported call passed legality checks");

                if (access->reads or access->writes) {
                    IRBuilder before_builder(call);

                    LoadInst *local_value = before_builder.CreateAlignedLoad(
                        global_type, &local_slot, local_slot.getAlign(),
                        Twine(candidate.global->getName()) + ".before.call");

                    before_builder.CreateAlignedStore(
                        local_value, global, get_safe_global_access_alignment(*candidate.global));
                }

                if (access->writes) {
                    auto *next = call->getNextNode();
                    assert(next and "CallInst must have a following instruction");

                    IRBuilder<> after_builder(next);

                    LoadInst *global_value = after_builder.CreateAlignedLoad(
                        global_type, global, get_safe_global_access_alignment(*candidate.global),
                        Twine(candidate.global->getName()) + ".after.call");

                    after_builder.CreateAlignedStore(global_value, &local_slot, local_slot.getAlign());
                }
            }
        }

        void redirect_loop_global_accesses(
            const GlobalEvolutionCandidate &candidate, const GlobalAccessMap &global_accesses, AllocaInst &local_slot
        ) {
            const auto global_it = global_accesses.find(candidate.global);
            if (global_it == global_accesses.end()) {
                return;
            }

            for (const auto &[_, loads]: global_it->second.loads) {
                for (const LoadInst *load: loads) {
                    if (not candidate.loop->contains(load)) {
                        continue;
                    }

                    auto *mutable_load = const_cast<LoadInst *>(load);
                    mutable_load->setOperand(0, &local_slot);
                }
            }

            for (const auto &[_, stores]: global_it->second.stores) {
                for (const StoreInst *store: stores) {
                    if (not candidate.loop->contains(store)) {
                        continue;
                    }

                    auto *mutable_store = const_cast<StoreInst *>(store);
                    mutable_store->setOperand(1, &local_slot);
                }
            }
        }

        void write_back_local_slot(const GlobalEvolutionCandidate &candidate, AllocaInst &local_slot) {
            auto *global = const_cast<GlobalVariable *>(candidate.global);
            auto *exit_block = const_cast<BasicBlock *>(candidate.loop->getUniqueExitBlock());

            IRBuilder<> builder(candidate.global->getContext());
            builder.SetInsertPoint(exit_block, exit_block->getFirstInsertionPt());

            LoadInst *final_value = builder.CreateAlignedLoad(
                candidate.global->getValueType(), &local_slot, local_slot.getAlign(),
                Twine(candidate.global->getName()) + ".final");

            builder.CreateAlignedStore(final_value, global, get_safe_global_access_alignment(*candidate.global));
        }

        PreservedAnalyses run(Module &module, ModuleAnalysisManager &mam) {
            bool changed = false;

            const auto global_accesses = collect_global_accesses(module);

            SmallVector<const GlobalVariable *> tracked_globals;
            tracked_globals.reserve(global_accesses.size());

            for (const auto &[global, _]: global_accesses) {
                tracked_globals.push_back(global);
            }

            const auto direct_access_summary = build_direct_access_summary(global_accesses);

            const auto transitive_access_summary
                = build_transitive_access_summary(module, direct_access_summary, tracked_globals);

            const auto direct_synchronization_summary = build_direct_synchronization_summary(module);

            const auto transitive_synchronization_summary
                = build_transitive_synchronization_summary(module, direct_synchronization_summary);

            DenseMap<const Function *, DenseMap<const GlobalVariable *, Evolution>> func_global_evolution;

            for (const Function &f: module.functions()) {
                if (f.isDeclaration()) {
                    continue;
                }

                FuncDfsState state;
                state.push_function(&f);

                func_dfs(state, module);

                for (const GlobalVariable &global: module.globals()) {
                    if (not can_analyze_iv_global(global)) {
                        continue;
                    }

                    if (const Evolution *ev = state.get_variable_evolution(global)) {
                        func_global_evolution[&f][&global] = *ev;
                    } else {
                        func_global_evolution[&f][&global] = make_identity_evolution(global);
                    }
                }
            }

            auto &function_analysis_manager = mam.getResult<FunctionAnalysisManagerModuleProxy>(module).getManager();

            for (Function &function: module) {
                if (function.isDeclaration()) {
                    continue;
                }

                auto &loop_info = function_analysis_manager.getResult<LoopAnalysis>(function);

                auto &dom_tree = function_analysis_manager.getResult<DominatorTreeAnalysis>(function);

                for (const Loop *loop: loop_info.getLoopsInPreorder()) {
                    auto result = analyze_loop_iteration(*loop, func_global_evolution, module);

                    if (not result) {
                        continue;
                    }

                    for (const auto &[global, evolution]: *result) {
                        if (evolution.function_evolution_type == EvolutionType::Unknown) {
                            continue;
                        }

                        const GlobalEvolutionCandidate candidate{loop, global};

                        if (not loop_writes_global(candidate, global_accesses)) {
                            continue;
                        }

                        const bool has_unsupported_global_call
                            = loop_has_unsupported_global_call(candidate, transitive_access_summary);

                        const bool has_may_throw_call = loop_has_may_throw_call(*candidate.loop);

                        const bool has_atomic_access = global_has_atomic_access(*candidate.global, global_accesses);

                        const bool has_may_synchronize
                            = loop_has_may_synchronize(*candidate.loop, transitive_synchronization_summary);

                        const auto *preheader = candidate.loop->getLoopPreheader();

                        const auto *exit_block = candidate.loop->getUniqueExitBlock();

                        const bool has_dedicated_exits = candidate.loop->hasDedicatedExits();

                        const bool has_entry_insertion_point = function.getEntryBlock().hasInsertionPt();

                        const bool has_preheader_insertion_point = preheader and preheader->hasInsertionPt();

                        const bool has_exit_insertion_point = exit_block and exit_block->hasInsertionPt();

                        const bool has_compatible_address_space
                            = candidate.global->getAddressSpace() == module.getDataLayout().getAllocaAddrSpace();

                        const bool has_guaranteed_store
                            = loop_has_guaranteed_store(candidate, global_accesses, dom_tree);

                        const auto needs_initial_value = loop_needs_initial_value(candidate, transitive_access_summary);

                        const bool is_supported = not has_unsupported_global_call and not has_may_throw_call
                                                  and not has_atomic_access and not has_may_synchronize and preheader
                                                  and exit_block and has_dedicated_exits and has_entry_insertion_point
                                                  and has_preheader_insertion_point and has_exit_insertion_point
                                                  and has_compatible_address_space and needs_initial_value.has_value()
                                                  and (*needs_initial_value or has_guaranteed_store);

                        if (not is_supported) {
                            continue;
                        }

                        auto *local_slot = create_local_slot(function, candidate, global_accesses);

                        initialize_local_slot(candidate, *local_slot, *needs_initial_value);

                        synchronize_global_around_calls(candidate, transitive_access_summary, *local_slot);

                        redirect_loop_global_accesses(candidate, global_accesses, *local_slot);

                        write_back_local_slot(candidate, *local_slot);

                        changed = true;
                    }
                }
            }

            return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
        }

        static bool isRequired() {
            return true;
        }
    };
} // namespace

static void registerGlobalIVPass(PassBuilder &builder) {
    builder.registerPipelineParsingCallback(
        [](StringRef name, ModulePassManager &manager, ArrayRef<PassBuilder::PipelineElement>) {
            if (name != "global-iv") {
                return false;
            }

            manager.addPass(GlobalIVPass{});
            return true;
        });
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "GlobalIVPass", LLVM_VERSION_STRING, registerGlobalIVPass};
}
