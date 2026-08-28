#include "GlobalIVPass.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

namespace {

    void register_global_iv_pass(llvm::PassBuilder &builder) {
        builder.registerPipelineParsingCallback([](llvm::StringRef name, llvm::ModulePassManager &manager,
                                                    llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
            if (name != "global-iv") {
                return false;
            }

            manager.addPass(global_iv::GlobalIVPass{});
            return true;
        });
    }

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "GlobalIVPass", LLVM_VERSION_STRING, register_global_iv_pass};
}
