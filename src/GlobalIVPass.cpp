#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/ValueLatticeUtils.h"

using namespace llvm;

namespace {
    class GlobalIVPass : PassInfoMixin<GlobalIVPass> {
        bool can_analyze_global(GlobalVariable &global) {
            return not global.isConstant()
                   and global.hasInitializer()
                   and canTrackGlobalVariableInterprocedurally(&global);
        }

    public:
        PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
            errs() << "GlobalIVPass: " << module.getName() << '\n';

            SmallVector<GlobalVariable *> globals;

            for (auto &global: module.globals()) {
                if (can_analyze_global(global))
                    globals.push_back(&global);
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
