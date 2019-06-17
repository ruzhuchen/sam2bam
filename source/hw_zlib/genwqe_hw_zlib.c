/*  genwqe_hw_zlib.c

    (C) Copyright IBM Corp. 2016

    Author: Takeshi Ogasawara, IBM Research - Tokyo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

#define HW_ZLIB

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <zlib.h>
#include "htslib/bgzf.h"
/*
static int (*p_hw_deflateInit2_)(z_streamp, int, int, int, int, int, const char *, int);
static int (*p_hw_deflate)(z_streamp, int);
static int (*p_hw_deflateReset)(z_streamp);
*/
int init_hw_zlib(hw_zlib_api_t *api) {
  int rc = 0;
  enum type { genwqe, capi, n_type};
  enum type type_arr[n_type] = { genwqe, capi }; 
  const char * type_name[n_type] = { "GENWQE", "CAPI" };
  const char * const path = "/usr/lib64/genwqe/libz.so";
  //const char * const echo = "/opt/genwqe/bin/genwqe_echo -vvv -c1 -A ";
  const char * const echo = "/usr/bin/genwqe_echo -c1 -A ";
  //const char * const redir = "";
  const char * const redir = " 1>/dev/null 2>/dev/null";
  const int echo_len = strlen(echo);
  char buf[echo_len+64+1];
  int cardtype = -1;
  strcpy(buf, echo);

  // determine the card type
  int i;
  for (i=0; i<n_type; i++) {
    //int ret = system("/opt/genwqe/bin/genwqe_echo -c1 1>/dev/null 2>/dev/null");
    memcpy(&buf[echo_len], type_name[i], strlen(type_name[i]));
    memcpy(&buf[echo_len + strlen(type_name[i])], redir, strlen(redir));
    buf[echo_len + + strlen(type_name[i]) + strlen(redir)] = 0;
    int ret = system(buf);
    fprintf(stderr, "(I) %s (exit status %d)\n", buf, ret);
    if (0 == ret) {
      cardtype = type_arr[i];
      break;
    }
  }
  if (-1 == cardtype) {
    fprintf(stderr, "(I) %s could not find any supported card\n", path);
    rc = 1;
  }

  if (0 == rc) {
    // set the environment variables

    // https://github.com/ibm-genwqe/genwqe-user/wiki/Environment%20Variables
    // ZLIB_DEFLATE_IMPL:
    //  0x00: SW, 0x01: HW The last 4 bits are used to switch between the hard- and software-implementation. The upper bits are used to control some internal optimizations. Since some of those are only useful for certain use-cases, we did not enable them by default.
    //  0x20: Keep file-handles/worker threads to the card open/alive, even if no stream is in use anymore
    //  0x40: Useful for cases like Genomics, or filesystems where the dictionary is not used after the operation completed, e.g. Z_FULL_FLUSH, etc
    //  0x80: Use polling mode, only for CAPI
    if (0 != putenv("ZLIB_DEFLATE_IMPL=0x01")) {
      perror("(E) setenv() for ZLIB_DEFLATE_IMPL failed: ");
      rc = 1;
    }
    switch (cardtype) {
    case genwqe:
      if (0 != putenv("ZLIB_ACCELERATOR=GENWQE")) {
        perror("(E) setenv() for ZLIB_ACCELERATOR=GENWQE failed: ");
        rc = 1;
      }
      break;
    case capi:
      if (0 != putenv("ZLIB_ACCELERATOR=CAPI")) {
        perror("(E) setenv() for ZLIB_ACCELERATOR=CAPI failed: ");
        rc = 1;
      }
      break;
    }
  
    if (0 == rc) {
      void *handle = dlopen(path, RTLD_LAZY);
      if (NULL == handle) {
        fprintf(stderr, "(I) no %s found\n", path);
        rc = 1;
      } else {
        char *e;
        dlerror();
        api->p_deflateInit2_ = dlsym(handle, "deflateInit2_");
        if (NULL != (e = dlerror())) {
          fprintf(stderr, "(E) %s does not provide deflateInit2_: %s\n", path, e);
          rc = 1;
        }
        dlerror();
        api->p_deflate = dlsym(handle, "deflate");
        if (NULL != (e = dlerror())) {
          fprintf(stderr, "(E) %s does not provide deflate: %s\n", path, e);
          rc = 1;
        }
        dlerror();
        api->p_deflateReset = dlsym(handle, "deflateReset");
        if (NULL != (e = dlerror())) {
          fprintf(stderr, "(E) %s does not provide deflateReset: %s\n", path, e);
          rc = 1;
        }
      }
    }
  }
  if (0 == rc) {
    fprintf(stderr, "(I) IBM hardware zlib (%s) enabled\n", cardtype==capi ? "CAPI" : "GenWQE");
  }
  return rc;
}
