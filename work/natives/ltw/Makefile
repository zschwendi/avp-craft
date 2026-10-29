# libltw for Darwin (macOS / visionOS device / visionOS simulator), over ANGLE ES 3.0.
#
#   make                      # macOS arm64  -> build/macosx/libltw.dylib
#   make SDK=xros             # visionOS     -> build/xros/libltw.dylib       (compile+link only)
#   make SDK=xrsimulator      # visionOS sim -> build/xrsimulator/libltw.dylib (compile+link only)
#   make test                 # macOS: build + run test/ltw_test (needs ANGLE in $(ANGLE_MAC))
#
# The upstream clone (work/src/PojavLauncherTeam_ltw) is compiled IN PLACE and never edited; every
# Darwin difference lives in darwin/ (prefix header, shim .c files, a header overlay dir).
# Source lists are read straight out of upstream's Android.mk so they cannot drift.

SDK      ?= macosx
LTW      ?= $(abspath ../../src/PojavLauncherTeam_ltw/ltw/src/main/tinywrapper)
ROOT     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
ANGLE_INC?= $(ROOT)/vendor/include
ANGLE_MAC?= $(ROOT)/vendor/out/Debug
# header-only: nir_print.c names VkDescriptorType values (the NDK sysroot ships these headers).
# Present after `make angle-fetch` (vendor/third_party/vulkan-headers). Header-only.
VK_INC   ?= $(ROOT)/vendor/third_party/vulkan-headers/src/include
HERE     := $(abspath .)
OUT      := $(HERE)/build/$(SDK)
OBJ      := $(OUT)/obj

ifeq ($(SDK),macosx)
  TARGET := arm64-apple-macos13.0
else ifeq ($(SDK),xros)
  TARGET := arm64-apple-xros26.0
else ifeq ($(SDK),xrsimulator)
  TARGET := arm64-apple-xros26.0-simulator
else
  $(error SDK must be macosx, xros or xrsimulator)
endif

CC  := xcrun --sdk $(SDK) clang -target $(TARGET)
CXX := xcrun --sdk $(SDK) clang++ -target $(TARGET)

G := $(LTW)/glsl_optimizer
# Upstream flags minus the Android ones (ANDROID, DETECT_OS_ANDROID): Mesa's detect_os.h then
# picks DETECT_OS_APPLE + DETECT_OS_POSIX by itself.
DEFS := -D_LIB -DNOMINMAX -D_USE_MATH_DEFINES -D__STDC_NO_THREADS__ -D__STDC_LIMIT_MACROS \
        -D__STDC_FORMAT_MACROS -D__STDC_CONSTANT_MACROS -DUTIL_ARCH_LITTLE_ENDIAN -DUNIX \
        -DHAVE_STRUCT_TIMESPEC -DHAVE_OPENGL -DHAVE_OPENGL_ES_1 -DHAVE_OPENGL_ES_2 \
        -DHAVE_TIMESPEC_GET
# darwin/include first (android/log.h, threads.h overlays), then upstream, then ANGLE's Khronos headers.
INCS := -I$(HERE)/darwin/include -I$(LTW) -I$(G)/include -I$(G)/src -I$(G)/src/mesa \
        -I$(G)/src/compiler -I$(G)/src/compiler/glsl -I$(G)/src/compiler/nir \
        -I$(G)/src/gallium/include -I$(G)/src/util -I$(ANGLE_INC) -idirafter $(VK_INC)
WARN := -Wno-everything
COMMON := -O2 -g -fPIC $(DEFS) $(INCS) $(WARN) -include $(HERE)/darwin/ltw_darwin_prefix.h
CFLAGS   := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=gnu++17

# --- source lists, parsed from Android.mk -----------------------------------------------------
MK := $(LTW)/Android.mk
# module 1 = glsl_optimizer (static, -fvisibility=hidden), module 2 = ltw (shared).
GLSL_SRCS := $(shell awk '/LOCAL_MODULE := glsl_optimizer/{m=1} /LOCAL_MODULE := ltw/{m=2} m==1' $(MK) \
               | grep -o 'glsl_optimizer/[^ \\]*\.c\(pp\)\?' )
# Android-only backtrace implementation (u_debug_stack.c carries the generic one).
GLSL_SRCS := $(filter-out glsl_optimizer/src/util/u_debug_stack_android.cpp,$(GLSL_SRCS))
LTW_SRCS  := $(shell awk '/LOCAL_MODULE := ltw/{m=1} m==1' $(MK) | grep -o '[A-Za-z_/]*\.c\b' )
SHIM_SRCS := darwin/ltw_darwin.c darwin/ltw_darwin_gl.c darwin/ltw_darwin_gl33.c darwin/ltw_darwin_buf.c

GLSL_OBJS := $(patsubst %,$(OBJ)/glsl/%.o,$(GLSL_SRCS))
LTW_OBJS  := $(patsubst %,$(OBJ)/ltw/%.o,$(LTW_SRCS))
SHIM_OBJS := $(patsubst %,$(OBJ)/shim/%.o,$(SHIM_SRCS))

DYLIB := $(OUT)/libltw.dylib

all: $(DYLIB)

$(OBJ)/glsl/%.c.o: $(LTW)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -fvisibility=hidden -c $< -o $@
$(OBJ)/glsl/%.cpp.o: $(LTW)/%.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -fvisibility=hidden -fvisibility-inlines-hidden -c $< -o $@
$(OBJ)/ltw/%.c.o: $(LTW)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@
# proc.c is the only upstream file with loader assumptions (dlopen libEGL.so, dlsym(NULL)).
$(OBJ)/ltw/proc.c.o: CFLAGS += -include $(HERE)/darwin/ltw_darwin_proc.h
# Follow-up 1: rename the upstream definitions that darwin/ltw_darwin_gl.c wraps (version report,
# base-vertex draws on ES 3.0). Upstream files stay pristine; the renamed copies are unexported.
$(OBJ)/ltw/main.c.o: CFLAGS += -include $(HERE)/darwin/ltw_darwin_rename_main.h
$(OBJ)/ltw/basevertex.c.o: CFLAGS += -include $(HERE)/darwin/ltw_darwin_rename_basevertex.h
# M3 stage A: cube-map face uploads -> swizzle tracker queries GL_TEXTURE_CUBE_MAP, not the face (INVALID_ENUM).
$(OBJ)/ltw/swizzle.c.o: CFLAGS += -include $(HERE)/darwin/ltw_darwin_swizzle_cube.h
# M3 stage A: glsl_optimizer's printer keeps a process-global, never-reset list of printed uniform blocks
# (UBO shaders lost their block declarations, then an assert aborted after 64 names) -> reset per Convert.
$(OBJ)/glsl/glsl_optimizer/src/code/ir_print_glsl_visitor.cpp.o: CXXFLAGS += -include $(HERE)/darwin/ltw_darwin_ubo_reset.h
# M3 stage A: texture buffers (clouds) -> darwin/ltw_darwin_gl.c owns glShaderSource (source rewrite first).
$(OBJ)/ltw/shader_wrapper.c.o: CFLAGS += -include $(HERE)/darwin/ltw_darwin_rename_shader.h
$(OBJ)/ltw/shader_wrapper.c.o: $(HERE)/darwin/ltw_darwin_rename_shader.h
# The force-included overlays above are prerequisites of their objects, so an edit to one rebuilds that
# object (before M3 stage A an incremental `make` silently kept the stale object).
$(OBJ)/ltw/proc.c.o: $(HERE)/darwin/ltw_darwin_proc.h
$(OBJ)/ltw/main.c.o: $(HERE)/darwin/ltw_darwin_rename_main.h
$(OBJ)/ltw/basevertex.c.o: $(HERE)/darwin/ltw_darwin_rename_basevertex.h
$(OBJ)/ltw/swizzle.c.o: $(HERE)/darwin/ltw_darwin_swizzle_cube.h
$(OBJ)/glsl/glsl_optimizer/src/code/ir_print_glsl_visitor.cpp.o: $(HERE)/darwin/ltw_darwin_ubo_reset.h
$(OBJ)/shim/%.c.o: $(HERE)/%.c $(wildcard $(HERE)/darwin/*.h)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/libglsl_optimizer.a: $(GLSL_OBJS)
	@rm -f $@; xcrun --sdk $(SDK) libtool -static -o $@ $^ 2>/dev/null

# libltw does NOT link ANGLE: like upstream it dlopen()s libEGL at load and pulls every GLES entry
# point through ANGLE's eglGetProcAddress (see darwin/ltw_darwin.c for the search order).
$(DYLIB): $(LTW_OBJS) $(SHIM_OBJS) $(OUT)/libglsl_optimizer.a
	$(CXX) -dynamiclib -install_name @rpath/libltw.dylib -Wl,-rpath,@loader_path \
	    '-Wl,-unexported_symbol,_ltw_upstream_*' -o $@ $(LTW_OBJS) $(SHIM_OBJS) \
	    $(OUT)/libglsl_optimizer.a
	@echo "built $@"

# --- macOS test ---------------------------------------------------------------------------------
TEST := $(OUT)/ltw_test
$(TEST): test/ltw_test.c $(DYLIB)
	$(CC) -O0 -g -I$(ANGLE_INC) test/ltw_test.c -o $@ -lz -Wl,-rpath,$(ANGLE_MAC)

# Four passes over the same binary (ltw_test reads LTW_TEST_EXPECT_GL / LTW_TEST_EXPECT_BASEVERTEX):
#   1. shipping defaults: base-vertex quads drawn (EXT route on macOS ANGLE), GL reported as 3.2;
#   2. LTW_DARWIN_BASEVERTEX_EXT=0 (+EMU=1): the attrib-offset EMULATION route the visionOS simulator
#      takes (its ANGLE has no base-vertex extension) -> quads drawn, VAO state restored;
#   3. negative control, EXT=0 EMU=0: upstream ES 3.0 path -> quads ABSENT, while the version report
#      still says 3.2 (proves ltw internally still treats the backend as ES 3.0);
#   4. LTW_REPORT_GL_VERSION=off: upstream "3.0 OpenLTW" / 3,0 again.
# Then two GL 3.3 passes (test/ltw_gl33_test.c, M3 stage C): the Minecraft 26.x surface (3.3 / GLSL 3.30, hidden ARB
# names, the 26.2 entry-point table -> test/ltw_gl33_test.log, honest errors), and LTW_DARWIN_HIDE_EXTENSIONS.
RUN := cd $(OUT) && LTW_EGL_LIBRARY=$(ANGLE_MAC)/libEGL.dylib
TEST33 := $(OUT)/ltw_gl33_test
$(TEST33): test/ltw_gl33_test.c $(DYLIB)
	$(CC) -O0 -g -I$(ANGLE_INC) test/ltw_gl33_test.c -o $@ -Wl,-rpath,$(ANGLE_MAC)

TESTBUF := $(OUT)/ltw_buf_test
$(TESTBUF): test/ltw_buf_test.c $(DYLIB)
	$(CC) -O0 -g -I$(ANGLE_INC) test/ltw_buf_test.c -o $@ -Wl,-rpath,$(ANGLE_MAC)

test: $(TEST) $(TEST33) $(TESTBUF)
	$(RUN) ./ltw_test $(DYLIB) $(ANGLE_MAC) $(OUT)/ltw_test.png
	$(RUN) LTW_DARWIN_BASEVERTEX_EXT=0 LTW_DARWIN_BASEVERTEX_EMU=1 \
	    ./ltw_test $(DYLIB) $(ANGLE_MAC) $(OUT)/ltw_test_emulated.png
	$(RUN) LTW_DARWIN_BASEVERTEX_EXT=0 LTW_DARWIN_BASEVERTEX_EMU=0 LTW_TEST_EXPECT_BASEVERTEX=0 \
	    ./ltw_test $(DYLIB) $(ANGLE_MAC) $(OUT)/ltw_test_nobasevertex.png
	$(RUN) LTW_REPORT_GL_VERSION=off LTW_TEST_EXPECT_GL=3.0 \
	    ./ltw_test $(DYLIB) $(ANGLE_MAC) $(OUT)/ltw_test_noreport.png
	@echo "make test: all four passes PASSED"
	$(RUN) LTW_REPORT_GL_VERSION=3.3 ./ltw_gl33_test $(DYLIB) $(ANGLE_MAC) $(HERE)/test/gl33_names.txt | tee $(HERE)/test/ltw_gl33_test.log
	$(RUN) LTW_REPORT_GL_VERSION=3.3 LTW_DARWIN_HIDE_EXTENSIONS="GL_ARB_direct_state_access GL_ARB_buffer_storage GL_EXT_texture_filter_anisotropic GL_KHR_debug" \
	    LTW_TEST_HIDDEN="GL_EXT_texture_filter_anisotropic GL_KHR_debug" ./ltw_gl33_test $(DYLIB) $(ANGLE_MAC) $(HERE)/test/gl33_names.txt > $(OUT)/ltw_gl33_test_hide.log
	@echo "make test: GL 3.3 passes (26.2 surface, extension hiding) PASSED"
	$(RUN) ./ltw_buf_test $(DYLIB) $(ANGLE_MAC) | tee $(HERE)/test/ltw_buf_test.log
	$(RUN) LTW_DARWIN_STAGE_BLOCK=64k ./ltw_buf_test $(DYLIB) $(ANGLE_MAC) > $(OUT)/ltw_buf_test_ring.log
	$(RUN) LTW_DARWIN_SUBDATA=off ./ltw_buf_test $(DYLIB) $(ANGLE_MAC) > $(OUT)/ltw_buf_test_off.log
	@echo "make test: staged buffer uploads (default, 64k ring, off) PASSED"

symbols: $(DYLIB)
	nm -gU $(DYLIB) | awk '{print $$3}' | sort > $(OUT)/exports.txt
	@wc -l < $(OUT)/exports.txt

clean:
	rm -rf $(OUT)

.PHONY: all test symbols clean
