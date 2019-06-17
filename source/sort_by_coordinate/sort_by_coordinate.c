/*  sort_by_coordinate.c

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

#define _GNU_SOURCE
#include <memory.h>
#include <pthread.h>
#include <assert.h>
#include <limits.h>
#include "bam_sam2bam.h"

#define FILTER_NAME "sort_by_coordinate"

#define REGISTER_MEM_ID(id) id = (*api.md_register_mem_id)(#id)
static struct func_vector_v1 api;

const char *get_api_version(void) {
  return "1";
}

const char *get_filter_name(void) {
  return FILTER_NAME;
}

static void mem_init(){
}

const char *init_filter(struct func_vector_v1 *p_vec, const char *args) {
  api = *p_vec;
  mem_init();

  return "\33[32;1m" FILTER_NAME "\33[0m";
}

/*
 * empty function to force sort
 */
void *do_filter(void) {
  return NULL;
}
/*
long pre_filter(bam1_t *b) { }
void analyze_data(bam1_t *b, short *p_data, size_t size) { }
long post_filter(bam1_t *b) { }
*/

void *end_filter(void) {
  return NULL;
}

