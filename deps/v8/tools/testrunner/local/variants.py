# Copyright 2016 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Use this to run several variants of the tests.
ALL_VARIANT_FLAGS = {
  "assert_types": [["--assert-types"]],
  "code_serializer": [["--cache=code"]],
  "default": [[]],
  "future": [["--future"]],
  "gc_stats": [["--gc-stats=1"]],
  # Alias of exhaustive variants, but triggering new test framework features.
  "infra_staging": [[]],
  "interpreted_regexp": [["--regexp-interpret-all"]],
  "experimental_regexp":  [["--default-to-experimental-regexp-engine"]],
  "jitless": [["--jitless"]],
  "minor_mc": [["--minor-mc"]],
  "nci": [["--turbo-nci"]],
  "nci_as_midtier": [["--turbo-nci-as-midtier"]],
  "no_lfa": [["--no-lazy-feedback-allocation"]],
  "no_local_heaps": [[
      "--no-local-heaps",
      "--no-turbo-direct-heap-access",
      "--no-finalize-streaming-on-background"]],
  # No optimization means disable all optimizations. OptimizeFunctionOnNextCall
  # would not force optimization too. It turns into a Nop. Please see
  # https://chromium-review.googlesource.com/c/452620/ for more discussion.
  # For WebAssembly, we test "Liftoff-only" in the nooptimization variant and
  # "TurboFan-only" in the stress variant. The WebAssembly configuration is
  # independent of JS optimizations, so we can combine those configs.
  "nooptimization": [["--no-opt", "--liftoff", "--no-wasm-tier-up",
                      "--wasm-generic-wrapper"]],
  "slow_path": [["--force-slow-path"]],
  "stress": [["--stress-opt", "--no-liftoff", "--stress-lazy-source-positions"]],
  "stress_concurrent_allocation": [["--stress-concurrent-allocation"]],
  "stress_js_bg_compile_wasm_code_gc": [["--stress-background-compile",
                                         "--finalize-streaming-on-background",
                                         "--stress-wasm-code-gc"]],
  "stress_incremental_marking": [["--stress-incremental-marking"]],
  "stress_snapshot": [["--stress-snapshot"]],
  # Trigger stress sampling allocation profiler with sample interval = 2^14
  "stress_sampling": [["--stress-sampling-allocation-profiler=16384"]],
  "trusted": [["--no-untrusted-code-mitigations"]],
  "no_wasm_traps": [["--no-wasm-trap-handler"]],
  "turboprop": [["--turboprop"]],
  "instruction_scheduling": [["--turbo-instruction-scheduling"]],
  "stress_instruction_scheduling": [["--turbo-stress-instruction-scheduling"]],
  "top_level_await": [["--harmony-top-level-await"]],
}

# Flags that lead to a contradiction with the flags provided by the respective
# variant. This depends on the flags specified in ALL_VARIANT_FLAGS and on the
# implications defined in flag-definitions.h.
INCOMPATIBLE_FLAGS_PER_VARIANT = {
  "assert_types": ["--no-assert-types"],
  "jitless": ["--opt", "--always-opt", "--liftoff", "--track-field-types", "--validate-asm"],
  "no_wasm_traps": ["--wasm-trap-handler"],
  "nooptimization": ["--opt", "--always-opt", "--no-liftoff", "--predictable", "--wasm-tier-up"],
  "slow_path": ["--no-force-slow-path"],
  "stress_concurrent_allocation": ["--single-threaded-gc", "--predictable"],
  "stress_incremental_marking": ["--no-stress-incremental-marking"],
  "future": ["--parallel-compile-tasks"],
  "stress_js_bg_compile_wasm_code_gc": ["--no-stress-background-compile", "--parallel-compile-tasks"],
  "stress": ["--no-stress-opt", "--always-opt", "--no-always-opt", "--liftoff", "--max-inlined-bytecode-size=*",
             "--max-inlined-bytecode-size-cumulative=*", "--stress-inline"],
  "turboprop": ["--interrupt-budget=*", "--no-turboprop"],
  "code_serializer": ["--cache=after-execute", "--cache=full-code-cache", "--cache=none"],
  "no_local_heaps": ["--concurrent-inlining"],
  "experimental_regexp": ["--no-enable-experimental-regexp-engine", "--no-default-to-experimental-regexp-engine"],
}

# Flags that lead to a contradiction under certain build variables.
# This corresponds to the build variables usable in status files as generated
# in _get_statusfile_variables in base_runner.py.
# The conflicts might be directly contradictory flags or be caused by the
# implications defined in flag-definitions.h.
INCOMPATIBLE_FLAGS_PER_BUILD_VARIABLE = {
  "lite_mode": ["--no-lazy-feedback-allocation", "--max-semi-space-size=*"]
               + INCOMPATIBLE_FLAGS_PER_VARIANT["jitless"],
  "predictable": ["--liftoff", "--parallel-compile-tasks",
                  "--concurrent-recompilation",
                  "--wasm-num-compilation-tasks=*",
                  "--stress-concurrent-allocation"],
}

# Flags that lead to a contradiction when a certain extra-flag is present.
# Such extra-flags are defined for example in infra/testing/builders.pyl or in
# standard_runner.py.
# The conflicts might be directly contradictory flags or be caused by the
# implications defined in flag-definitions.h.
INCOMPATIBLE_FLAGS_PER_EXTRA_FLAG = {
  "--concurrent-recompilation": ["--no-concurrent-recompilation", "--predictable"],
  "--enable-armv8": ["--no-enable-armv8"],
  "--gc-interval=*": ["--gc-interval=*"],
  "--no-enable-sse3": ["--enable-sse3"],
  "--no-enable-sse4-1": ["--enable-sse4-1"],
  "--optimize-for-size": ["--max-semi-space-size=*"],
  "--stress_concurrent_allocation": ["--single-threaded-gc", "--predictable"],
  "--stress-flush-bytecode": ["--no-stress-flush-bytecode"],
  "--stress-incremental-marking": INCOMPATIBLE_FLAGS_PER_VARIANT["stress_incremental_marking"],
}

SLOW_VARIANTS = set([
  'stress',
  'stress_snapshot',
  'nooptimization',
])

FAST_VARIANTS = set([
  'default'
])


def _variant_order_key(v):
  if v in SLOW_VARIANTS:
    return 0
  if v in FAST_VARIANTS:
    return 100
  return 50

ALL_VARIANTS = sorted(ALL_VARIANT_FLAGS.keys(),
                      key=_variant_order_key)

# Check {SLOW,FAST}_VARIANTS entries
for variants in [SLOW_VARIANTS, FAST_VARIANTS]:
  for v in variants:
    assert v in ALL_VARIANT_FLAGS
