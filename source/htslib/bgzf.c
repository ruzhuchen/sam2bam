/* The MIT License

   Copyright (c) 2008 Broad Institute / Massachusetts Institute of Technology
                 2011, 2012 Attractive Chaos <attractor@live.co.uk>
   Copyright (C) 2009, 2013, 2014 Genome Research Ltd

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
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/
#define ASYNC_FLUSH

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#define __USE_GNU
#include <pthread.h>
#include <sys/types.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dlfcn.h>
#if	defined(__powerpc64__) || defined(__x86_64__)
#define GET_N_PHYSICAL_CORES_FROM_OS
#define CPU_SET_AFFINITY
typedef struct {
  int use_nproc;
  int hts_proc_offset;
  int hts_nproc;
  int n_targets; // header->n_targets
  int create_bai;
  const char *fnout;
  int map[];
} cpu_map_t;
#define CPU_SET2(i, set)        {CPU_SET(mt->cpu_map->map[i], set);}

static struct timespec throttle;

static int n_physical_cores_valid = 0;
static int n_physical_cores;
//#define N_SAMPARSER_CORES()	(cpu_map->hts_proc_offset/SMT - 1/* core0 */)
//#define N_CORES()	(cpu_map->hts_nproc/SMT - 1/* core0 */)
//#define N_THREADS()	(mt->n_threads - (cpu_map->hts_proc_offset - SMT/* core0 */))
#if	defined(__powerpc64__)
//#define CPU_OFST()	(cpu_map->hts_proc_offset - SMT)
#define SMT		8
#define CPU_MAIN()	0
#define CPU_WRITE()	(n_physical_cores*SMT-1)
#define CPU_SYNC()	2
#define CPU_ZLIB_0(i)	(SMT + SMT*((i)%(N_SAMPARSER_CORES())) + (i)/(N_SAMPARSER_CORES()))
//#define CPU_HWZLIB(i)	(SMT + SMT*((mt->n_threads+(i))%(n_physical_cores-1)) + (mt->n_threads+(i))/(n_physical_cores-1))
#define CPU_ZLIB(i)	(SMT + SMT*(((i)/SMT)%(n_physical_cores-1)) + (i)%SMT)
#define CPU_HWZLIB(i)	CPU_ZLIB(mt->n_threads + i)
//#define CPU_HWZLIB(i)	(CPU_OFST() + SMT + SMT*((N_THREADS()+(i))%(N_CORES())) + (N_THREADS()+(i))/(N_CORES()-1))
#elif	defined(__x86_64__)
#define SMT		2
#define CPU_MAIN()	0
#define CPU_WRITE()	(n_physical_cores)
#define CPU_SYNC()	(n_physical_cores)
#define CPU_ZLIB(i)	(1 + ((i)/(n_physical_cores-1))*(n_physical_cores) + (i)%(n_physical_cores-1))
#endif
#endif
#if	defined(__powerpc64__)
#include <sys/platform/ppc.h>
#define HW_ZLIB
#define IN_SPIN_WAIT_LOOP()		__ppc_set_ppr_low()
#define FINISH_SPIN_WAIT_LOOP()		__ppc_set_ppr_med()
#define FINISH_SPIN_WAIT_LOOP2()	__ppc_set_ppr_med_low()
#elif	defined(__x86_64__)
#include <xmmintrin.h>		// required for _mm_pause()
#define IN_SPIN_WAIT_LOOP()	_mm_pause()
#define FINISH_SPIN_WAIT_LOOP()
#define FINISH_SPIN_WAIT_LOOP2()
#else
#define IN_FPIN_WAIT_LOOP()	nanosleep(&throttle, NULL)
#define FINISH_SPIN_WAIT_LOOP()
#define FINISH_SPIN_WAIT_LOOP2()
#endif

#include "htslib/hts.h"
#include "htslib/sam.h"
#include "htslib/bgzf.h"
#include "htslib/hfile.h"

#define BGZF_CACHE
#define BGZF_MT

#define BLOCK_HEADER_LENGTH 18
#define BLOCK_FOOTER_LENGTH 8


/* BGZF/GZIP header (speciallized from RFC 1952; little endian):
 +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 | 31|139|  8|  4|              0|  0|255|      6| 66| 67|      2|BLK_LEN|
 +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
  BGZF extension:
                ^                              ^   ^   ^
                |                              |   |   |
               FLG.EXTRA                     XLEN  B   C

  BGZF format is compatible with GZIP. It limits the size of each compressed
  block to 2^16 bytes and adds and an extra "BC" field in the gzip header which
  records the size.

*/
static const uint8_t g_magic[19] = "\037\213\010\4\0\0\0\0\0\377\6\0\102\103\2\0\0\0";

#ifdef BGZF_CACHE
typedef struct {
    int size;
    uint8_t *block;
    int64_t end_offset;
} cache_t;
#include "htslib/khash.h"
KHASH_MAP_INIT_INT64(cache, cache_t)
#endif

typedef struct
{
    uint64_t uaddr;  // offset w.r.t. uncompressed data
    uint64_t caddr;  // offset w.r.t. compressed data
}
bgzidx1_t;

struct __bgzidx_t
{
    int noffs, moffs;       // the size of the index, n:used, m:allocated
    bgzidx1_t *offs;        // offsets
    uint64_t ublock_addr;   // offset of the current block (uncompressed data)
};

void bgzf_index_destroy(BGZF *fp);
int bgzf_index_add_block(BGZF *fp);

static inline void packInt16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = value;
    buffer[1] = value >> 8;
}

static inline int unpackInt16(const uint8_t *buffer)
{
    return buffer[0] | buffer[1] << 8;
}

static inline void packInt32(uint8_t *buffer, uint32_t value)
{
    buffer[0] = value;
    buffer[1] = value >> 8;
    buffer[2] = value >> 16;
    buffer[3] = value >> 24;
}


static BGZF *bgzf_read_init(hFILE *hfpr)
{
    BGZF *fp;
    uint8_t magic[18];
    ssize_t n = hpeek(hfpr, magic, 18);
    if (n < 0) return NULL;

    fp = (BGZF*)calloc(1, sizeof(BGZF));
    if (fp == NULL) return NULL;

    fp->is_write = 0;
    fp->is_compressed = (n==2 && magic[0]==0x1f && magic[1]==0x8b);
    fp->uncompressed_block = malloc(BGZF_MAX_BLOCK_SIZE);
    fp->compressed_block = malloc(BGZF_MAX_BLOCK_SIZE);
    fp->is_compressed = (n==18 && magic[0]==0x1f && magic[1]==0x8b) ? 1 : 0;
    fp->is_gzip = ( !fp->is_compressed || ((magic[3]&4) && memcmp(&magic[12], "BC\2\0",4)==0) ) ? 0 : 1;
#ifdef BGZF_CACHE
    fp->cache = kh_init(cache);
#endif
    return fp;
}

#if defined(ASYNC_FLUSH)
typedef struct {
  memcpy_info_t mi;
  bam1_t *bam;
} memcpy_info_bam_t;
#endif
// get the compress level from the mode string: compress_level==-1 for the default level, -2 plain uncompressed
static int mode2level(const char *__restrict mode)
{
    int i, compress_level = -1;
    for (i = 0; mode[i]; ++i)
        if (mode[i] >= '0' && mode[i] <= '9') break;
    if (mode[i]) compress_level = (int)mode[i] - '0';
    if (strchr(mode, 'u')) compress_level = -2;
    return compress_level;
}
static BGZF *bgzf_write_init(const char *mode)
{
    BGZF *fp;
    fp = (BGZF*)calloc(1, sizeof(BGZF));
    fp->is_write = 1;
    int compress_level = mode2level(mode);
    if ( compress_level==-2 )
    {
        fp->is_compressed = 0;
        return fp;
    }
    fp->is_compressed = 1;
    fp->uncompressed_block = malloc(BGZF_MAX_BLOCK_SIZE);
#if defined(ASYNC_FLUSH)
    memcpy_info_array_t *mia = (memcpy_info_array_t*)fp->uncompressed_block;
    mia->last = mia->info;
#endif
    fp->compressed_block = malloc(BGZF_MAX_BLOCK_SIZE);
    fp->compress_level = compress_level < 0? Z_DEFAULT_COMPRESSION : compress_level; // Z_DEFAULT_COMPRESSION==-1
    if (fp->compress_level > 9) fp->compress_level = Z_DEFAULT_COMPRESSION;
    if ( strchr(mode,'g') )
    {
        // gzip output
        fp->is_gzip = 1;
        fp->gz_stream = (z_stream*)calloc(1,sizeof(z_stream));
        fp->gz_stream->zalloc = NULL;
        fp->gz_stream->zfree  = NULL;
        if ( deflateInit2(fp->gz_stream, fp->compress_level, Z_DEFLATED, 15|16, 8, Z_DEFAULT_STRATEGY)!=Z_OK ) return NULL;
    }
    return fp;
}

BGZF *bgzf_open(const char *path, const char *mode)
{
    BGZF *fp = 0;
    assert(compressBound(BGZF_BLOCK_SIZE) < BGZF_MAX_BLOCK_SIZE);
    if (strchr(mode, 'r')) {
        hFILE *fpr;
        if ((fpr = hopen(path, mode)) == 0) return 0;
        fp = bgzf_read_init(fpr);
        if (fp == 0) { hclose_abruptly(fpr); return NULL; }
        fp->fp = fpr;
    } else if (strchr(mode, 'w') || strchr(mode, 'a')) {
        hFILE *fpw;
        if ((fpw = hopen(path, mode)) == 0) return 0;
        fp = bgzf_write_init(mode);
        fp->fp = fpw;
    }
    else { errno = EINVAL; return 0; }

    fp->is_be = ed_is_big();
    return fp;
}

BGZF *bgzf_dopen(int fd, const char *mode)
{
    BGZF *fp = 0;
    assert(compressBound(BGZF_BLOCK_SIZE) < BGZF_MAX_BLOCK_SIZE);
    if (strchr(mode, 'r')) {
        hFILE *fpr;
        if ((fpr = hdopen(fd, mode)) == 0) return 0;
        fp = bgzf_read_init(fpr);
        if (fp == 0) { hclose_abruptly(fpr); return NULL; } // FIXME this closes fd
        fp->fp = fpr;
    } else if (strchr(mode, 'w') || strchr(mode, 'a')) {
        hFILE *fpw;
        if ((fpw = hdopen(fd, mode)) == 0) return 0;
        fp = bgzf_write_init(mode);
        fp->fp = fpw;
    }
    else { errno = EINVAL; return 0; }

    fp->is_be = ed_is_big();
    return fp;
}

BGZF *bgzf_hopen(hFILE *hfp, const char *mode)
{
    BGZF *fp = NULL;
    assert(compressBound(BGZF_BLOCK_SIZE) < BGZF_MAX_BLOCK_SIZE);
    if (strchr(mode, 'r')) {
        fp = bgzf_read_init(hfp);
        if (fp == NULL) return NULL;
    } else if (strchr(mode, 'w') || strchr(mode, 'a')) {
        fp = bgzf_write_init(mode);
    }
    else { errno = EINVAL; return 0; }

    fp->fp = hfp;
    fp->is_be = ed_is_big();
    return fp;
}

#if defined(HW_ZLIB)
static hw_zlib_api_t hw_zlib_api;
  
static int bgzf_compress_hw(z_stream *p_zs, void *_dst, int *dlen, void *src, int slen)
{
    uint32_t crc;
    uint8_t *dst = (uint8_t*)_dst;

    // compress the body
    p_zs->next_in  = (Bytef*)src;
    p_zs->avail_in = slen;
    p_zs->next_out = dst + BLOCK_HEADER_LENGTH;
    p_zs->avail_out = *dlen - BLOCK_HEADER_LENGTH - BLOCK_FOOTER_LENGTH;
    int rc;
    rc = (*hw_zlib_api.p_deflate)(p_zs, Z_FINISH);
    *dlen = p_zs->total_out + BLOCK_HEADER_LENGTH + BLOCK_FOOTER_LENGTH;
    if(rc != Z_STREAM_END) { fprintf(stderr, "deflate[%p] error rc %d (slen=%d dlen=%d)\n", p_zs, rc, slen, *dlen); return -1;}
    // write the header
    memcpy(dst, g_magic, BLOCK_HEADER_LENGTH); // the last two bytes are a place holder for the length of the block
    packInt16(&dst[16], *dlen - 1); // write the compressed length; -1 to fit 2 bytes
    // write the footer
    crc = crc32(crc32(0L, NULL, 0L), (Bytef*)src, slen);
    packInt32((uint8_t*)&dst[*dlen - 8], crc);
    packInt32((uint8_t*)&dst[*dlen - 4], slen);
    rc = (*hw_zlib_api.p_deflateReset)(p_zs);
    if(rc != Z_OK) {fprintf(stderr, "deflateReset[%p] error rc %d\n", p_zs, rc); return -1;}
    return 0;
}
#endif
static int bgzf_compress2(z_stream *p_zs, void *_dst, int *dlen, void *src, int slen)
{
    uint32_t crc;
    uint8_t *dst = (uint8_t*)_dst;

    // compress the body
    p_zs->next_in  = (Bytef*)src;
    p_zs->avail_in = slen;
    p_zs->next_out = dst + BLOCK_HEADER_LENGTH;
    p_zs->avail_out = *dlen - BLOCK_HEADER_LENGTH - BLOCK_FOOTER_LENGTH;
    int rc;
    rc = deflate(p_zs, Z_FINISH);
    *dlen = p_zs->total_out + BLOCK_HEADER_LENGTH + BLOCK_FOOTER_LENGTH;
    if(rc != Z_STREAM_END) { fprintf(stderr, "bgzf_compress2: deflate[%p] error rc %d (slen=%d dlen=%d)\n", p_zs, rc, slen, *dlen); return -1;}
    // write the header
    memcpy(dst, g_magic, BLOCK_HEADER_LENGTH); // the last two bytes are a place holder for the length of the block
    packInt16(&dst[16], *dlen - 1); // write the compressed length; -1 to fit 2 bytes
    // write the footer
    crc = crc32(crc32(0L, NULL, 0L), (Bytef*)src, slen);
    packInt32((uint8_t*)&dst[*dlen - 8], crc);
    packInt32((uint8_t*)&dst[*dlen - 4], slen);
    rc = deflateReset(p_zs);
    if(rc != Z_OK) {fprintf(stderr, "bgzf_compress2: deflateReset[%p] error rc %d\n", p_zs, rc); return -1;}
    return 0;
}
static int bgzf_compress(void *_dst, int *dlen, void *src, int slen, int level)
{
    uint32_t crc;
    z_stream zs;
    uint8_t *dst = (uint8_t*)_dst;

    // compress the body
    zs.zalloc = NULL; zs.zfree = NULL;
    zs.next_in  = (Bytef*)src;
    zs.avail_in = slen;
    zs.next_out = dst + BLOCK_HEADER_LENGTH;
    zs.avail_out = *dlen - BLOCK_HEADER_LENGTH - BLOCK_FOOTER_LENGTH;
    if (deflateInit2(&zs, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return -1; // -15 to disable zlib header/footer
    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) return -1;
    if (deflateEnd(&zs) != Z_OK) return -1;
    *dlen = zs.total_out + BLOCK_HEADER_LENGTH + BLOCK_FOOTER_LENGTH;
    // write the header
    memcpy(dst, g_magic, BLOCK_HEADER_LENGTH); // the last two bytes are a place holder for the length of the block
    packInt16(&dst[16], *dlen - 1); // write the compressed length; -1 to fit 2 bytes
    // write the footer
    crc = crc32(crc32(0L, NULL, 0L), (Bytef*)src, slen);
    packInt32((uint8_t*)&dst[*dlen - 8], crc);
    packInt32((uint8_t*)&dst[*dlen - 4], slen);
    return 0;
}

static int bgzf_gzip_compress(BGZF *fp, void *_dst, int *dlen, void *src, int slen, int level)
{
    uint8_t *dst = (uint8_t*)_dst;
    z_stream *zs = fp->gz_stream;
    int flush = slen ? Z_NO_FLUSH : Z_FINISH;
    zs->next_in   = (Bytef*)src;
    zs->avail_in  = slen;
    zs->next_out  = dst;
    zs->avail_out = *dlen;
    if ( deflate(zs, flush) == Z_STREAM_ERROR ) return -1;
    *dlen = *dlen - zs->avail_out;
    return 0;
}

// Deflate the block in fp->uncompressed_block into fp->compressed_block. Also adds an extra field that stores the compressed block length.
static int deflate_block(BGZF *fp, int block_length)
{
    int comp_size = BGZF_MAX_BLOCK_SIZE;
    int ret;
    if ( !fp->is_gzip )
        ret = bgzf_compress(fp->compressed_block, &comp_size, fp->uncompressed_block, block_length, fp->compress_level);
    else
        ret = bgzf_gzip_compress(fp, fp->compressed_block, &comp_size, fp->uncompressed_block, block_length, fp->compress_level);

    if ( ret != 0 )
    {
        fp->errcode |= BGZF_ERR_ZLIB;
        return -1;
    }
    fp->block_offset = 0;

    return comp_size;
}

// Inflate the block in fp->compressed_block into fp->uncompressed_block
static int inflate_block(BGZF* fp, int block_length)
{
    z_stream zs;
    zs.zalloc = NULL;
    zs.zfree = NULL;
    zs.next_in = (Bytef*)fp->compressed_block + 18;
    zs.avail_in = block_length - 16;
    zs.next_out = (Bytef*)fp->uncompressed_block;
    zs.avail_out = BGZF_MAX_BLOCK_SIZE;

    if (inflateInit2(&zs, -15) != Z_OK) {
        fp->errcode |= BGZF_ERR_ZLIB;
        return -1;
    }
    if (inflate(&zs, Z_FINISH) != Z_STREAM_END) {
        inflateEnd(&zs);
        fp->errcode |= BGZF_ERR_ZLIB;
        return -1;
    }
    if (inflateEnd(&zs) != Z_OK) {
        fp->errcode |= BGZF_ERR_ZLIB;
        return -1;
    }
    return zs.total_out;
}

static int inflate_gzip_block(BGZF *fp, int cached)
{
    int ret = Z_OK;
    do
    {
        if ( !cached && fp->gz_stream->avail_out!=0 )
        {
            fp->gz_stream->avail_in = hread(fp->fp, fp->compressed_block, BGZF_BLOCK_SIZE);
            if ( fp->gz_stream->avail_in<=0 ) return fp->gz_stream->avail_in;
            if ( fp->gz_stream->avail_in==0 ) break;
            fp->gz_stream->next_in = fp->compressed_block;
        }
        else cached = 0;
        do
        {
            fp->gz_stream->next_out = (Bytef*)fp->uncompressed_block + fp->block_offset;
            fp->gz_stream->avail_out = BGZF_MAX_BLOCK_SIZE - fp->block_offset;
            ret = inflate(fp->gz_stream, Z_NO_FLUSH);
            if ( ret==Z_BUF_ERROR ) continue;   // non-critical error
            if ( ret<0 ) return -1;
            unsigned int have = BGZF_MAX_BLOCK_SIZE - fp->gz_stream->avail_out;
            if ( have ) return have;
        }
        while ( fp->gz_stream->avail_out == 0 );
    }
    while (ret != Z_STREAM_END);
    return BGZF_MAX_BLOCK_SIZE - fp->gz_stream->avail_out;
}

// Returns: 0 on success (BGZF header); -1 on non-BGZF GZIP header; -2 on error
static int check_header(const uint8_t *header)
{
    if ( header[0] != 31 || header[1] != 139 || header[2] != 8 ) return -2;
    return ((header[3] & 4) != 0
            && unpackInt16((uint8_t*)&header[10]) == 6
            && header[12] == 'B' && header[13] == 'C'
            && unpackInt16((uint8_t*)&header[14]) == 2) ? 0 : -1;
}

#ifdef BGZF_CACHE
static void free_cache(BGZF *fp)
{
    khint_t k;
    khash_t(cache) *h = (khash_t(cache)*)fp->cache;
    if (fp->is_write) return;
    for (k = kh_begin(h); k < kh_end(h); ++k)
        if (kh_exist(h, k)) free(kh_val(h, k).block);
    kh_destroy(cache, h);
}

static int load_block_from_cache(BGZF *fp, int64_t block_address)
{
    khint_t k;
    cache_t *p;
    khash_t(cache) *h = (khash_t(cache)*)fp->cache;
    k = kh_get(cache, h, block_address);
    if (k == kh_end(h)) return 0;
    p = &kh_val(h, k);
    if (fp->block_length != 0) fp->block_offset = 0;
    fp->block_address = block_address;
    fp->block_length = p->size;
    memcpy(fp->uncompressed_block, p->block, BGZF_MAX_BLOCK_SIZE);
    if ( hseek(fp->fp, p->end_offset, SEEK_SET) < 0 )
    {
        // todo: move the error up
        fprintf(stderr,"Could not hseek to %"PRId64"\n", p->end_offset);
        exit(1);
    }
    return p->size;
}

static void cache_block(BGZF *fp, int size)
{
    int ret;
    khint_t k;
    cache_t *p;
    khash_t(cache) *h = (khash_t(cache)*)fp->cache;
    if (BGZF_MAX_BLOCK_SIZE >= fp->cache_size) return;
    if ((kh_size(h) + 1) * BGZF_MAX_BLOCK_SIZE > (uint32_t)fp->cache_size) {
        /* A better way would be to remove the oldest block in the
         * cache, but here we remove a random one for simplicity. This
         * should not have a big impact on performance. */
        for (k = kh_begin(h); k < kh_end(h); ++k)
            if (kh_exist(h, k)) break;
        if (k < kh_end(h)) {
            free(kh_val(h, k).block);
            kh_del(cache, h, k);
        }
    }
    k = kh_put(cache, h, fp->block_address, &ret);
    if (ret == 0) return; // if this happens, a bug!
    p = &kh_val(h, k);
    p->size = fp->block_length;
    p->end_offset = fp->block_address + size;
    p->block = (uint8_t*)malloc(BGZF_MAX_BLOCK_SIZE);
    memcpy(kh_val(h, k).block, fp->uncompressed_block, BGZF_MAX_BLOCK_SIZE);
}
#else
static void free_cache(BGZF *fp) {}
static int load_block_from_cache(BGZF *fp, int64_t block_address) {return 0;}
static void cache_block(BGZF *fp, int size) {}
#endif

int bgzf_read_block(BGZF *fp)
{
    uint8_t header[BLOCK_HEADER_LENGTH], *compressed_block;
    int count, size = 0, block_length, remaining;

    // Reading an uncompressed file
    if ( !fp->is_compressed )
    {
        count = hread(fp->fp, fp->uncompressed_block, BGZF_MAX_BLOCK_SIZE);
        if ( count==0 )
        {
            fp->block_length = 0;
            return 0;
        }
        if (fp->block_length != 0) fp->block_offset = 0;
        fp->block_address += count;
        fp->block_length = count;
        return 0;
    }

    // Reading compressed file
    int64_t block_address;
    block_address = htell(fp->fp);
    if ( fp->is_gzip && fp->gz_stream ) // is this is a initialized gzip stream?
    {
        count = inflate_gzip_block(fp, 0);
        if ( count<0 )
        {
            fp->errcode |= BGZF_ERR_ZLIB;
            return -1;
        }
        fp->block_length = count;
        fp->block_address = block_address;
        return 0;
    }
    if (fp->cache_size && load_block_from_cache(fp, block_address)) return 0;
    count = hread(fp->fp, header, sizeof(header));
    if (count == 0) { // no data read
        fp->block_length = 0;
        return 0;
    }
    int ret;
    if ( count != sizeof(header) || (ret=check_header(header))==-2 )
    {
        fp->errcode |= BGZF_ERR_HEADER;
        return -1;
    }
    if ( ret==-1 )
    {
        // GZIP, not BGZF
        uint8_t *cblock = (uint8_t*)fp->compressed_block;
        memcpy(cblock, header, sizeof(header));
        count = hread(fp->fp, cblock+sizeof(header), BGZF_BLOCK_SIZE - sizeof(header)) + sizeof(header);
        int nskip = 10;

        // Check optional fields to skip: FLG.FNAME,FLG.FCOMMENT,FLG.FHCRC,FLG.FEXTRA
        // Note: Some of these fields are untested, I did not have appropriate data available
        if ( header[3] & 0x4 ) // FLG.FEXTRA
        {
            nskip += unpackInt16(&cblock[nskip]) + 2;
        }
        if ( header[3] & 0x8 ) // FLG.FNAME
        {
            while ( nskip<count && cblock[nskip] ) nskip++;
            nskip++;
        }
        if ( header[3] & 0x10 ) // FLG.FCOMMENT
        {
            while ( nskip<count && cblock[nskip] ) nskip++;
            nskip++;
        }
        if ( header[3] & 0x2 ) nskip += 2;  //  FLG.FHCRC

        /* FIXME: Should handle this better.  There's no reason why
           someone shouldn't include a massively long comment in their
           gzip stream. */
        if ( nskip >= count )
        {
            fp->errcode |= BGZF_ERR_HEADER;
            return -1;
        }

        fp->is_gzip = 1;
        fp->gz_stream = (z_stream*) calloc(1,sizeof(z_stream));
        int ret = inflateInit2(fp->gz_stream, -15);
        if (ret != Z_OK)
        {
            fp->errcode |= BGZF_ERR_ZLIB;
            return -1;
        }
        fp->gz_stream->avail_in = count - nskip;
        fp->gz_stream->next_in  = cblock + nskip;
        count = inflate_gzip_block(fp, 1);
        if ( count<0 )
        {
            fp->errcode |= BGZF_ERR_ZLIB;
            return -1;
        }
        fp->block_length = count;
        fp->block_address = block_address;
        if ( fp->idx_build_otf ) return -1; // cannot build index for gzip
        return 0;
    }
    size = count;
    block_length = unpackInt16((uint8_t*)&header[16]) + 1; // +1 because when writing this number, we used "-1"
    compressed_block = (uint8_t*)fp->compressed_block;
    memcpy(compressed_block, header, BLOCK_HEADER_LENGTH);
    remaining = block_length - BLOCK_HEADER_LENGTH;
    count = hread(fp->fp, &compressed_block[BLOCK_HEADER_LENGTH], remaining);
    if (count != remaining) {
        fp->errcode |= BGZF_ERR_IO;
        return -1;
    }
    size += count;
    if ((count = inflate_block(fp, block_length)) < 0) return -1;
    if (fp->block_length != 0) fp->block_offset = 0; // Do not reset offset if this read follows a seek.
    fp->block_address = block_address;
    fp->block_length = count;
    if ( fp->idx_build_otf )
    {
        bgzf_index_add_block(fp);
        fp->idx->ublock_addr += count;
    }
    cache_block(fp, size);
    return 0;
}

ssize_t bgzf_read(BGZF *fp, void *data, size_t length)
{
    ssize_t bytes_read = 0;
    uint8_t *output = (uint8_t*)data;
    if (length <= 0) return 0;
    assert(fp->is_write == 0);
    while (bytes_read < length) {
        int copy_length, available = fp->block_length - fp->block_offset;
        uint8_t *buffer;
        if (available <= 0) {
            if (bgzf_read_block(fp) != 0) return -1;
            available = fp->block_length - fp->block_offset;
            if (available <= 0) break;
        }
        copy_length = length - bytes_read < available? length - bytes_read : available;
        buffer = (uint8_t*)fp->uncompressed_block;
        memcpy(output, buffer + fp->block_offset, copy_length);
        fp->block_offset += copy_length;
        output += copy_length;
        bytes_read += copy_length;
    }
    if (fp->block_offset == fp->block_length) {
        fp->block_address = htell(fp->fp);
        fp->block_offset = fp->block_length = 0;
    }
    fp->uncompressed_address += bytes_read;
    return bytes_read;
}

ssize_t bgzf_raw_read(BGZF *fp, void *data, size_t length)
{
    return hread(fp->fp, data, length);
}

#ifdef BGZF_MT

typedef struct {
    struct bgzf_mtaux_t *mt;
    void *buf;
    int i, errcode, toproc, compress_level;
    z_stream zs;
#if defined(ASYNC_FLUSH)
    pthread_t tid;
//    void * bam_buf;
    int64_t job_count;
//    int enabled;
    struct timespec time_rt;
    struct timespec time_th;
    void *bam_space_cache;
#endif
} worker_t;

#if defined(HW_ZLIB)
typedef worker_t hw_worker_t; 
#endif

typedef struct bgzf_mtaux_t {
    int n_threads, n_blks;
    volatile int done;
    volatile int proc_cnt;
    void **blk;
#if !defined(ASYNC_FLUSH)
    int *len;
#endif
    worker_t *w;
#if !defined(ASYNC_FLUSH)
    pthread_t *tid;
#endif
    pthread_mutex_t lock;
    pthread_cond_t cv;
#if defined(ASYNC_FLUSH)
#if defined(HW_ZLIB)
    int n_hw_threads;
    hw_worker_t *hw_w;
#endif
    void * bam_space_id;
    memcpy_info_array_t **memcpy_info_array;
    volatile int *len;
    pthread_t writer_tid;
    worker_t writer;
    volatile int writer_done;
    pthread_t synchronizer_tid;
    worker_t synchronizer;
    volatile int64_t write_id, deflate_id, enqueue_id;
    BGZF *fp;
    int n_active_threads;
    int auto_tune_n_active_threads;
    char *path;
    cpu_map_t *cpu_map;
    size_t compressed_bytes;
    struct {
      long count;
      double time;
      volatile int done;
    } sync;
#else
    int curr;
#endif
} mtaux_t;

#if defined(ASYNC_FLUSH)
extern void * get_bam_space(int n_threads);
extern void remove_bam_space();
extern void bam_copy(void **p_bam_space_cache, void *bam_space_id, memcpy_info_array_t *mia, char *blk, int len, int bigendian, int myid);

#if defined(HW_ZLIB)
static int worker_aux_hw(hw_worker_t *w)
{
  mtaux_t *mt = w->mt;

  w->errcode = 0;

    FINISH_SPIN_WAIT_LOOP2();
    int64_t dflid, qid;
    while(1) {
        //if (mt->done) return 1; // to quit the thread
//      if (w->enabled) {
	// ensure that mt->deflate_id is loaded and then mt->enqueue_id is loaded on an out-of-order execution processor 
	// otherwise, thread A can load deflate_id and then enqueue_id, and thread B can load enqueue_id and then deflate_id.
	// suppose that:
	// thread A			thread B
	// reads 98 for enqueue_id 	reads 98 for deflate_id
	// 				reads 99 for enqueue_id 
	//				updates deflate_id from 98 to 99
	// reads 99 for deflate_id
	// updates deflate_id 99 to 100						<-- at this moment, enqueue_id=99, deflate_id=100
        __atomic_load(&mt->deflate_id, &dflid, __ATOMIC_ACQUIRE);
        qid = mt->enqueue_id;
        if (dflid < 0 || dflid == qid) {
            if (mt->done) return 1; // to quit the thread
            //w->idle_count++;
	    IN_SPIN_WAIT_LOOP();
            continue;
        }
	FINISH_SPIN_WAIT_LOOP();
        if (__sync_bool_compare_and_swap(&mt->deflate_id,dflid,dflid+1)) {
            w->job_count++;
            break;
        }
//      } else {
//        if (mt->done) return 1; // to quit the thread
//        usleep(100*1000);
//      }
    }
    FINISH_SPIN_WAIT_LOOP2();

    // obtained a new job
    int i = dflid % mt->n_blks;
    int clen = BGZF_MAX_BLOCK_SIZE;
    size_t compressed_bytes = 0;

    /* memcpy */
    bam_copy(&w->bam_space_cache, w->mt->bam_space_id, (memcpy_info_array_t*)mt->memcpy_info_array[i], (char*)mt->blk[i], mt->len[i], mt->fp->is_be, w-w->mt->w);

    compressed_bytes += mt->len[i];
    if (bgzf_compress_hw(&w->zs, w->buf, &clen, mt->blk[i], mt->len[i]) != 0){
      w->errcode |= BGZF_ERR_ZLIB;
fprintf(stderr, "worker_aux2 error enq %ld dfl %ld wrt %ld\n", qid, dflid, mt->write_id);
exit(-1);
    }
    memcpy(mt->blk[i], w->buf, clen);
__sync_synchronize();
    mt->len[i] = - clen; /* sign flag shows that a block is deflated */

    __sync_fetch_and_add(&mt->compressed_bytes, compressed_bytes);

    return 0;
}
#endif

static int worker_aux2(worker_t *w)
{
  mtaux_t *mt = w->mt;

  w->errcode = 0;
#if 0
  if (w->i == 1) {
    static int warmup_done = 0;
    if (mt->auto_tune_n_active_threads == 0) {
      int i;
      warmup_done = 1; 
        worker_t * top = w - w->i; 
	for(i=mt->n_active_threads+1; i<mt->n_threads; i++){ 
	  top[i].enabled = 1;
	}
        mt->n_active_threads = mt->n_threads - 1;
      }
      if (!warmup_done) {
#if 1
	static uint64_t enq0,dfl0; 
	static int wupx = 0; 
	switch(wupx){
	case 0: 
	  enq0 = mt->enqueue_id;
	  dfl0 = mt->deflate_id;
	  wupx ++;
	  break;
	case 1: {
          if (0 == ((1+ w->job_count) & 0x01f)) {
	    uint64_t enq_inc = mt->enqueue_id - enq0;
	    uint64_t dfl_inc = mt->deflate_id - dfl0;
	    unsigned int thd_inc = (mt->n_threads-2 < enq_inc/dfl_inc-1 ? mt->n_threads-2 : enq_inc/dfl_inc-1); /* thd0/1 already active */
	    int i;
            worker_t * top = w - w->i;
	    for(i=0; i<thd_inc; i++){
              top[mt->n_active_threads].enabled = 1;
              mt->n_active_threads ++;
	    }
            fprintf(stderr, "#thr %3d +#enqueue %ld +#deflate %ld +#threads %d\n", mt->n_active_threads, enq_inc, dfl_inc, thd_inc);
	    warmup_done = 1;
	  }
	} /* case 1 */
      } /* switch */
#else
{{{
      if (0 == ((1+ w->job_count) & 0x01f)) {
	static int retry = 10;
        struct timespec time_th, time_rt;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time_th);
        clock_gettime(CLOCK_REALTIME, &time_rt);
        int64_t rt = time_rt.tv_sec*1000000000 + time_rt.tv_nsec - (w->time_rt.tv_sec*1000000000 + w->time_rt.tv_nsec);
        int64_t th = time_th.tv_sec*1000000000 + time_th.tv_nsec - (w->time_th.tv_sec*1000000000 + w->time_th.tv_nsec);
        double util = th*1.0/rt;
        w->time_rt = time_rt; w->time_th = time_th;

        //double util = ((double)w->job_count)/(w->idle_count+w->job_count);
        //w->idle_count = w->job_count = 0;
        if (util > 0.99 && mt->n_active_threads*10 < mt->enqueue_id - mt->deflate_id && mt->n_active_threads < mt->n_threads) {
            worker_t * top = w - w->i;
            top[mt->n_active_threads].enabled = 1;
            mt->n_active_threads ++;
            fprintf(stderr, "#thr %3d util %.1f %% enqueueId %ld deflateId %ld\n", mt->n_active_threads, util*100, mt->enqueue_id, mt->deflate_id);
	    retry = 10;
        } else {
	    retry--;
	    if(retry == 0){
	      warmup_done = 1;
	    }
        }
      }
}}}
#endif
     } else {
#if 0
{{{
      if (0 == ((1+ w->job_count) & 0x5ff)) {
        struct timespec time_th, time_rt;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time_th);
        clock_gettime(CLOCK_REALTIME, &time_rt);
        int64_t rt = time_rt.tv_sec*1000000000 + time_rt.tv_nsec - (w->time_rt.tv_sec*1000000000 + w->time_rt.tv_nsec);
        int64_t th = time_th.tv_sec*1000000000 + time_th.tv_nsec - (w->time_th.tv_sec*1000000000 + w->time_th.tv_nsec);
        double util = th*1.0/rt;
        w->time_rt = time_rt; w->time_th = time_th;
        fprintf(stderr, "#thr %3d util %.1f %% deflate req %ld req being deflated %ld\n", mt->n_active_threads, util*100, mt->deflate_id - mt->enqueue_id, mt->deflate_id - mt->write_id);
      }
}}}
#endif
     }
    }

    FINISH_SPIN_WAIT_LOOP2();
#endif
    int64_t dflid, qid;
//#define DEFLATE_BLOCK_COUNT 64 // 1
    int dflt_blk_count;
    while(1) {
        //if (mt->done) return 1; // to quit the thread
//      if (w->enabled) {
	// ensure that mt->deflate_id is loaded and then mt->enqueue_id is loaded on an out-of-order execution processor 
	// otherwise, thread A can load deflate_id and then enqueue_id, and thread B can load enqueue_id and then deflate_id.
	// suppose that:
	// thread A			thread B
	// reads 98 for enqueue_id 	reads 98 for deflate_id
	// 				reads 99 for enqueue_id 
	//				updates deflate_id from 98 to 99
	// reads 99 for deflate_id
	// updates deflate_id 99 to 100						<-- at this moment, enqueue_id=99, deflate_id=100
        __atomic_load(&mt->deflate_id, &dflid, __ATOMIC_ACQUIRE);
        qid = mt->enqueue_id;
	dflt_blk_count = BGZF_BLOCK_COUNT_PER_WORKER;
        if (dflid < 0) {
	  IN_SPIN_WAIT_LOOP();
          continue;
	} else if (qid < dflid +dflt_blk_count) {
	  dflt_blk_count = 1;
	  if (qid < dflid +dflt_blk_count) {
            if (mt->done) {
	      return 1; // to quit the thread
	    }
            //w->idle_count++;
	    IN_SPIN_WAIT_LOOP();
            continue;
	  }
        }
	FINISH_SPIN_WAIT_LOOP2();
        if (__sync_bool_compare_and_swap(&mt->deflate_id,dflid,dflid +dflt_blk_count)) {
            w->job_count+=dflt_blk_count;
            break;
        }
//      } else {
//        if (mt->done) return 1; // to quit the thread
//        usleep(100*1000);
//      }
    }

    // obtained a new job
    int const i0 = dflid % mt->n_blks;

    int i, start[2], end[2];
    int const last_i = i0 + dflt_blk_count; 
    if (last_i <= mt->n_blks) {
      start[0] = i0;
      end[0] = last_i;
      start[1] = 0;
      end[1] = 0;
    } else {
      start[0] = i0;
      end[0] = mt->n_blks;
      start[1] = 0;
      end[1] = dflt_blk_count - (mt->n_blks - i0);
    }
    int j;
    size_t compressed_bytes = 0;
    for (j=0; j < 2; j++) {
//fprintf(stderr, "worker_aux2[%d] j %d start %d - end %d deflate[%p]\n", w->i, j, start[j], end[j], &w->zs);
      for (i=start[j]; i < end[j]; i++){
//fprintf(stderr, "worker_aux2[%d] i %d deflate[%p]\n", w->i, i, &w->zs);
        /* memcpy */
        bam_copy(&w->bam_space_cache, w->mt->bam_space_id, (memcpy_info_array_t*)mt->memcpy_info_array[i], (char*)mt->blk[i], mt->len[i], mt->fp->is_be, w-w->mt->w);

        int clen = BGZF_MAX_BLOCK_SIZE;
	compressed_bytes += mt->len[i];
        if (bgzf_compress2(&w->zs, w->buf, &clen, mt->blk[i], mt->len[i]) != 0){
          w->errcode |= BGZF_ERR_ZLIB;
          fprintf(stderr, "worker_aux2 error enq %ld dfl %ld wrt %ld\n", qid, dflid, mt->write_id);
          exit(-1);
        }
        memcpy(mt->blk[i], w->buf, clen);
__sync_synchronize();
        mt->len[i] = - clen; /* sign flag shows that a block is deflated */
      }
    }
    __sync_fetch_and_add(&mt->compressed_bytes, compressed_bytes);

    return 0;
}
#else
static int worker_aux(worker_t *w)
{
    int i, stop = 0;
    // wait for condition: to process or all done
    pthread_mutex_lock(&w->mt->lock);
    while (!w->toproc && !w->mt->done)
        pthread_cond_wait(&w->mt->cv, &w->mt->lock);
    if (w->mt->done) stop = 1;
    w->toproc = 0;
    pthread_mutex_unlock(&w->mt->lock);
    if (stop) return 1; // to quit the thread
    w->errcode = 0;
    for (i = w->i; i < w->mt->curr; i += w->mt->n_threads) {
        int clen = BGZF_MAX_BLOCK_SIZE;
        if (bgzf_compress2(&w->zs, w->buf, &clen, w->mt->blk[i], w->mt->len[i]) != 0)
            w->errcode |= BGZF_ERR_ZLIB;
        memcpy(w->mt->blk[i], w->buf, clen);
        w->mt->len[i] = clen;
    }
    __sync_fetch_and_add(&w->mt->proc_cnt, 1);
    return 0;
}
#endif

static void *mt_worker(void *data)
{
#if defined(ASYNC_FLUSH)
    while (worker_aux2((worker_t*)data) == 0);
#else
    while (worker_aux((worker_t*)data) == 0);
#endif
    return 0;
}

#if defined(ASYNC_FLUSH)
#if defined(HW_ZLIB)
static void *mt_hw_zlib(void *data)
{
    while (worker_aux_hw((hw_worker_t*)data) == 0);
    return 0;
}
#endif
#endif

#define USE_SYNC_FILE_RANGE
#if defined(ASYNC_FLUSH)
static void *mt_synchronizer(void *data)
{
  mtaux_t *mt = ((worker_t*)data)->mt;

  struct timeval time_begin, time_end, time_fsync, time_last;
  long count = 0;
  const long interval = 100*1000; /* 100 ms */
  time_last.tv_sec = time_last.tv_usec = 0;

  gettimeofday(&time_begin, 0);
  while(1) {
    if (mt->writer_done) {
	gettimeofday(&time_end, 0);
	double t = time_end.tv_sec-time_begin.tv_sec + (time_end.tv_usec-time_begin.tv_usec)*1.0/1000/1000;
	mt->sync.count = count;
	mt->sync.time = t;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	mt->sync.done = 1;
        //fprintf(stderr, "** synchronizer done (%ld sync %.2f fsync/sec)\n", count, count/t);
	return 0;
    }

    count ++;
    {
#if defined(USE_SYNC_FILE_RANGE)
      static off_t last = 0;
      int fd = ((hFILE_fd*)(mt->fp->fp))->fd;
      off_t const curr = lseek(fd, 0, SEEK_CUR);
      sync_file_range(fd, last, curr - last, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE);
      sync_file_range(fd, last, curr - last, SYNC_FILE_RANGE_WAIT_BEFORE);
      posix_fadvise(fd, last, curr - last, POSIX_FADV_DONTNEED);
      last = curr;
#else
      // -> sam_open -> hts_open -> hts_open_format -> hopen      -> hopen_fd -> open
      // <-----------<-----------<- htsFile fp      <-------------<- hFILE &fp->base (hFILE_fd fp)
      //                            fp->fn=path                      fp->fd=
      //                            fp->fp.bgzf->fp = hFILE
      const int fd = open(mt->path, O_RDONLY);
      if (fd == -1) {
	fprintf(stderr, "(W) open(%s) failed\n", mt->path);
	return 0;
      }
      fsync(fd);
      close(fd);
#endif
    }
    gettimeofday(&time_fsync, 0);
    long t = (time_fsync.tv_sec-time_last.tv_sec)*1000*1000 + (time_fsync.tv_usec-time_last.tv_usec);
    time_last = time_fsync;
    t = interval - t;
    if (t > 0) {
      usleep(t);
    }
  }

  return 0;
}

static void *mt_writer2(void *data)
{
  mtaux_t *mt = ((worker_t*)data)->mt;

  // .bai init
  hts_idx_t * bai = NULL;
  if (mt->cpu_map->create_bai) {
    off_t const offset0_H = (htell(mt->fp->fp)<<16);
    uint16_t const offset0_L = 0;
    bai = hts_idx_init(mt->cpu_map->n_targets, HTS_FMT_BAI, offset0_H|offset0_L, 14, 5);
  }

  while(1) {
    int64_t wr = mt->write_id;
    int64_t de = mt->deflate_id;
    // the writer must be enabled and not catch up the deflaters
    if(wr < 0 || wr == de) {
      if (mt->done && de == mt->enqueue_id) {
	    mt->writer_done = 1;
            goto mt_writer2_done; // to quit the thread
      }
      IN_SPIN_WAIT_LOOP();
      continue;
    }
    FINISH_SPIN_WAIT_LOOP();

    int i = wr % mt->n_blks;
    int len;
    while( (len = mt->len[i]) > 0) {
      /* deflate is not completed */
      IN_SPIN_WAIT_LOOP();
    }
    FINISH_SPIN_WAIT_LOOP();
    // a negative len means that a block at i was compressed
    len = - len;

    // .bai push bam records except the last one
    memcpy_info_t *mi_last = NULL;
    if (mt->cpu_map->create_bai) {
      memcpy_info_array_t * const mia = mt->memcpy_info_array[i];
      memcpy_info_t *mi = mia->info;
      off_t const offset_H = (htell(mt->fp->fp)<<16);
      while ( 1 ) {
        memcpy_info_t *mi_next = (memcpy_info_t*)((char*)mi + sizeof(memcpy_info_ptr_t));
        uint16_t const offset_L = ((mi->block_offset + mi->len)&0xFFFF);
        if (!mi_next->is_end) {
          int ret = hts_idx_push(bai, mi->tid, mi->pos, mi->endpos, offset_H|offset_L, (0 == mi->unmap));
          if (ret < 0) {
            fprintf(stderr, "(E) hts_idx_push failed\n");
            exit(-1);
          }
        } else {
          mi_last = mi;
          break;
        }
        mi = mi_next;
      }
    }

    if (hwrite(mt->fp->fp, mt->blk[i], len) != len) {
      mt->fp->errcode |= BGZF_ERR_IO;
      break;
    }
    // .bai push the last bam record
    if (mt->cpu_map->create_bai) {
      off_t const offset_H = (htell(mt->fp->fp)<<16);
      uint16_t const offset_L = 0; // ((mi->block_offset + mi->len)&0xFFFF);
      int ret = hts_idx_push(bai, mi_last->tid, mi_last->pos, mi_last->endpos, offset_H|offset_L, (0 == mi_last->unmap));
      if (ret < 0) {
        fprintf(stderr, "(E) hts_idx_push failed\n");
        exit(-1);
      }
    }

    mt->write_id = wr + 1;
  }

mt_writer2_done:
  // .bai finish
  if (mt->cpu_map->create_bai) {
    hts_idx_finish(bai, htell(mt->fp->fp));
    int ret = hts_idx_save(bai, mt->cpu_map->fnout, HTS_FMT_BAI);
    if (ret < 0) {
      fprintf(stderr, "(E) hts_idx_save failed\n");
      exit(-1);
    }
    hts_idx_destroy(bai);
  }
  return 0;
}
#endif

#if defined(ASYNC_FLUSH)
static mtaux_t *mt_;
void bgzf_progress(int *to_be_deflated, int *being_deflated, size_t *compressed_bytes,
	int *writer_done, int *sync_done, long *sync_count, double *sync_time) {
  *to_be_deflated = mt_->enqueue_id - mt_->deflate_id; 
  *being_deflated = mt_->deflate_id - mt_->write_id;
  *compressed_bytes = mt_->compressed_bytes;
  *writer_done = mt_->writer_done;

  *sync_done = mt_->sync.done;
  *sync_count= mt_->sync.count;
  *sync_time = mt_->sync.time;
}
#endif

static long hw_zlib = -1;
int bgzf_init_accelerator(void) {
#if defined(HW_ZLIB)

  // open the directory <path of executable>/filter.d/ to load filters under it
    char buf[256];
    ssize_t sz = readlink( "/proc/self/exe", buf, sizeof(buf)-1 );
    if (-1 == sz) {
      fprintf(stderr, "could not get the path of this program\n");
      exit(-1);
    }
    if (sz == sizeof(buf)-1) {
      fprintf(stderr, "too long path of this program\n");
      exit(-1);
    }
    buf[sz] = 0;
    rindex(buf, (int)'/')[1] = 0; // remove the program name
    if (strlen(buf)+strlen("accelerator.d/")+1 > sizeof(buf)) {
      fprintf(stderr, "too long path of this program\n");
      exit(-1);
    }
    strcat(buf, "accelerator.d/libz_hw.so");

    void *handle = dlopen(buf, RTLD_LAZY);
    if (NULL == handle) {
      fprintf(stderr, "(I) no %s found\n", buf);
    } else {
      dlerror();
      char *e;
      int (*p_init_hw_zlib)(hw_zlib_api_t *);
      p_init_hw_zlib = dlsym(handle, "init_hw_zlib");
      if (NULL != (e = dlerror())) {
        fprintf(stderr, "(E) %s does not provide init_hw_zlib: %s\n", buf, e);
	exit(-1);
      } else {
	long err = (*p_init_hw_zlib)(&hw_zlib_api);
	if (err) {
          fprintf(stderr, "(W) failed to setup hw zlib functions for %s\n", buf);
	} else {
          fprintf(stderr, "(I) obtained hw zlib functions for %s\n", buf);
	  hw_zlib = 0;
        }
      }
    }
#endif
  return 0;
}

static void bgzf_mt_create(mtaux_t *mt, const char *name, worker_t *w, int cpu, pthread_attr_t *attr, uint64_t *cpu_mask, const int cpu_vec_len, int disable_cpuaffinity, uint64_t *cpu_vec) {
  pthread_t * const tid = &w->tid;
  if (0 != (cpu_mask[cpu/64]&(1LL<<(63-cpu%64)))) {
#if defined(ASYNC_FLUSH)
      pthread_create(tid, attr, mt_worker, w);
#if defined(CPU_SET_AFFINITY)
      if (n_physical_cores_valid) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
//fprintf(stderr, "CPU_ZLIB(%d)=%d\n", i, CPU_ZLIB(i));
        CPU_SET2(cpu, &cpuset);
        cpu_vec[cpu/64] |= 1LL<<(63-cpu%64);
        if (!disable_cpuaffinity) pthread_setaffinity_np(*tid, sizeof(cpu_set_t), &cpuset);
        char str[32];
        sprintf(str, "%s-%d-%d", name, (int)(w-w->mt->w), cpu);
        if(pthread_setname_np(*tid, str)){}
      } else {
#endif
      if(pthread_setname_np(*tid, name)){}
#if defined(CPU_SET_AFFINITY)
      }
#endif
#else
      pthread_create(tid, &attr, mt_worker, w);
#endif
  }
}

static mtaux_t *mt = NULL;
static int disable_cpuaffinity = 0;

int bgzf_mt_3(BGZF *fp, int n_sub_blks, char *fn, void *_cpu_map)
{
    cpu_map_t * const cpu_map = (cpu_map_t *)_cpu_map;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    const int cpu_vec_len = (n_physical_cores*SMT+63)/64;
    uint64_t cpu_vec[cpu_vec_len];
    uint64_t cpu_mask[cpu_vec_len];
    memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
    memset(cpu_mask, 0, sizeof(uint64_t)*cpu_vec_len);
    int i;
    for (i=SMT; i < cpu_map->hts_proc_offset; i++) {
      cpu_mask[i/64] |= 1LL<<(63-i%64);
    }
    for (i=0; i < cpu_map->hts_proc_offset-SMT/* core0 */; i++) {
      bgzf_mt_create(mt, "zlib", &mt->w[i], CPU_ZLIB(i), &attr, cpu_mask, cpu_vec_len, disable_cpuaffinity, cpu_vec);
    }
    fprintf(stderr, "(I) CPU %15s ", "zlib");
    for(i=0; i<cpu_vec_len; i++) {
      int j;
      for(j=0; j<64; j++) { fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-'); }
    }
    fprintf(stderr, "\n");

    return 0;
}

int bgzf_mt(BGZF *fp, int n_threads, int n_sub_blks, char *fn, void *_cpu_map)
{
    int i;
    cpu_map_t *const cpu_map = (cpu_map_t*)_cpu_map;
    pthread_attr_t attr;
#if defined(ASYNC_FLUSH)
    if (!fp->is_write || fp->mt) return -1;
#else
    if (!fp->is_write || fp->mt || n_threads <= 1) return -1;
#endif
    mt = (mtaux_t*)calloc(1, sizeof(mtaux_t));
#if defined(ASYNC_FLUSH)
{
    char *no_cpuaffinity_str = getenv("NO_CPUAFFINITY");
    if (no_cpuaffinity_str) {
      disable_cpuaffinity = 1; 
    }
    throttle.tv_sec = 0;
    throttle.tv_nsec = 500;
#if defined(__x86_64__)
    disable_cpuaffinity = 1; 
/*
  If each thread (e.g., gzip) is bound to a specific cpu on rhel7.1, it suffers from the cost of page faults.
           llist_add_batch		[k] 31%
           smp_call_function_many	[k] 17%
           native_flush_tlb_others	[k]
           flush_tlb_page		[k]
           ptep_clear_flush		[k]
           try_to_unmap_one		[k]
           try_to_unmap_anon		[k]
           try_to_unmap			[k]
           migrate_pages		[k]
           migrate_misplaced_page	[k]
           do_numa_page			[k]
           handle_mm_fault		[k]
           __do_page_fault		[k]
           do_page_fault		[k]
           page_fault			[k]
           bam_picard_markduplicates_core_ext
           bam_picard_markduplicates
*/
#endif
    fprintf(stderr, "(I) setaffinity in the gzip-and-write stage %s%s\n",
	disable_cpuaffinity ? "disabled" : "enabled",
#if defined(__x86_64__)
	" (always in x86_64)"
#else
	""
#endif
	);
}
#endif
#if defined(HW_ZLIB)
{
    if (-1 != hw_zlib) {
          char *str = getenv("HW_ZLIB"), *endp;
	  if (NULL == str) {
            fprintf(stderr, "(W) use HW_ZLIB=<n> to enable accelerator\n");
	  } else {
            int n = strtol(str, &endp, 10);
            if (*endp == 0) {
              fprintf(stderr, "(I) hw_zlib %d\n", n);
              hw_zlib = n;
              mt->hw_w = calloc(hw_zlib, sizeof(mt->hw_w[0]));
            } else {
              fprintf(stderr, "(W) parameter for HW_ZLIB is not a number\n");
	    }
	  }
    }
}
#endif
#if defined(ASYNC_FLUSH)
    mt_ = mt;
    mt->fp = fp;
    mt->path = strdup(fn);
    mt->deflate_id = mt->write_id = -1;
    mt->n_active_threads = 1; // 0 and 1
    mt->cpu_map = (cpu_map_t*)cpu_map;
#if defined(GET_N_PHYSICAL_CORES_FROM_OS)
    n_physical_cores_valid = 0;
    {
      int n = mt->cpu_map->use_nproc;
      if (n > 0) {
#if defined(__powerpc64__)
        n_physical_cores = n/8; // 0 origin
#elif defined(__x86_64__)
        n_physical_cores = n/2; // 0 origin
#endif
        n_physical_cores_valid = 1;
      }
    }
#endif
#endif
    n_threads = cpu_map->use_nproc - SMT /* core0 is dedicated to main & sync threads */ - 1 /* the last thread on coreN-1 is dedicated to write thread */
    		- (-1 == hw_zlib ? 0 : hw_zlib);
    mt->n_threads = n_threads;
    mt->n_blks = (n_threads + hw_zlib) * n_sub_blks;
    mt->len = (int*)calloc(mt->n_blks, sizeof(int));
    mt->blk = (void**)calloc(mt->n_blks, sizeof(void*));
#if defined(ASYNC_FLUSH)
    mt->bam_space_id = get_bam_space(n_threads);
    mt->memcpy_info_array = (memcpy_info_array_t**)calloc(mt->n_blks, sizeof(void*));
    memcpy_info_array_t * mia = (memcpy_info_array_t*)fp->uncompressed_block;
    mia->last = mia->info;
#endif
    for (i = 0; i < mt->n_blks; ++i) {
        mt->blk[i] = malloc(BGZF_MAX_BLOCK_SIZE);
#if defined(ASYNC_FLUSH)
        memcpy_info_array_t * const mia = (memcpy_info_array_t*)malloc(BGZF_MAX_BLOCK_SIZE);
	mt->memcpy_info_array[i] = mia;
#endif
    }
#if !defined(ASYNC_FLUSH)
    mt->tid = (pthread_t*)calloc(mt->n_threads, sizeof(pthread_t)); // tid[0] is not used, as the worker 0 is launched by the master
#endif
    mt->w = (worker_t*)calloc(mt->n_threads, sizeof(worker_t));
    for (i = 0; i < mt->n_threads; ++i) {
        mt->w[i].i = i;
        mt->w[i].mt = mt;
        mt->w[i].compress_level = fp->compress_level;
        mt->w[i].buf = malloc(BGZF_MAX_BLOCK_SIZE);
#if defined(ASYNC_FLUSH)
        {
	  int rc = deflateInit2(&(mt->w[i].zs), fp->compress_level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
          if (rc != Z_OK) {
	    fprintf(stderr, "deflateInit2 error rc %d\n", rc);
	    return -1; // -15 to disable zlib header/footer
	  }
	}
	//mt->w[i].bam_buf = NULL;
	mt->w[i].bam_space_cache = NULL;
        //mt->w[i].idle_count = 0;
	mt->w[i].job_count = 0;
	clock_gettime(CLOCK_REALTIME, &mt->w[i].time_rt);
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &mt->w[i].time_th);
#endif
    }
#if defined(ASYNC_FLUSH)
//    mt->w[0].enabled = 1;
//    mt->w[1].enabled = 1;
#endif
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_mutex_init(&mt->lock, 0);
    pthread_cond_init(&mt->cv, 0);
#if defined(CPU_SET_AFFINITY)
if (!disable_cpuaffinity) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET2(CPU_MAIN(), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
#endif
    const int cpu_vec_len = (n_physical_cores*SMT+63)/64;
    uint64_t cpu_vec[cpu_vec_len];
    uint64_t cpu_mask[cpu_vec_len];
    memset(cpu_mask, 0, sizeof(uint64_t)*cpu_vec_len);
    for (i=cpu_map->hts_proc_offset; i<cpu_map->use_nproc; i++) {
      cpu_mask[i/64] |= 1LL<<(63-i%64);
    }

    memset(cpu_vec, 0, sizeof(cpu_vec[0])*cpu_vec_len);
    for (i=(cpu_map->hts_proc_offset ? cpu_map->hts_proc_offset-SMT/* core0 */ : 0); i<mt->n_threads; i++) {
      bgzf_mt_create(mt, "zlib", &mt->w[i], CPU_ZLIB(i), &attr, cpu_mask, cpu_vec_len, disable_cpuaffinity, cpu_vec);
    }
    fprintf(stderr, "(I) CPU %15s ", "zlib");
    for(i=0; i<cpu_vec_len; i++) {
      int j;
      for(j=0; j<64; j++) { fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-'); }
    }
    fprintf(stderr, "\n");

#if defined(HW_ZLIB)
{
    mt->n_hw_threads = hw_zlib;
    memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
    for (i = 0; i < hw_zlib; ++i) { // worker 0 is effectively launched by the master thread
        mt->hw_w[i].i = i;
        mt->hw_w[i].mt = mt;
//        mt->hw_w[i].enabled = 1;
        mt->hw_w[i].compress_level = fp->compress_level;
        mt->hw_w[i].buf = malloc(BGZF_MAX_BLOCK_SIZE);
        {
	  int rc = (*hw_zlib_api.p_deflateInit2_)(&(mt->hw_w[i].zs), fp->compress_level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, (int)sizeof(z_stream));
          if (rc != Z_OK) {
	    fprintf(stderr, "HW deflateInit2 error rc %d\n", rc);
	    return -1; // -15 to disable zlib header/footer
	  }
	}
	//mt->w[i].bam_space_id = get_bam_space();
        //mt->w[i].idle_count = 0;
	mt->hw_w[i].job_count = 0;
	clock_gettime(CLOCK_REALTIME, &mt->hw_w[i].time_rt);
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &mt->hw_w[i].time_th);
        pthread_create(&mt->hw_w[i].tid, &attr, mt_hw_zlib, &mt->hw_w[i]);
#if defined(CPU_SET_AFFINITY)
if (n_physical_cores_valid) {
    int c = CPU_HWZLIB(i);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
//fprintf(stderr, "CPU_HWZLIB(%d)=%d\n", i, CPU_HWZLIB(i));
    CPU_SET2(c, &cpuset);
    cpu_vec[CPU_HWZLIB(i)/64] |= 1LL<<(63-CPU_HWZLIB(i)%64);
    if (!disable_cpuaffinity) pthread_setaffinity_np(mt->hw_w[i].tid, sizeof(cpu_set_t), &cpuset);
    char str[16];
    sprintf(str, "hw_zlib-%d-%d", i, c);
    if(pthread_setname_np(mt->hw_w[i].tid, str)){}
} else
#endif
	if(pthread_setname_np(mt->hw_w[i].tid, "hw_zlib")){}
    }
  fprintf(stderr, "(I) CPU %15s ", "hw_zlib");
  for(i=0; i<cpu_vec_len; i++) {
    int j;
    for(j=0; j<64; j++) {
      fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
    }
  }
  fprintf(stderr, "\n");
}
#endif
#if defined(ASYNC_FLUSH)
    int disable_fsync = 0;
{
    char *no_fsync_str = getenv("NO_FSYNC");
    if (no_fsync_str) {
      disable_fsync = 1; 
    }
}
    mt->writer.mt = mt;
    pthread_create(&mt->writer_tid, &attr, mt_writer2, &mt->writer);
#if defined(CPU_SET_AFFINITY)
if (!disable_cpuaffinity) {
    memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET2(CPU_WRITE(), &cpuset);
    cpu_vec[CPU_WRITE()/64] |= 1LL<<(63-CPU_WRITE()%64);
    pthread_setaffinity_np(mt->writer_tid, sizeof(cpu_set_t), &cpuset);
    fprintf(stderr, "(I) CPU %15s ", "<<writer>>");
    for(i=0; i<cpu_vec_len; i++) {
      int j;
      for(j=0; j<64; j++) {
        fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
      }
    }
    fprintf(stderr, "\n");
}
#endif
    if(pthread_setname_np(mt->writer_tid, "<<writer>>")){}

if (!disable_fsync) {
    mt->synchronizer.mt = mt;
    pthread_create(&mt->synchronizer_tid, &attr, mt_synchronizer, &mt->synchronizer);
#if defined(CPU_SET_AFFINITY)
if (!disable_cpuaffinity) {
    memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET2(CPU_SYNC(), &cpuset);
    cpu_vec[CPU_SYNC()/64] |= 1LL<<(63-CPU_SYNC()%64);
    pthread_setaffinity_np(mt->synchronizer_tid, sizeof(cpu_set_t), &cpuset);
    fprintf(stderr, "(I) CPU %15s ", "<<sync>>");
    for(i=0; i<cpu_vec_len; i++) {
      int j;
      for(j=0; j<64; j++) {
        fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
      }
    }
    fprintf(stderr, "\n");
}
#endif
    if(pthread_setname_np(mt->synchronizer_tid, "<<sync>>")){}
} else {
    fprintf(stderr, "(I) background fsync disabled\n");
}
    mt->auto_tune_n_active_threads = 0;
    char *auto_thr = getenv("PERF_AUTO_THREADS");
    if (auto_thr) {
	  mt->auto_tune_n_active_threads = 1;
    }
#endif
    fp->mt = mt;
    return 0;
}

static void mt_destroy(mtaux_t *mt)
{
    int i;
    // signal all workers to quit
    pthread_mutex_lock(&mt->lock);
    mt->done = 1; mt->proc_cnt = 0;
    pthread_cond_broadcast(&mt->cv);
    pthread_mutex_unlock(&mt->lock);
#if defined(ASYNC_FLUSH)
    for (i = 0; i < mt->n_threads; ++i) pthread_join(mt->w[i].tid, 0);
#if defined(HW_ZLIB)
    for (i = 0; i < mt->n_hw_threads; ++i) pthread_join(mt->hw_w[i].tid, 0); 
#endif
    pthread_join(mt->writer_tid, 0);
    if (mt->synchronizer.mt) {
int i;
for(i=0;i<1000;i++) if(hflush(mt->fp->fp)){};
      pthread_join(mt->synchronizer_tid, 0);
    }
#else
    for (i = 1; i < mt->n_threads; ++i) pthread_join(mt->tid[i], 0); // worker 0 is effectively launched by the master thread
#endif
    // free other data allocated on heap
    for (i = 0; i < mt->n_blks; ++i) free(mt->blk[i]);
    for (i = 0; i < mt->n_threads; ++i) free(mt->w[i].buf);
#if defined(HW_ZLIB)
    for (i = 0; i < mt->n_hw_threads; ++i) free(mt->hw_w[i].buf);
    free(mt->hw_w);
#endif
    free(mt->blk); free((void*)mt->len); free(mt->w); 
#if !defined(ASYNC_FLUSH)
    free(mt->tid);
#endif
    pthread_cond_destroy(&mt->cv);
    pthread_mutex_destroy(&mt->lock);
    free(mt);
    remove_bam_space();
}

#if defined(ASYNC_FLUSH)
static void mt_queue2(BGZF *fp)
{
    mtaux_t *mt = fp->mt;
    int64_t qid;
    while(((qid = mt->enqueue_id) +1) % mt->n_blks == mt->write_id % mt->n_blks) {
      IN_SPIN_WAIT_LOOP();
    }
    FINISH_SPIN_WAIT_LOOP();

    int const i = ( qid ) % mt->n_blks;

    // copy memcpy_info array so that mt workers can memcpy
    memcpy_info_array_t * const mia = (memcpy_info_array_t*)fp->uncompressed_block;
    int const copylen = (char*)mia->last - (char*)mia;
    memcpy(mt->memcpy_info_array[i], mia, copylen);
    mt->memcpy_info_array[i]->last = (memcpy_info_t*)((char*)mt->memcpy_info_array[i] + copylen);
    mt->memcpy_info_array[i]->last->is_end = 1;
    mt->len[i] = fp->block_offset;
    mia->last = &mia->info[0];
    fp->block_offset = 0;

__sync_synchronize();
    mt->enqueue_id = qid+1;
    if(mt->write_id == -1) {
__sync_synchronize();
      mt->deflate_id = mt->write_id = 0;
    }
}
#else
static void mt_queue(BGZF *fp)
{
    mtaux_t *mt = fp->mt;
    assert(mt->curr < mt->n_blks); // guaranteed by the caller
    memcpy(mt->blk[mt->curr], fp->uncompressed_block, fp->block_offset);
    mt->len[mt->curr] = fp->block_offset;
    fp->block_offset = 0;
    ++mt->curr;
}
#endif

#if defined(ASYNC_FLUSH)
static int mt_flush_queue2(BGZF *fp)
{
    mtaux_t *mt = fp->mt;

    while(mt->write_id < mt->enqueue_id) {
      IN_SPIN_WAIT_LOOP();
    }
    FINISH_SPIN_WAIT_LOOP();

    return 0;
}
#else
static int mt_flush_queue(BGZF *fp)
{
    int i;
    mtaux_t *mt = fp->mt;
    // signal all the workers to compress
    pthread_mutex_lock(&mt->lock);
    for (i = 0; i < mt->n_threads; ++i) mt->w[i].toproc = 1;
    mt->proc_cnt = 0;
    pthread_cond_broadcast(&mt->cv);
    pthread_mutex_unlock(&mt->lock);
    // worker 0 is doing things here
    worker_aux(&mt->w[0]);
    // wait for all the threads to complete
    while (mt->proc_cnt < mt->n_threads);
    // dump data to disk
    for (i = 0; i < mt->n_threads; ++i) fp->errcode |= mt->w[i].errcode;
    for (i = 0; i < mt->curr; ++i)
        if (hwrite(fp->fp, mt->blk[i], mt->len[i]) != mt->len[i]) {
            fp->errcode |= BGZF_ERR_IO;
            break;
        }
    mt->curr = 0;
    return (fp->errcode == 0)? 0 : -1;
}
#endif

static int lazy_flush(BGZF *fp)
{
    if (fp->mt) {
#if defined(ASYNC_FLUSH)
	if (fp->block_offset) mt_queue2(fp);
	return 0;
#else
        if (fp->block_offset) mt_queue(fp);
        return (fp->mt->curr < fp->mt->n_blks)? 0 : mt_flush_queue(fp);
#endif
    }
    else return bgzf_flush(fp);
}

#else  // ~ #ifdef BGZF_MT

int bgzf_mt(BGZF *fp, int n_threads, int n_sub_blks)
{
    return 0;
}

static inline int lazy_flush(BGZF *fp)
{
    return bgzf_flush(fp);
}

#endif // ~ #ifdef BGZF_MT

int bgzf_flush(BGZF *fp)
{
    if (!fp->is_write) return 0;
#ifdef BGZF_MT
    if (fp->mt) {
#if defined(ASYNC_FLUSH)
        if (fp->block_offset) mt_queue2(fp); // guaranteed that assertion does not fail
        return mt_flush_queue2(fp);
#else
        if (fp->block_offset) mt_queue(fp); // guaranteed that assertion does not fail
        return mt_flush_queue(fp);
#endif
    }
#endif
    while (fp->block_offset > 0) {
        if ( fp->idx_build_otf )
        {
            bgzf_index_add_block(fp);
            fp->idx->ublock_addr += fp->block_offset;
        }
        int block_length = deflate_block(fp, fp->block_offset);
        if (block_length < 0) return -1;
        if (hwrite(fp->fp, fp->compressed_block, block_length) != block_length) {
            fp->errcode |= BGZF_ERR_IO; // possibly truncated file
            return -1;
        }
        fp->block_address += block_length;
    }
    return 0;
}

int bgzf_flush_try(BGZF *fp, ssize_t size)
{
    if (fp->block_offset + size > BGZF_BLOCK_SIZE) return lazy_flush(fp);
    return 0;
}

ssize_t bgzf_write(BGZF *fp, const void *data, size_t length)
{
    if ( !fp->is_compressed )
        return hwrite(fp->fp, data, length);

    const uint8_t *input = (const uint8_t*)data;
    ssize_t remaining = length;
    assert(fp->is_write);
    while (remaining > 0) {
        uint8_t* buffer = (uint8_t*)fp->uncompressed_block;
        int copy_length = BGZF_BLOCK_SIZE - fp->block_offset;
        if (copy_length > remaining) copy_length = remaining;
        memcpy(buffer + fp->block_offset, input, copy_length);
        fp->block_offset += copy_length;
        input += copy_length;
        remaining -= copy_length;
        if (fp->block_offset == BGZF_BLOCK_SIZE) {
            if (lazy_flush(fp) != 0) return -1;
        }
    }
    return length - remaining;
}
#if defined(ASYNC_FLUSH)
inline ssize_t bgzf_write3(BGZF *fp, bam1_t *b)
{
    if ( !fp->is_compressed )
/* TODO
        return hwrite(fp->fp, data, block_len_plus_4);
*/
        abort();

    memcpy_info_array_t * const mia = (memcpy_info_array_t*)fp->uncompressed_block;

    const uint8_t *input = (const uint8_t*)b;
    ssize_t remaining = 4 + 32 + b->l_data;
    assert(fp->is_write);
    while (remaining > 0) {
        int copy_length = BGZF_BLOCK_SIZE - fp->block_offset;
        if (copy_length > remaining) copy_length = remaining;

	memcpy_info_t * const mi = mia->last;
	mi->is_end = 0;
	//mi->dup = (dup != 0);
	mi->block_offset = fp->block_offset;
	mi->len = copy_length;
	//mi->is_ptr = (is_bam == 0);
	int l;
	memcpy_info_ptr_t * const mip = (memcpy_info_ptr_t*)mia->last;
	bam_vaddr_t bamva;
	bam_vaddr_init2(&bamva, b);
	mip->ptr = bamva;
	l = sizeof(memcpy_info_ptr_t);
	mia->last = (memcpy_info_t*)((char*)mia->last + l);
        fp->block_offset += copy_length;
        //bam_vaddr_add_offset(&input, copy_length);
	input += copy_length;
        remaining -= copy_length;
        if (fp->block_offset == BGZF_BLOCK_SIZE) {
	    mia->last->is_end = 1;
            if (lazy_flush(fp) != 0) return -1;
        }
    }
    return 4 + 32 + b->l_data - remaining;
}
inline ssize_t bgzf_write2(BGZF *fp, const bam_vaddr_t bamva, size_t block_len_plus_4, int dup)
{
    if ( !fp->is_compressed )
/* TODO
        return hwrite(fp->fp, data, block_len_plus_4);
*/
        abort();

    memcpy_info_array_t * const mia = (memcpy_info_array_t*)fp->uncompressed_block;

    bam_vaddr_t input = bamva;
    ssize_t remaining = block_len_plus_4;
    assert(fp->is_write);
    while (remaining > 0) {
        int copy_length = BGZF_BLOCK_SIZE - fp->block_offset;
        if (copy_length > remaining) copy_length = remaining;

	memcpy_info_t * const mi = mia->last;
	mi->is_end = 0;
	mi->dup = (dup != 0);
	mi->block_offset = fp->block_offset;
	mi->len = copy_length;
	//mi->is_ptr = (is_bam == 0);
	int l;
	memcpy_info_ptr_t * const mip = (memcpy_info_ptr_t*)mia->last;
	mip->ptr = (bam_vaddr_t)input;
	l = sizeof(memcpy_info_ptr_t);
	mia->last = (memcpy_info_t*)((char*)mia->last + l);
        fp->block_offset += copy_length;
        bam_vaddr_add_offset(&input, copy_length);
        remaining -= copy_length;
        if (fp->block_offset == BGZF_BLOCK_SIZE) {
	    mia->last->is_end = 1;
            if (lazy_flush(fp) != 0) return -1;
        }
    }
    return block_len_plus_4 - remaining;
}

int bam_write3(BGZF *fp, bam1_t *b)
{
    int ok = (bgzf_flush_try(fp, 4 + 32 + b->l_data) >= 0);
    if (ok) ok = (bgzf_write3(fp, b) >= 0);
    return ok? 4 + 32 + b->l_data : -1;
}
int bam_write2(BGZF *fp, const bam_vaddr_t bamva, uint32_t l_data /* == b->l_data */, int mark_dup) // avoid touching *b
{
    int ok = (bgzf_flush_try(fp, 4 + 32 + l_data) >= 0);
    if (ok) ok = (bgzf_write2(fp, bamva, 4 + 32 + l_data, mark_dup) >= 0);
    return ok? 4 + 32 + l_data : -1;
}
#endif

ssize_t bgzf_raw_write(BGZF *fp, const void *data, size_t length)
{
    return hwrite(fp->fp, data, length);
}

int bgzf_close(BGZF* fp)
{
    int ret, block_length;
    if (fp == 0) return -1;
    if (fp->is_write && fp->is_compressed) {
        if (bgzf_flush(fp) != 0) return -1;
        fp->compress_level = -1;
        block_length = deflate_block(fp, 0); // write an empty block
        if (hwrite(fp->fp, fp->compressed_block, block_length) < 0
            || hflush(fp->fp) != 0) {
            fp->errcode |= BGZF_ERR_IO;
            return -1;
        }
#ifdef BGZF_MT
        if (fp->mt) mt_destroy(fp->mt);
#endif
    }
    if ( fp->is_gzip )
    {
        if (!fp->is_write) (void)inflateEnd(fp->gz_stream);
        else (void)deflateEnd(fp->gz_stream);
        free(fp->gz_stream);
    }
    ret = hclose(fp->fp);
    if (ret != 0) return -1;
    bgzf_index_destroy(fp);
    free(fp->uncompressed_block);
    free(fp->compressed_block);
    free_cache(fp);
    free(fp);
    return 0;
}

void bgzf_set_cache_size(BGZF *fp, int cache_size)
{
    if (fp) fp->cache_size = cache_size;
}

int bgzf_check_EOF(BGZF *fp)
{
    uint8_t buf[28];
    off_t offset = htell(fp->fp);
    if (hseek(fp->fp, -28, SEEK_END) < 0) {
        if (errno == ESPIPE) { hclearerr(fp->fp); return 2; }
        else return -1;
    }
    if ( hread(fp->fp, buf, 28) != 28 ) return -1;
    if ( hseek(fp->fp, offset, SEEK_SET) < 0 ) return -1;
    return (memcmp("\037\213\010\4\0\0\0\0\0\377\6\0\102\103\2\0\033\0\3\0\0\0\0\0\0\0\0\0", buf, 28) == 0)? 1 : 0;
}

int64_t bgzf_seek(BGZF* fp, int64_t pos, int where)
{
    int block_offset;
    int64_t block_address;

    if (fp->is_write || where != SEEK_SET) {
        fp->errcode |= BGZF_ERR_MISUSE;
        return -1;
    }
    block_offset = pos & 0xFFFF;
    block_address = pos >> 16;
    if (hseek(fp->fp, block_address, SEEK_SET) < 0) {
        fp->errcode |= BGZF_ERR_IO;
        return -1;
    }
    fp->block_length = 0;  // indicates current block has not been loaded
    fp->block_address = block_address;
    fp->block_offset = block_offset;
    return 0;
}

int bgzf_is_bgzf(const char *fn)
{
    uint8_t buf[16];
    int n;
    hFILE *fp;
    if ((fp = hopen(fn, "r")) == 0) return 0;
    n = hread(fp, buf, 16);
    if ( hclose(fp) < 0 ) return -1;
    if (n != 16) return 0;
    return memcmp(g_magic, buf, 16) == 0? 1 : 0;
}

int bgzf_getc(BGZF *fp)
{
    int c;
    if (fp->block_offset >= fp->block_length) {
        if (bgzf_read_block(fp) != 0) return -2; /* error */
        if (fp->block_length == 0) return -1; /* end-of-file */
    }
    c = ((unsigned char*)fp->uncompressed_block)[fp->block_offset++];
    if (fp->block_offset == fp->block_length) {
        fp->block_address = htell(fp->fp);
        fp->block_offset = 0;
        fp->block_length = 0;
    }
    fp->uncompressed_address++;
    return c;
}

#ifndef kroundup32
#define kroundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))
#endif

int bgzf_getline(BGZF *fp, int delim, kstring_t *str)
{
    int l, state = 0;
    unsigned char *buf = (unsigned char*)fp->uncompressed_block;
    str->l = 0;
    do {
        if (fp->block_offset >= fp->block_length) {
            if (bgzf_read_block(fp) != 0) { state = -2; break; }
            if (fp->block_length == 0) { state = -1; break; }
        }
        for (l = fp->block_offset; l < fp->block_length && buf[l] != delim; ++l);
        if (l < fp->block_length) state = 1;
        l -= fp->block_offset;
        if (str->l + l + 1 >= str->m) {
            str->m = str->l + l + 2;
            kroundup32(str->m);
            str->s = (char*)realloc(str->s, str->m);
        }
        memcpy(str->s + str->l, buf + fp->block_offset, l);
        str->l += l;
        fp->block_offset += l + 1;
        if (fp->block_offset >= fp->block_length) {
            fp->block_address = htell(fp->fp);
            fp->block_offset = 0;
            fp->block_length = 0;
        }
    } while (state == 0);
    if (str->l == 0 && state < 0) return state;
    fp->uncompressed_address += str->l;
    if ( delim=='\n' && str->l>0 && str->s[str->l-1]=='\r' ) str->l--;
    str->s[str->l] = 0;
    return str->l;
}

void bgzf_index_destroy(BGZF *fp)
{
    if ( !fp->idx ) return;
    free(fp->idx->offs);
    free(fp->idx);
    fp->idx = NULL;
    fp->idx_build_otf = 0;
}

int bgzf_index_build_init(BGZF *fp)
{
    bgzf_index_destroy(fp);
    fp->idx = (bgzidx_t*) calloc(1,sizeof(bgzidx_t));
    if ( !fp->idx ) return -1;
    fp->idx_build_otf = 1;  // build index on the fly
    return 0;
}

int bgzf_index_add_block(BGZF *fp)
{
    fp->idx->noffs++;
    if ( fp->idx->noffs > fp->idx->moffs )
    {
        fp->idx->moffs = fp->idx->noffs;
        kroundup32(fp->idx->moffs);
        fp->idx->offs = (bgzidx1_t*) realloc(fp->idx->offs, fp->idx->moffs*sizeof(bgzidx1_t));
        if ( !fp->idx->offs ) return -1;
    }
    fp->idx->offs[ fp->idx->noffs-1 ].uaddr = fp->idx->ublock_addr;
    fp->idx->offs[ fp->idx->noffs-1 ].caddr = fp->block_address;
    return 0;
}

int bgzf_index_dump(BGZF *fp, const char *bname, const char *suffix)
{
    if (bgzf_flush(fp) != 0) return -1;

    assert(fp->idx);
    char *tmp = NULL;
    if ( suffix )
    {
        int blen = strlen(bname);
        int slen = strlen(suffix);
        tmp = (char*) malloc(blen + slen + 1);
        if ( !tmp ) return -1;
        memcpy(tmp,bname,blen);
        memcpy(tmp+blen,suffix,slen+1);
    }

    FILE *idx = fopen(tmp?tmp:bname,"wb");
    if ( tmp ) free(tmp);
    if ( !idx ) return -1;

    // Note that the index contains one extra record when indexing files opened
    // for reading. The terminating record is not present when opened for writing.
    // This is not a bug.

    int i;
    if ( fp->is_be )
    {
        uint64_t x = fp->idx->noffs - 1;
        fwrite(ed_swap_8p(&x), 1, sizeof(x), idx);
        for (i=1; i<fp->idx->noffs; i++)
        {
            x = fp->idx->offs[i].caddr; fwrite(ed_swap_8p(&x), 1, sizeof(x), idx);
            x = fp->idx->offs[i].uaddr; fwrite(ed_swap_8p(&x), 1, sizeof(x), idx);
        }
    }
    else
    {
        uint64_t x = fp->idx->noffs - 1;
        fwrite(&x, 1, sizeof(x), idx);
        for (i=1; i<fp->idx->noffs; i++)
        {
            fwrite(&fp->idx->offs[i].caddr, 1, sizeof(fp->idx->offs[i].caddr), idx);
            fwrite(&fp->idx->offs[i].uaddr, 1, sizeof(fp->idx->offs[i].uaddr), idx);
        }
    }
    fclose(idx);
    return 0;
}


int bgzf_index_load(BGZF *fp, const char *bname, const char *suffix)
{
    char *tmp = NULL;
    if ( suffix )
    {
        int blen = strlen(bname);
        int slen = strlen(suffix);
        tmp = (char*) malloc(blen + slen + 1);
        if ( !tmp ) return -1;
        memcpy(tmp,bname,blen);
        memcpy(tmp+blen,suffix,slen+1);
    }

    FILE *idx = fopen(tmp?tmp:bname,"rb");
    if ( tmp ) free(tmp);
    if ( !idx ) return -1;

    fp->idx = (bgzidx_t*) calloc(1,sizeof(bgzidx_t));
    uint64_t x;
    if ( fread(&x, 1, sizeof(x), idx) != sizeof(x) ) return -1;

    fp->idx->noffs = fp->idx->moffs = 1 + (fp->is_be ? ed_swap_8(x) : x);
    fp->idx->offs  = (bgzidx1_t*) malloc(fp->idx->moffs*sizeof(bgzidx1_t));
    fp->idx->offs[0].caddr = fp->idx->offs[0].uaddr = 0;

    int i;
    if ( fp->is_be )
    {
        int ret = 0;
        for (i=1; i<fp->idx->noffs; i++)
        {
            ret += fread(&x, 1, sizeof(x), idx); fp->idx->offs[i].caddr = ed_swap_8(x);
            ret += fread(&x, 1, sizeof(x), idx); fp->idx->offs[i].uaddr = ed_swap_8(x);
        }
        if ( ret != sizeof(x)*2*(fp->idx->noffs-1) ) return -1;
    }
    else
    {
        int ret = 0;
        for (i=1; i<fp->idx->noffs; i++)
        {
            ret += fread(&x, 1, sizeof(x), idx); fp->idx->offs[i].caddr = x;
            ret += fread(&x, 1, sizeof(x), idx); fp->idx->offs[i].uaddr = x;
        }
        if ( ret != sizeof(x)*2*(fp->idx->noffs-1) ) return -1;
    }
    fclose(idx);
    return 0;

}

int bgzf_useek(BGZF *fp, long uoffset, int where)
{
    if ( !fp->is_compressed )
    {
        if (hseek(fp->fp, uoffset, SEEK_SET) < 0)
        {
            fp->errcode |= BGZF_ERR_IO;
            return -1;
        }
        fp->block_length = 0;  // indicates current block has not been loaded
        fp->block_address = uoffset;
        fp->block_offset = 0;
        bgzf_read_block(fp);
        fp->uncompressed_address = uoffset;
        return 0;
    }

    if ( !fp->idx )
    {
        fp->errcode |= BGZF_ERR_IO;
        return -1;
    }

    // binary search
    int ilo = 0, ihi = fp->idx->noffs - 1;
    while ( ilo<=ihi )
    {
        int i = (ilo+ihi)*0.5;
        if ( uoffset < fp->idx->offs[i].uaddr ) ihi = i - 1;
        else if ( uoffset >= fp->idx->offs[i].uaddr ) ilo = i + 1;
        else break;
    }
    int i = ilo-1;
    if (hseek(fp->fp, fp->idx->offs[i].caddr, SEEK_SET) < 0)
    {
        fp->errcode |= BGZF_ERR_IO;
        return -1;
    }
    fp->block_length = 0;  // indicates current block has not been loaded
    fp->block_address = fp->idx->offs[i].caddr;
    fp->block_offset = 0;
    if ( bgzf_read_block(fp) < 0 ) return -1;
    if ( uoffset - fp->idx->offs[i].uaddr > 0 )
    {
        fp->block_offset = uoffset - fp->idx->offs[i].uaddr;
        assert( fp->block_offset <= fp->block_length );     // todo: skipped, unindexed, blocks
    }
    fp->uncompressed_address = uoffset;
    return 0;
}

long bgzf_utell(BGZF *fp)
{
    return fp->uncompressed_address;    // currently maintained only when reading
}

