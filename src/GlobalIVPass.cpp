#include "GlobalIVPass.h"

#include "EvolutionAnalyzer.h"
#include "LoopGlobalPromoter.h"
#include "ModuleEffects.h"
#include "PromotionLegality.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

namespace global_iv {

    llvm::PreservedAnalyses GlobalIVPass::run(llvm::Module &module, llvm::ModuleAnalysisManager &mam) {
        ModuleEffectsAnalyzer effects_analyzer(module);
        const auto effects = effects_analyzer.analyze();

        EvolutionAnalyzer evolution_analyzer(module);
        const auto function_evolutions = evolution_analyzer.analyze_functions();

        const PromotionLegality legality(module, effects);
        const LoopGlobalPromoter promoter(effects);

        auto &function_analysis_manager = mam.getResult<llvm::FunctionAnalysisManagerModuleProxy>(module).getManager();

        bool changed = false;

        for (auto &function: module) {
            if (function.isDeclaration()) {
                continue;
            }

            auto &loop_info = function_analysis_manager.getResult<llvm::LoopAnalysis>(function);
            auto &dom_tree = function_analysis_manager.getResult<llvm::DominatorTreeAnalysis>(function);

            for (auto *loop: loop_info.getLoopsInPreorder()) {
                const auto evolutions = evolution_analyzer.analyze_loop_iteration(*loop, function_evolutions);
                if (not evolutions) {
                    continue;
                }

                for (const auto &[global, evolution]: *evolutions) {
                    if (evolution.type == EvolutionType::Unknown) {
                        continue;
                    }

                    const auto plan = legality.build_plan(function, *loop, *global, dom_tree);
                    if (not plan) {
                        continue;
                    }

                    promoter.promote(function, *plan);
                    changed = true;
                }
            }
        }

        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }

} // namespace global_iv
