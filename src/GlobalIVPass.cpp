#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/ValueLatticeUtils.h"
#include "llvm/IR/InstIterator.h"

using namespace llvm;

namespace {
    struct GlobalAccesses {
        DenseMap<const Function *, SmallVector<const LoadInst *> > loads;
        DenseMap<const Function *, SmallVector<const StoreInst *> > stores;
    };

    class GlobalIVPass : public PassInfoMixin<GlobalIVPass> {
        bool can_analyze_global(GlobalVariable &global) {
            return canTrackGlobalVariableInterprocedurally(&global);
        }

        SmallVector<const GlobalVariable *> evaluate_globals(Module &module) {
            SmallVector<const GlobalVariable *> globals;

            for (auto &global: module.globals()) {
                if (can_analyze_global(global))
                    globals.push_back(&global);
            }

            return globals;
        }


        DenseMap<const GlobalVariable *, GlobalAccesses> evaluate_globals_info(
            const SmallVector<const GlobalVariable *> &globals_vec
        ) {
            DenseMap<const GlobalVariable *, GlobalAccesses> info;

            for (const auto global: globals_vec) {
                for (const auto user: global->users()) {
                    if (const auto *load = dyn_cast<LoadInst>(user)) {
                        info[global].loads[load->getFunction()].push_back(load);
                    } else if (const auto *store = dyn_cast<StoreInst>(user)) {
                        info[global].stores[store->getFunction()].push_back(store);
                    }
                }
            }

            return info;
        }

    public:
        PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
            auto globals_vec = evaluate_globals(module);
            auto globals_info = evaluate_globals_info(globals_vec);


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
