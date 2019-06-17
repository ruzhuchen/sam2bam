#     (C) Copyright IBM Corp. 2016
# 
#     Author: Takeshi Ogasawara, IBM Research - Tokyo
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

PLATFORM = $(shell uname -m)
ifeq ($(wildcard /opt/at12.0),)
CC       = gcc
else
CC       = /opt/at12.0/bin/gcc
endif
ifeq ($(PLATFORM), ppc64le)
CFLAGS   = -fpic -g -Wall -O2 -mcpu=power9 -mtune=power9
else
CFLAGS   = -fpic -g -Wall -O2
endif
CPPFLAGS = -I../samtools -I../htslib -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE
LDFLAGS = -shared -Wl,-soname,$(PROGRAM).$(LIB_VERSION) -pthread -lz
HTSDIR=../htslib

LIB_VERSION = 1.0.0
PROGRAM = lib_pre_filter.so
LIB_OBJS = prefilter.o

all: $(PROGRAM)

-include $(HTSDIR)/htslib.mk

prefilter.o: ../samtools/bam_sam2bam.h

$(PROGRAM): $(LIB_OBJS) $(HTSDIR)/libhts.so
	$(CC) $(LDFLAGS) -o $@ $(LIB_OBJS) $(HTSDIR)/*.pico

clean: 
	-rm -f $(PROGRAM) $(LIB_OBJS)
