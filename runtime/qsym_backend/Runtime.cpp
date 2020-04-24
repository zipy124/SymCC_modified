//
// Definitions that we need for the Qsym backend
//

#include "Runtime.h"

// C++
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_set>

#ifdef DEBUG_RUNTIME
#include <chrono>
#endif

// C
#include <cstdio>

// Qsym
#include <afl_trace_map.h>
#include <call_stack_manager.h>
#include <expr_builder.h>
#include <solver.h>

// LLVM
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>

// Runtime
#include <Config.h>
#include <LibcWrappers.h>
#include <Shadow.h>

namespace qsym {

ExprBuilder *g_expr_builder;
Solver *g_solver;
CallStackManager g_call_stack_manager;
z3::context g_z3_context;

} // namespace qsym

namespace {

/// Indicate whether the runtime has been initialized.
std::atomic_flag g_initialized = ATOMIC_FLAG_INIT;

/// The file that contains out input.
std::string inputFileName;

void deleteInputFile() { std::remove(inputFileName.c_str()); }

/// A mapping of all expressions that we have ever received from Qsym to the
/// corresponding shared pointers on the heap.
///
/// We can't expect C clients to handle std::shared_ptr, so we maintain a single
/// copy per expression in order to keep the expression alive. The garbage
/// collector decides when to release our shared pointer.
///
/// std::map seems to perform slightly better than std::unordered_map on our
/// workload.
std::map<SymExpr, qsym::ExprRef> allocatedExpressions;

/// The number of allocated expressions at which we start collecting garbage.
///
/// Collecting too often hurts performance, whereas delaying garbage collection
/// for too long might make us run out of memory. The goal of this empirically
/// determined constant is to keep peek memory consumption below 2 GB on most
/// workloads because requiring that amount of memory per core participating in
/// the analysis seems reasonable.
constexpr size_t kGarbageCollectionThreshold = 5'000'000;

/// An imitation of std::span (which is not available before C++20) for symbolic
/// expressions.
using ExpressionRegion = std::pair<SymExpr *, size_t>;

/// A list of memory regions that are known to contain symbolic expressions.
std::vector<ExpressionRegion> expressionRegions;

SymExpr registerExpression(qsym::ExprRef expr) {
  SymExpr rawExpr = expr.get();

  if (allocatedExpressions.count(rawExpr) == 0) {
    // We don't know this expression yet. Create a copy of the shared pointer to
    // keep the expression alive.
    allocatedExpressions[rawExpr] = expr;
  }

  return rawExpr;
}

} // namespace

using namespace qsym;

void _sym_initialize(void) {
  if (g_initialized.test_and_set())
    return;

  loadConfig();
  initLibcWrappers();
  if (g_config.fullyConcrete)
    return;

  // Qsym requires the full input in a file
  if (g_config.inputFile.empty()) {
    std::istreambuf_iterator<char> in_begin(std::cin), in_end;
    std::vector<char> inputData(in_begin, in_end);
    inputFileName = std::tmpnam(nullptr);
    std::ofstream inputFile(inputFileName, std::ios::trunc);
    std::copy(inputData.begin(), inputData.end(),
              std::ostreambuf_iterator<char>(inputFile));
    inputFile.close();

#ifdef DEBUG_RUNTIME
    std::cout << "Loaded input:" << std::endl;
    std::copy(inputData.begin(), inputData.end(),
              std::ostreambuf_iterator<char>(std::cout));
    std::cout << std::endl;
#endif

    atexit(deleteInputFile);

    // Restore some semblance of standard input
    auto newStdin = freopen(inputFileName.c_str(), "r", stdin);
    if (newStdin == nullptr) {
      perror("Failed to reopen stdin");
      exit(-1);
    }
  } else {
    inputFileName = g_config.inputFile;
  }

  g_solver =
      new Solver(inputFileName, g_config.outputDir, g_config.aflCoverageMap);
  g_expr_builder = g_config.pruning ? PruneExprBuilder::create()
                                    : SymbolicExprBuilder::create();
}

SymExpr _sym_build_integer(uint64_t value, uint8_t bits) {
  // Qsym's API takes uintptr_t, so we need to be careful when compiling for
  // 32-bit systems: the compiler would helpfully truncate our uint64_t to fit
  // into 32 bits.
  if constexpr (sizeof(uint64_t) == sizeof(uintptr_t)) {
    // 64-bit case: all good.
    return registerExpression(g_expr_builder->createConstant(value, bits));
  } else {
    // 32-bit case: use the regular API if possible, otherwise create an
    // llvm::APInt.
    if (uintptr_t value32 = value; value32 == value) {
      return registerExpression(g_expr_builder->createConstant(value32, bits));
    } else {
      return registerExpression(
          g_expr_builder->createConstant({64, value}, bits));
    }
  }
}

SymExpr _sym_build_integer128(uint64_t high, uint64_t low) {
  std::array<uint64_t, 2> words = {low, high};
  return registerExpression(g_expr_builder->createConstant({128, words}, 128));
}

SymExpr _sym_build_null_pointer() {
  return registerExpression(
      g_expr_builder->createConstant(0, sizeof(uintptr_t) * 8));
}

SymExpr _sym_build_true() {
  return registerExpression(g_expr_builder->createTrue());
}

SymExpr _sym_build_false() {
  return registerExpression(g_expr_builder->createFalse());
}

SymExpr _sym_build_bool(bool value) {
  return registerExpression(g_expr_builder->createBool(value));
}

#define DEF_BINARY_EXPR_BUILDER(name, qsymName)                                \
  SymExpr _sym_build_##name(SymExpr a, SymExpr b) {                            \
    return registerExpression(g_expr_builder->create##qsymName(                \
        allocatedExpressions[a], allocatedExpressions[b]));                    \
  }

DEF_BINARY_EXPR_BUILDER(add, Add)
DEF_BINARY_EXPR_BUILDER(sub, Sub)
DEF_BINARY_EXPR_BUILDER(mul, Mul)
DEF_BINARY_EXPR_BUILDER(unsigned_div, UDiv)
DEF_BINARY_EXPR_BUILDER(signed_div, SDiv)
DEF_BINARY_EXPR_BUILDER(unsigned_rem, URem)
DEF_BINARY_EXPR_BUILDER(signed_rem, SRem)

DEF_BINARY_EXPR_BUILDER(shift_left, Shl)
DEF_BINARY_EXPR_BUILDER(logical_shift_right, LShr)
DEF_BINARY_EXPR_BUILDER(arithmetic_shift_right, AShr)

DEF_BINARY_EXPR_BUILDER(signed_less_than, Slt)
DEF_BINARY_EXPR_BUILDER(signed_less_equal, Sle)
DEF_BINARY_EXPR_BUILDER(signed_greater_than, Sgt)
DEF_BINARY_EXPR_BUILDER(signed_greater_equal, Sge)
DEF_BINARY_EXPR_BUILDER(unsigned_less_than, Ult)
DEF_BINARY_EXPR_BUILDER(unsigned_less_equal, Ule)
DEF_BINARY_EXPR_BUILDER(unsigned_greater_than, Ugt)
DEF_BINARY_EXPR_BUILDER(unsigned_greater_equal, Uge)
DEF_BINARY_EXPR_BUILDER(equal, Equal)
DEF_BINARY_EXPR_BUILDER(not_equal, Distinct)

DEF_BINARY_EXPR_BUILDER(bool_and, LAnd)
DEF_BINARY_EXPR_BUILDER(and, And)
DEF_BINARY_EXPR_BUILDER(bool_or, LOr)
DEF_BINARY_EXPR_BUILDER(or, Or)
DEF_BINARY_EXPR_BUILDER(bool_xor, Distinct)
DEF_BINARY_EXPR_BUILDER(xor, Xor)

#undef DEF_BINARY_EXPR_BUILDER

SymExpr _sym_build_neg(SymExpr expr) {
  return registerExpression(
      g_expr_builder->createNeg(allocatedExpressions[expr]));
}

SymExpr _sym_build_not(SymExpr expr) {
  return registerExpression(
      g_expr_builder->createNot(allocatedExpressions[expr]));
}

SymExpr _sym_build_sext(SymExpr expr, uint8_t bits) {
  return registerExpression(g_expr_builder->createSExt(
      allocatedExpressions[expr], bits + expr->bits()));
}

SymExpr _sym_build_zext(SymExpr expr, uint8_t bits) {
  return registerExpression(g_expr_builder->createZExt(
      allocatedExpressions[expr], bits + expr->bits()));
}

SymExpr _sym_build_trunc(SymExpr expr, uint8_t bits) {
  return registerExpression(
      g_expr_builder->createTrunc(allocatedExpressions[expr], bits));
}

void _sym_push_path_constraint(SymExpr constraint, int taken,
                               uintptr_t site_id) {
  if (!constraint)
    return;

  g_solver->addJcc(allocatedExpressions[constraint], taken, site_id);
}

SymExpr _sym_get_input_byte(size_t offset) {
  return registerExpression(g_expr_builder->createRead(offset));
}

SymExpr _sym_concat_helper(SymExpr a, SymExpr b) {
  return registerExpression(g_expr_builder->createConcat(
      allocatedExpressions[a], allocatedExpressions[b]));
}

SymExpr _sym_extract_helper(SymExpr expr, size_t first_bit, size_t last_bit) {
  return registerExpression(g_expr_builder->createExtract(
      allocatedExpressions[expr], last_bit, first_bit - last_bit + 1));
}

size_t _sym_bits_helper(SymExpr expr) { return expr->bits(); }

SymExpr _sym_build_bool_to_bits(SymExpr expr, uint8_t bits) {
  return registerExpression(
      g_expr_builder->boolToBit(allocatedExpressions[expr], bits));
}

//
// Floating-point operations (unsupported in Qsym)
//

#define UNSUPPORTED(prototype)                                                 \
  prototype { return nullptr; }

UNSUPPORTED(SymExpr _sym_build_float(double value, int is_double))
UNSUPPORTED(SymExpr _sym_build_fp_add(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_fp_sub(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_fp_mul(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_fp_div(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_fp_rem(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_fp_abs(SymExpr a))
UNSUPPORTED(SymExpr _sym_build_float_ordered_greater_than(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_ordered_greater_equal(SymExpr a,
                                                           SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_ordered_less_than(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_ordered_less_equal(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_ordered_equal(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_ordered_not_equal(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_ordered(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_unordered(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_unordered_greater_than(SymExpr a,
                                                            SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_unordered_greater_equal(SymExpr a,
                                                             SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_unordered_less_than(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_unordered_less_equal(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_unordered_equal(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_float_unordered_not_equal(SymExpr a, SymExpr b))
UNSUPPORTED(SymExpr _sym_build_int_to_float(SymExpr value, int is_double,
                                            int is_signed))
UNSUPPORTED(SymExpr _sym_build_float_to_float(SymExpr expr, int to_double))
UNSUPPORTED(SymExpr _sym_build_bits_to_float(SymExpr expr, int to_double))
UNSUPPORTED(SymExpr _sym_build_float_to_bits(SymExpr expr))
UNSUPPORTED(SymExpr _sym_build_float_to_signed_integer(SymExpr expr,
                                                       uint8_t bits))
UNSUPPORTED(SymExpr _sym_build_float_to_unsigned_integer(SymExpr expr,
                                                         uint8_t bits))

#undef UNSUPPORTED
#undef H

//
// Call-stack tracing
//

void _sym_notify_call(uintptr_t site_id) {
  g_call_stack_manager.visitCall(site_id);
}

void _sym_notify_ret(uintptr_t site_id) {
  g_call_stack_manager.visitRet(site_id);
}

void _sym_notify_basic_block(uintptr_t site_id) {
  g_call_stack_manager.visitBasicBlock(site_id);
}

//
// Debugging
//

const char *_sym_expr_to_string(SymExpr expr) {
  static char buffer[4096];

  auto expr_string = expr->toString();
  auto copied = expr_string.copy(
      buffer, std::min(expr_string.length(), sizeof(buffer) - 1));
  buffer[copied] = '\0';

  return buffer;
}

bool _sym_feasible(SymExpr expr) {
  expr->simplify();

  g_solver->push();
  g_solver->add(expr->toZ3Expr());
  bool feasible = (g_solver->check() == z3::sat);
  g_solver->pop();

  return feasible;
}

//
// Garbage collection
//

void _sym_register_expression_region(SymExpr *start, size_t length) {
  expressionRegions.push_back({start, length});
}

void _sym_collect_garbage() {
  if (allocatedExpressions.size() < kGarbageCollectionThreshold)
    return;

#ifdef DEBUG_RUNTIME
  auto start = std::chrono::high_resolution_clock::now();
#endif

  std::unordered_set<SymExpr> reachableExpressions;
  auto collectReachableExpressions = [&](SymExpr *start, SymExpr *end) {
    for (SymExpr *expr_ptr = start; expr_ptr < end; expr_ptr++) {
      if (*expr_ptr != nullptr) {
        reachableExpressions.insert(*expr_ptr);
      }
    }
  };

  for (auto &[start, length] : expressionRegions) {
    collectReachableExpressions(start, start + length);
  }

  for (auto &[concrete, symbolic] : g_shadow_pages) {
    collectReachableExpressions(symbolic, symbolic + kPageSize);
  }

  for (auto expr_it = allocatedExpressions.begin();
       expr_it != allocatedExpressions.end();) {
    if (reachableExpressions.count(expr_it->first) == 0) {
      expr_it = allocatedExpressions.erase(expr_it);
    } else {
      ++expr_it;
    }
  }

#ifdef DEBUG_RUNTIME
  auto end = std::chrono::high_resolution_clock::now();

  std::cout << "After garbage collection: " << allocatedExpressions.size()
            << " expressions remain" << std::endl
            << "\t(collection took "
            << std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                     start)
                   .count()
            << " milliseconds)" << std::endl;
#endif
}
