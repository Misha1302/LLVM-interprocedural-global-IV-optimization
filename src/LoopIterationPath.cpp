#include "LoopIterationPath.h"

#include <unordered_set>

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"


namespace global_iv {
    std::optional<LoopIterationPath> build_loop_iteration_path(const llvm::Loop &loop) {
        LoopIterationPath blocks;
        std::unordered_set<const llvm::BasicBlock *> visited;

        const auto *header = loop.getHeader();
        const auto *block = header;

        while (true) {
            if (not visited.insert(block).second) {
                return std::nullopt;
            }

            blocks.push_back(block);

            const auto *terminator = block->getTerminator();
            const llvm::BasicBlock *next = nullptr;

            for (auto i = 0ll; i < terminator->getNumSuccessors(); ++i) {
                const auto *successor = terminator->getSuccessor(i);
                if (not loop.contains(successor)) {
                    continue;
                }

                if (next) {
                    return std::nullopt;
                }

                next = successor;
            }

            if (not next) {
                return std::nullopt;
            }

            if (next == header) {
                break;
            }

            block = next;
        }

        return blocks;
    }
} // namespace global_iv
