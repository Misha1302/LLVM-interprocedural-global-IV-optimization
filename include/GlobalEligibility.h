#pragma once

namespace llvm {
    class GlobalVariable;
}

namespace global_iv {

    bool can_analyze_iv_global(const llvm::GlobalVariable &global);

} // namespace global_iv
