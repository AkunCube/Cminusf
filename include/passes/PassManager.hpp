#pragma once

#include "Module.hpp"

#include <cassert>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

class PassManager;

/// Base class of all transform passes. A pass is created through its factory
/// function in passes.h, and the pass manager feeds the module and itself to
/// `run`, so passes can request analyses via `pm.getAnalysis<...>()`.
class Pass {
public:
  Pass(Module *m) : m_(m) {}
  virtual ~Pass() = default;

  virtual void run(PassManager &pm) = 0;

protected:
  Module *m_;
};

/// Base class of all analysis passes. Analyses are constructed lazily on the
/// first `getAnalysis<...>()` request and shared by every pass in the
/// pipeline, similar to LLVM/MLIR analysis managers.
class Analysis {
public:
  explicit Analysis(Module *m) : m_(m) {}
  virtual ~Analysis() = default;

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
      pass->run(*this);
    }
  }

private:
  std::vector<std::unique_ptr<Pass>> passes_;
  std::unordered_map<std::type_index, std::unique_ptr<Analysis>> analyses_;
  Module *m_;
};
