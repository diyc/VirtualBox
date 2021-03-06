# Copyright © 2012 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

include Makefile.sources
include $(top_srcdir)/src/gallium/Automake.inc

AM_CFLAGS = \
	$(GALLIUM_CFLAGS) \
	$(VISIBILITY_CFLAGS) \
	$(VL_CFLAGS) \
	$(XCB_DRI3_CFLAGS) \
	$(XVMC_CFLAGS)

noinst_LTLIBRARIES = libxvmctracker.la

libxvmctracker_la_SOURCES = $(C_SOURCES)

## The tests require the XvMC library to be installed and the wrapper to
## point to the correct library (via /etc/X11/XvMCConfig).
noinst_PROGRAMS = \
	tests/xvmc_bench \
	tests/test_context \
	tests/test_surface \
	tests/test_subpicture \
	tests/test_blocks \
	tests/test_rendering

noinst_HEADERS = tests/testlib.h

TEST_LIBS = $(XVMC_LIBS) -lXvMCW $(VL_LIBS) $(XCB_DRI3_LIBS)
tests_test_context_SOURCES = tests/test_context.c tests/testlib.c
tests_test_context_LDADD = $(TEST_LIBS)
tests_test_surface_SOURCES = tests/test_surface.c tests/testlib.c
tests_test_surface_LDADD = $(TEST_LIBS)
tests_test_subpicture_SOURCES = tests/test_subpicture.c tests/testlib.c
tests_test_subpicture_LDADD = $(TEST_LIBS)
tests_test_blocks_SOURCES = tests/test_blocks.c tests/testlib.c
tests_test_blocks_LDADD = $(TEST_LIBS)
tests_test_rendering_SOURCES = tests/test_rendering.c tests/testlib.c
tests_test_rendering_LDADD = $(TEST_LIBS)
tests_xvmc_bench_SOURCES = tests/xvmc_bench.c tests/testlib.c
tests_xvmc_bench_LDADD = $(TEST_LIBS)
