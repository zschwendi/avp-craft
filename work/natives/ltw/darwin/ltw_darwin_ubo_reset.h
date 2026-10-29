/* Force-included into upstream glsl_optimizer src/code/ir_print_glsl_visitor.cpp ONLY (Makefile target-specific
 * flag), M3 stage A (Minecraft 1.21.11).
 * IR_TO_GLSL keeps the uniform-block names it has already printed in a STATIC, process-global array of 64
 * (processed_uniform_blocks / num_uniform_blocks) and never resets it. Its purpose is per shader: an
 * instance-less block shows up as one ir_variable per member, and the block must be declared once. Across
 * shaders it is wrong twice: (1) a block name already printed for an EARLIER shader (Minecraft 1.21.6+ shares
 * `Projection`, `Globals`, `DynamicTransforms`, `Fog`, ... across 22 std140 shaders) is skipped, so later
 * shaders lose their block declaration; (2) after 64 distinct names the assert aborts the whole process
 * ("Assertion failed: (num_uniform_blocks < sizeof( processed_uniform_blocks ) ...)", ir_print_glsl_visitor.cpp
 * :2154; logs/t4-m3a-jvm-world4.txt). 1.21.5's #version 150 shaders had no uniform blocks, so this never ran.
 * Fix: reset the list at the start of every IR_TO_GLSL::Convert. Convert calls analyze_loop_variables() exactly
 * once, before it prints anything, so a function-like macro on that call does the reset; the declaration in
 * loop_analysis.h is included (guarded) BEFORE the macro exists. */
#ifndef LTW_DARWIN_UBO_RESET_H
#define LTW_DARWIN_UBO_RESET_H
#include <inttypes.h>
#include "code/ir_print_glsl_visitor.h"
#include "compiler/glsl/loop_analysis.h"
#define analyze_loop_variables(instr) (IR_TO_GLSL::num_uniform_blocks = 0, analyze_loop_variables(instr))
#endif
