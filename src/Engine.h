// -*- c++ -*-
#ifndef REGEXBENCH_ENGINE_H
#define REGEXBENCH_ENGINE_H

#include <vector>

#include "Rule.h"

namespace regexbench {

class Engine {
public:
  virtual ~Engine();

  virtual void compile(const std::vector<Rule>&, size_t) {}
  virtual void init(size_t) {}
  virtual size_t getNumThreads() { return numThreads; }
  virtual void load(const std::string&, size_t) {}
  virtual size_t match(const char*, size_t, size_t, size_t = 0) = 0;
protected:
  size_t numThreads = 1;
};

} // namespace regexbench

#endif
