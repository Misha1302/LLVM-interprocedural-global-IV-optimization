#include "Evolution.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/Casting.h"

namespace global_iv {

    Evolution make_unknown_evolution() {
        return {EvolutionType::Unknown, {llvm::APInt::getZero(1), llvm::APInt::getZero(1)}};
    }

    Evolution make_identity_evolution(const llvm::GlobalVariable &global) {
        const auto *integer_type = llvm::cast<llvm::IntegerType>(global.getValueType());
        const auto bit_width = integer_type->getBitWidth();

        return {EvolutionType::Linear, {llvm::APInt::getZero(bit_width), llvm::APInt(bit_width, 1)}};
    }

} // namespace global_iv
