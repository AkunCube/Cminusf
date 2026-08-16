#pragma once

#include "Module.hpp"

#include <cassert>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PassManager;

/// Declarative description of which analyses a pass preserves, filled in by
/// `Pass::getAnalysisUsage`. Analyses that are neither named nor covered by
/// the coarse-grained flags are invalidated after the pass and recomputed on
/// the next request, so passes never have to list every analysis manually.
class AnalysisUsage {
public:
  template <typename... AnalysisTs> void preserve() {
    (preserved_.insert(std::type_index(typeid(AnalysisTs))), ...);
  }

  /// The pass does not change the CFG; every CFG-only analysis (e.g.
  /// Dominators, LoopInfo) survives automatically.
  void setPreservesCFG() { preserves_cfg_ = true; }

  /// The pass does not modify the IR at all; every analysis survives.
  void setPreservesAll() { preserves_all_ = true; }

  bool preserves(const std::type_index &key) const {
    return preserved_.find(key) != preserved_.end();
  }
  bool preservesCFG() const { return preserves_cfg_; }
  bool preservesAll() const { return preserves_all_; }

private:
  std::unordered_set<std::type_index> preserved_;
  bool preserves_cfg_ = false;
  bool preserves_all_ = false;
};

/// Base class of all transform passes. A pass is created through its factory
/// function in passes.h, and the pass manager feeds the module and itself to
/// `run`, so passes can request analyses via `pm.getAnalysis<...>()`.
class Pass {
public:
  Pass(Module *m) : m_(m) {}
  virtual ~Pass() = default;

  /// Declare which analyses this pass preserves. The default preserves
  /// nothing, i.e. every cached analysis is invalidated after the pass.
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {}

  virtual void run(PassManager &pm) = 0;

protected:
  Module *m_;
};

/// Base class of all analysis passes. Analyses are constructed lazily on the
/// first `getAnalysis<...>()` request and shared by every pass in the
/// pipeline, similar to LLVM/MLIR analysis managers. They are only valid as
/// long as the IR they were computed on is unchanged: after every pass the
/// manager drops every analysis the pass did not explicitly preserve.
class Analysis {
public:
  explicit Analysis(Module *m) : m_(m) {}
  virtual ~Analysis() = default;

  /// Whether this analysis depends only on the CFG, so it is preserved by
  /// passes that declare `AnalysisUsage::setPreservesCFG()`.
  virtual bool isCFGOnly() const { return false; }

  virtual void run() = 0;

protected:
  Module *m_;
};

class PassManager {
public:
  PassManager(Module *m) : m_(m) {}

  /// Add a transform pass created by its factory, e.g.
  /// `pm.add_pass(createDeadCode(m));`.
  void add_pass(std::unique_ptr<Pass> pass) {
    passes_.push_back(std::move(pass));
  }

  /// Return the requested analysis, constructing and running it on first use.
  template <typename AnalysisT> AnalysisT &getAnalysis() {
    auto key = std::type_index(typeid(AnalysisT));
    auto it = analyses_.find(key);
    if (it != analyses_.end()) {
      return *static_cast<AnalysisT *>(it->second.get());
    }

    std::unique_ptr<Analysis> analysis = std::make_unique<AnalysisT>(m_);
    analysis->run();

    auto inserted = analyses_.emplace(key, std::move(analysis));
    return *static_cast<AnalysisT *>(inserted.first->second.get());
  }

  void run() {
    for (auto &pass : passes_) {
      AnalysisUsage usage;
      pass->getAnalysisUsage(usage);
      pass->run(*this);
      invalidateStaleAnalyses(usage);
    }
  }

private:
  void invalidateStaleAnalyses(const AnalysisUsage &usage) {
    for (auto it = analyses_.begin(); it != analyses_.end();) {
      if (usage.preservesAll() || usage.preserves(it->first) ||
          (usage.preservesCFG() && it->second->isCFGOnly())) {
        ++it;
      } else {
        it = analyses_.erase(it);
      }
    }
  }

  std::vector<std::unique_ptr<Pass>> passes_;
  std::unordered_map<std::type_index, std::unique_ptr<Analysis>> analyses_;
  Module *m_;
};
