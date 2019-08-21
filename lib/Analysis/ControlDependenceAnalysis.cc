#include "seahorn/Analysis/ControlDependenceAnalysis.hh"

#include "llvm/Pass.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include "llvm/Analysis/IteratedDominanceFrontier.h"
#include "llvm/Analysis/PostDominators.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SparseBitVector.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "seahorn/Support/SeaDebug.h"

#define CDA_LOG(...) LOG("cda", __VA_ARGS__)

using namespace llvm;

namespace seahorn {

namespace {

class ControlDependenceAnalysisImpl
    : public seahorn::ControlDependenceAnalysis {
  Function &m_function;

  /// \brief maps basic blocks to ids in a linearization of some topo order
  DenseMap<const BasicBlock *, unsigned> m_BBToIdx;
  /// \brief list of basic blocks in topological order
  std::vector<const BasicBlock *> m_postOrderBlocks;
  /// \brief a map from a basic blocks to all reachable blocks
  mutable DenseMap<const BasicBlock *, SparseBitVector<>> m_reach;
  /// \brief maps a basic block to its control dependent blocks
  DenseMap<const BasicBlock *, SmallVector<BasicBlock *, 4>> m_cdInfo;

public:
  ControlDependenceAnalysisImpl(Function &f, PostDominatorTree &pdt)
      : m_function(f) {
    initReach();
    calculate(pdt);
  }

  ArrayRef<BasicBlock *> getCDBlocks(BasicBlock *BB) const override;
  bool isReachable(BasicBlock *Src, BasicBlock *Dst) const override;
  virtual unsigned getBBTopoIdx(llvm::BasicBlock *BB) const override {
    auto it = m_BBToIdx.find(BB);
    assert(it != m_BBToIdx.end());
    // m_BBToIdx supplies post-order numbers, RPO is topological.
    return m_BBToIdx.size() - it->second;
  }

private:
  /// \brief Computes control dependence
  // Calculates control dependence information using IteratedDominanceFrontier.
  // The resulting CD sets are sorted in reverse-topological order.
  void calculate(PostDominatorTree &PDT);

  // Initializes reachability information and PO numbering.
  void initReach();
};

void ControlDependenceAnalysisImpl::calculate(PostDominatorTree &PDT) {
  // XXX: Question: why is CD not computed incrementaly but upfront?
  // XXX: Question: what is the connection between reachability and CD?
  // XXX            seems like these are two independent concepts that are merged together
  // XXX            for no apparent reason.
  for (BasicBlock &BB : m_function) {
    CDA_LOG(errs() << BB.getName() << ":\n");
    ReverseIDFCalculator calculator(PDT);
    SmallPtrSet<BasicBlock *, 1> incoming = {&BB};

    calculator.setDefiningBlocks(incoming);
    SmallVector<BasicBlock *, 4> RDF;
    calculator.calculate(RDF);

    (void)m_cdInfo[&BB]; // Init CD set to an empty vector.

    for (auto *CD : RDF) {
      CDA_LOG(errs() << "\t" << CD->getName());
      m_cdInfo[&BB].push_back({CD});
    }
    CDA_LOG(errs() << "\n");
  }

  CDA_LOG(errs() << "\n");

  BasicBlock *entry = &m_function.getEntryBlock();

  for (auto &BBToVec : m_cdInfo) {
    SmallVectorImpl<BasicBlock *> &vec = BBToVec.second;
    // Sort in reverse-topological order.
    std::sort(vec.begin(), vec.end(),
              [this](const BasicBlock *first, const BasicBlock *second) {
                return m_BBToIdx[first] < m_BBToIdx[second];
              });
  }

  CDA_LOG(for (const BasicBlock *BB
               : llvm::reverse(m_postOrderBlocks)) {
    errs() << "cd(" << BB->getName() << "): ";
    for (BasicBlock *CDBlock : m_cdInfo[BB])
      errs() << CDBlock->getName() << ", ";
    errs() << "\n";
  });
}

void ControlDependenceAnalysisImpl::initReach() {
  m_postOrderBlocks.reserve(m_function.size());
  DenseMap<BasicBlock *, unsigned> poNums;
  poNums.reserve(m_function.size());
  unsigned num = 0;
  for (BasicBlock *BB : llvm::post_order(&m_function.getEntryBlock())) {
    m_postOrderBlocks.push_back(BB);
    poNums[BB] = num;
    m_BBToIdx[BB] = num;
    m_reach[BB].set(num);

    ++num;
  }

  // XXX: inverseSucc == predecessors?
  std::vector<SmallDenseSet<BasicBlock *, 4>> inverseSuccessors(
      m_BBToIdx.size());

  // XXX seems unnecessary since predecessors are already known to each basic
  // block
  // XXX initializing m_reach can also be done in the next loop
  for (auto &BB : m_function)
    for (auto *succ : successors(&BB)) {
      m_reach[&BB].set(m_BBToIdx[succ]);
      inverseSuccessors[m_BBToIdx[succ]].insert(&BB);
    }

  // XXX comment what this is intending to do
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto *BB : m_postOrderBlocks) {
      auto &currReach = m_reach[BB];
      for (BasicBlock *pred : inverseSuccessors[m_BBToIdx[BB]]) {
        auto &predReach = m_reach[pred];
        const size_t initSize = predReach.count();
        predReach |= currReach;
        if (predReach.count() != initSize)
          changed = true;
      }
    }
  }
}

llvm::ArrayRef<llvm::BasicBlock *>
ControlDependenceAnalysisImpl::getCDBlocks(llvm::BasicBlock *BB) const {
  auto it = m_cdInfo.find(BB);
  assert(it != m_cdInfo.end());
  return it->second;
}

bool ControlDependenceAnalysisImpl::isReachable(BasicBlock *Src,
                                                BasicBlock *Dst) const {
  auto it = m_reach.find(Src);
  assert(it != m_reach.end());
  auto dstIdxIt = m_BBToIdx.find(Dst);
  assert(dstIdxIt != m_BBToIdx.end());
  return it->second.test(dstIdxIt->second);
}

} // anonymous namespace

char ControlDependenceAnalysisPass::ID = 0;

void ControlDependenceAnalysisPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.setPreservesAll();
}

bool ControlDependenceAnalysisPass::runOnModule(llvm::Module &M) {
  bool changed = false;
  for (auto &F : M)
    if (!F.isDeclaration())
      changed |= runOnFunction(F);
  return changed;
}

bool ControlDependenceAnalysisPass::runOnFunction(llvm::Function &F) {
  CDA_LOG(llvm::errs() << "CDA: Running on " << F.getName() << "\n");

  auto &PDT = getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();
  m_analyses[&F] = llvm::make_unique<ControlDependenceAnalysisImpl>(F, PDT);
  return false;
}

void ControlDependenceAnalysisPass::print(llvm::raw_ostream &os,
                                          const llvm::Module *) const {
  os << "ControlDependenceAnalysisPass::print\n";
}

llvm::ModulePass *createControlDependenceAnalysisPass() {
  return new ControlDependenceAnalysisPass();
}

} // namespace seahorn

static llvm::RegisterPass<seahorn::ControlDependenceAnalysisPass>
    X("cd-analysis", "Compute Control Dependence", true, true);
