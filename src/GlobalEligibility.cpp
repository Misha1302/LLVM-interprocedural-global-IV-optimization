#include "GlobalEligibility.h"

#include "llvm/Analysis/ValueLatticeUtils.h"
#include "llvm/IR/GlobalVariable.h"

namespace global_iv {

    bool can_analyze_iv_global(const llvm::GlobalVariable &global) {
        return global.getValueType()->isIntegerTy()
               and llvm::canTrackGlobalVariableInterprocedurally(const_cast<llvm::GlobalVariable *>(&global));
    }

} // namespace global_iv
