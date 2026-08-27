#include <unordered_set>

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/ValueLatticeUtils.h"
#include "llvm/IR/InstIterator.h"

using namespace llvm;

using i64 = int64_t;
using i32 = int32_t;

namespace {
    struct GlobalAccesses {
        DenseMap<const Function *, SmallVector<const LoadInst *> > loads;
        DenseMap<const Function *, SmallVector<const StoreInst *> > stores;
    };

    enum class EvolutionType {
        Unknown,
        Linear
    };

    struct LinearEvolution {
        // b + k * x
        i64 b;
        i64 k;
    };

    struct Evolution {
        EvolutionType function_evolution_type;
        LinearEvolution function_linear_evolution;
    };

    struct GlobalEvolutionCandidate {
        const Loop *loop;
        const GlobalVariable *global;
        Evolution evolution;
    };

    class FuncDfsState {
        std::multiset<const Function *> analyzing_functions_multiset;
        SmallVector<const Function *> analyzing_functions_stack;
        DenseMap<const GlobalVariable *, Evolution> variable_to_evolution;
        DenseMap<const LoadInst *, Evolution> load_to_evolution;

    public:
        [[nodiscard]] const Evolution *get_variable_evolution(const GlobalVariable &global) const {
            const auto it = variable_to_evolution.find(&global);
            if (it == variable_to_evolution.end())
                return nullptr;

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
            variable_to_evolution[&global] = {EvolutionType::Unknown, {}};
        }

        void compose_variable_evolution(
            const GlobalVariable &global,
            const Evolution &after
        ) {
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

            if (before.function_evolution_type == EvolutionType::Unknown)
                return;

            const auto &before_linear = before.function_linear_evolution;
            const auto &after_linear = after.function_linear_evolution;

            variable_to_evolution[&global] = {
                EvolutionType::Linear,
                {
                    after_linear.b + after_linear.k * before_linear.b,
                    after_linear.k * before_linear.k
                }
            };
        }

        void remember_load(
            const LoadInst &load,
            const GlobalVariable &global
        ) {
            if (variable_to_evolution.contains(&global)) {
                load_to_evolution[&load] = variable_to_evolution[&global];
            } else {
                load_to_evolution[&load] = {EvolutionType::Linear, {0, 1}};
            }
        }

        [[nodiscard]] const Evolution *get_load_evolution(const LoadInst &load) const {
            const auto it = load_to_evolution.find(&load);
            if (it == load_to_evolution.end()) return nullptr;

            return &it->second;
        }

        bool is_func_in_analyzing_stack_double(const Function *function) const {
            return analyzing_functions_multiset.count(function) >= 2;
        }
    };


    class GlobalIVPass : public PassInfoMixin<GlobalIVPass> {
        DenseMap<const GlobalVariable *, GlobalAccesses> collect_global_accesses(Module &module) {
            DenseMap<const GlobalVariable *, GlobalAccesses> result;

            for (auto &global: module.globals()) {
                if (!can_analyze_iv_global(global))
                    continue;

                auto &accesses = result[&global];

                for (User *user: global.users()) {
                    if (const auto *load = dyn_cast<LoadInst>(user)) {
                        const Function *function = load->getFunction();

                        accesses.loads[function].push_back(load);
                        continue;
                    }

                    if (const auto *store = dyn_cast<StoreInst>(user)) {
                        const Function *function = store->getFunction();

                        accesses.stores[function].push_back(store);
                        continue;
                    }

                    llvm_unreachable(
                        "canTrackGlobalVariableInterprocedurally guaranteed load/store-only users"
                    );
                }
            }

            return result;
        }

        bool can_analyze_iv_global(const GlobalVariable &global) {
            return global.getValueType()->isIntegerTy()
                   and canTrackGlobalVariableInterprocedurally(const_cast<GlobalVariable *>(&global));
        }

        std::optional<std::pair<const GlobalVariable *, Evolution> > evaluate_linear_value(
            const Value *value,
            const FuncDfsState &state
        ) {
            if (const auto *constant = dyn_cast<ConstantInt>(value)) {
                return std::pair{
                    nullptr, Evolution{EvolutionType::Linear, LinearEvolution{constant->getSExtValue(), 0}}
                };
            }

            if (const auto *load = dyn_cast<LoadInst>(value)) {
                if (const auto &global = dyn_cast<GlobalVariable>(load->getPointerOperand()))
                    if (can_analyze_iv_global(*global)) {
                        const auto *evolution = state.get_load_evolution(*load);
                        if (!evolution) return std::nullopt;
                        if (evolution->function_evolution_type != EvolutionType::Linear) return std::nullopt;

                        return std::pair{global, *evolution};
                    }

                return std::nullopt;
            }

            const auto *binary = dyn_cast<BinaryOperator>(value);
            if (!binary) return std::nullopt;

            const auto lhs = evaluate_linear_value(binary->getOperand(0), state);
            const auto rhs = evaluate_linear_value(binary->getOperand(1), state);

            if (!lhs or !rhs) return std::nullopt;
            if (lhs->first and rhs->first and lhs->first != rhs->first) return std::nullopt;

            auto global_ptr = lhs->first ? lhs->first : rhs->first;

            const auto &l_linear_ev = lhs->second.function_linear_evolution;
            const auto &r_linear_ev = rhs->second.function_linear_evolution;
            switch (binary->getOpcode()) {
                case Instruction::Add:
                    return std::pair{
                        global_ptr,
                        Evolution{
                            EvolutionType::Linear, LinearEvolution{
                                l_linear_ev.b + r_linear_ev.b,
                                l_linear_ev.k + r_linear_ev.k
                            }
                        }
                    };

                case Instruction::Sub:
                    return std::pair{
                        global_ptr,
                        Evolution{
                            EvolutionType::Linear, LinearEvolution{
                                l_linear_ev.b - r_linear_ev.b,
                                l_linear_ev.k - r_linear_ev.k
                            }
                        }
                    };

                case Instruction::Mul:
                    if (lhs->second.function_linear_evolution.k == 0)
                        return std::pair{
                            global_ptr,
                            Evolution{
                                EvolutionType::Linear, LinearEvolution{
                                    l_linear_ev.b * r_linear_ev.b,
                                    l_linear_ev.b * r_linear_ev.k
                                }
                            }
                        };

                    if (rhs->second.function_linear_evolution.k == 0)
                        return std::pair{
                            global_ptr,
                            Evolution{
                                EvolutionType::Linear, LinearEvolution{
                                    r_linear_ev.b * l_linear_ev.b,
                                    r_linear_ev.b * l_linear_ev.k
                                }
                            }
                        };

                    return std::nullopt;

                default:
                    return std::nullopt;
            }
        }

        void set_all_globals_unknown(FuncDfsState &state, const Module &module) {
            for (const auto &global: module.globals()) {
                if (!can_analyze_iv_global(global))
                    continue;

                state.set_variable_unknown(global);
            }
        }

    public:
        // TODO: add memorization
        void func_dfs(FuncDfsState &state, const Module &module) {
            // check for recursion
            if (state.is_func_in_analyzing_stack_double(state.get_current_function())) {
                set_all_globals_unknown(state, module);
                state.pop_function();
                return;
            }

            const Function *function = state.get_current_function();
            const BasicBlock *bb = &function->getEntryBlock();

            std::unordered_set<const BasicBlock *> visited;
            while (true) {
                if (not visited.insert(bb).second) {
                    set_all_globals_unknown(state, module);
                    state.pop_function();
                    return;
                }

                const Instruction *terminator = bb->getTerminator();

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

                for (const auto &instr: *bb) {
                    if (const auto *load = dyn_cast<LoadInst>(&instr)) {
                        const auto *global = dyn_cast<GlobalVariable>(load->getPointerOperand());
                        if (!global) continue;
                        if (!can_analyze_iv_global(*global)) continue;

                        state.remember_load(*load, *global);
                    } else if (const auto *call_base = dyn_cast<CallBase>(&instr)) {
                        const auto &callee = call_base->getCalledFunction();

                        // indirection call
                        if (!callee or callee->isDeclaration()) {
                            set_all_globals_unknown(state, module);
                            continue;
                        }

                        state.push_function(callee);
                        func_dfs(state, module);
                    } else if (const auto &store = dyn_cast<StoreInst>(&instr)) {
                        const auto *global = dyn_cast<GlobalVariable>(store->getPointerOperand());
                        if (!global) continue;
                        if (!can_analyze_iv_global(*global)) continue;

                        const auto evolution = evaluate_linear_value(store->getValueOperand(), state);
                        if (!evolution) {
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

                if (terminator->getNumSuccessors() == 0)
                    break;

                bb = terminator->getSuccessor(0);
            }


            state.pop_function();
        }


        std::optional<DenseMap<const GlobalVariable *, Evolution> > analyze_loop_iteration(
            const Loop &loop,
            const DenseMap<const Function *, DenseMap<const GlobalVariable *, Evolution> > &func_global_evolution,
            const Module &module
        ) {
            if (!loop.isInnermost())
                return std::nullopt;


            FuncDfsState state;

            const BasicBlock *header = loop.getHeader();
            const BasicBlock *bb = header;

            std::unordered_set<const BasicBlock *> visited;

            while (true) {
                if (!visited.insert(bb).second)
                    return std::nullopt;

                for (const auto &instr: *bb) {
                    if (const auto *load = dyn_cast<LoadInst>(&instr)) {
                        const auto *global = dyn_cast<GlobalVariable>(load->getPointerOperand());

                        if (!global) continue;
                        if (!can_analyze_iv_global(*global)) continue;

                        state.remember_load(*load, *global);
                    } else if (const auto *call = dyn_cast<CallBase>(&instr)) {
                        const Function *callee = call->getCalledFunction();

                        if (!callee) {
                            set_all_globals_unknown(state, module);
                            continue;
                        }

                        const auto func_it = func_global_evolution.find(callee);

                        if (func_it == func_global_evolution.end()) {
                            set_all_globals_unknown(state, module);
                            continue;
                        }

                        for (const auto &[global, evolution]: func_it->second) {
                            state.compose_variable_evolution(
                                *global,
                                evolution
                            );
                        }
                    } else if (const auto *store = dyn_cast<StoreInst>(&instr)) {
                        const auto *global = dyn_cast<GlobalVariable>(store->getPointerOperand());

                        if (!global) continue;
                        if (!can_analyze_iv_global(*global)) continue;

                        auto evolution = evaluate_linear_value(store->getValueOperand(), state);

                        if (!evolution) {
                            state.set_variable_unknown(*global);
                            continue;
                        }

                        if (evolution->first &&
                            evolution->first != global) {
                            state.set_variable_unknown(*global);
                            continue;
                        }

                        state.set_variable_evolution(
                            *global,
                            evolution->second
                        );
                    }
                }

                const Instruction *terminator =
                        bb->getTerminator();

                SmallVector<const BasicBlock *, 2>
                        successors_inside_loop;

                for (unsigned i = 0;
                     i < terminator->getNumSuccessors();
                     ++i) {
                    const BasicBlock *successor =
                            terminator->getSuccessor(i);

                    if (loop.contains(successor))
                        successors_inside_loop.push_back(
                            successor
                        );
                }

                if (successors_inside_loop.size() != 1)
                    return std::nullopt;

                const BasicBlock *next =
                        successors_inside_loop.front();

                if (next == header)
                    break;

                bb = next;
            }

            DenseMap<const GlobalVariable *, Evolution> result;

            for (const auto &global: module.globals()) {
                if (!can_analyze_iv_global(global))
                    continue;

                if (const Evolution *evolution = state.get_variable_evolution(global)) {
                    result[&global] = *evolution;
                } else {
                    result[&global] = {EvolutionType::Linear, {0, 1}};
                }
            }

            return result;
        }

        void print_info(
            const DenseMap<const Function *, DenseMap<const GlobalVariable *, Evolution> > &func_global_evolution
        ) {
            for (const auto &[f, map]: func_global_evolution) {
                errs() << f->getName() << ": \n";

                for (const auto &[global, evolution]: map) {
                    errs() << "  " << global->getName();

                    if (evolution.function_evolution_type == EvolutionType::Unknown) {
                        errs() << ": unknown\n";
                        continue;
                    }

                    const auto &[b, k] = evolution.function_linear_evolution;
                    errs() << ": b=" << b << ", k=" << k << "\n";
                }
                errs() << "\n";
            }
        }

        PreservedAnalyses run(Module &module, ModuleAnalysisManager &mam) {
            const auto &global_accesses = collect_global_accesses(module);


            for (const auto &[global, accesses]: global_accesses) {
                errs() << global->getName() << ":\n";

                for (const auto &[function, loads]: accesses.loads)
                    errs() << "  " << function->getName() << " loads=" << loads.size() << "\n";

                for (const auto &[function, stores]: accesses.stores)
                    errs() << "  " << function->getName() << " stores=" << stores.size() << "\n";
            }


            DenseMap<const Function *, DenseMap<const GlobalVariable *, Evolution> > func_global_evolution;

            const auto &fs_it = module.functions();

            for (const auto &f: fs_it) {
                if (f.isDeclaration())
                    continue;

                FuncDfsState state;
                state.push_function(&f);
                func_dfs(state, module);


                for (const auto &global: module.globals()) {
                    if (!can_analyze_iv_global(global))
                        continue;

                    auto &global_ev = func_global_evolution[&f][&global];

                    if (const auto ev_ptr = state.get_variable_evolution(global)) {
                        global_ev = *ev_ptr;
                    } else {
                        global_ev = Evolution{EvolutionType::Linear, {0, 1}};
                    }
                }
            }

            print_info(func_global_evolution);

            auto &function_analysis_manager =
                    mam
                    .getResult<FunctionAnalysisManagerModuleProxy>(module)
                    .getManager();

            for (auto &function: module) {
                if (function.isDeclaration())
                    continue;

                auto &loop_info = function_analysis_manager.getResult<LoopAnalysis>(function);

                for (const auto loop: loop_info.getLoopsInPreorder()) {
                    errs() << "Loop "
                            << loop->getHeader()->getName()
                            << ":\n";

                    auto result = analyze_loop_iteration(
                        *loop,
                        func_global_evolution,
                        module
                    );

                    if (!result) {
                        errs() << "  unsupported\n";
                        continue;
                    }

                    for (const auto &[global, evolution]: *result) {
                        errs() << "  "
                                << global->getName()
                                << ": ";

                        if (evolution.function_evolution_type ==
                            EvolutionType::Unknown) {
                            errs() << "unknown\n";
                            continue;
                        }

                        const auto &[b, k] = evolution.function_linear_evolution;

                        errs() << "b=" << b
                                << ", k=" << k
                                << "\n";
                    }
                }
            }

            return PreservedAnalyses::all();
        }

        static bool isRequired() {
            return true;
        }
    };
}

static void registerGlobalIVPass(PassBuilder &builder) {
    builder.registerPipelineParsingCallback(
        [](StringRef name,
           ModulePassManager &manager,
           ArrayRef<PassBuilder::PipelineElement>) {
            if (name != "global-iv") {
                return false;
            }

            manager.addPass(GlobalIVPass{});
            return true;
        });
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "GlobalIVPass",
        LLVM_VERSION_STRING,
        registerGlobalIVPass
    };
}
