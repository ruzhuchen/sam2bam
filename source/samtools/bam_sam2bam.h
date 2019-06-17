/*  bam_sam2bam.h

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

#include <stdint.h>
#include <sys/types.h>
#include "htslib/sam.h"

// #define DEBUG_POS

#define MT_HASH		// enable pipelining between sam_read and registration of reads to the address space
#define BAMP_ARRAY

//#define BUCKET_BITS	12 // 4K 	// BUCKET_SIZE is 2^BUCKET_BITS
#define BUCKET_BITS	16 // 64 K	// BUCKET_SIZE is 2^BUCKET_BITS

// SN:1 length 0x0EDB433D
#define POS_BITS	28
#define TID_BITS	12 // 12:0-4095 (hg38 has 3366), 8:0-255, 7:0-127

#define MAPQL_BITS		7
#define TLEN_BITS		23	
#define TLEN_MAX		((1L<<TLEN_BITS)-1)
#define TLEN_MIN		(-TLEN_MAX-1)

typedef struct {
  uint32_t unclipped:1;		// [1]	*** DO NOT MOVE *** (1 bit)
  uint32_t mapqL    :MAPQL_BITS;// 8
  uint32_t tid      :TID_BITS;  // 36
  uint32_t mtid     :TID_BITS;	// 48
  uint16_t flag;                // 24
  uint16_t filter_data;         // 64   SAM: 0x21-0x7E, BAM: 0x00-0x5D, 0x5D*704=65472 < UINT16_MAX             *** used only for Clipped POS bamptr

  uint32_t libid    :8;
  int32_t  mapqH    :1;		// [2]	(1 bit)
  int32_t  tlen     :TLEN_BITS;	//
  uint32_t offset_unclipped_pos;// 	offset unclipped position (calculated only for mapped reads)

  uint64_t id;                  //*[3]  The same ID for a bamptr for clipped and a bamptr for unclipped that were created for the same read
  uint32_t pos;                 // [4]
  uint32_t mpos;                //
  uint64_t qnameid;             // [5]	qnameid is used for finding mates
} baminfoU1_t;

typedef struct {
  uint8_t  unclipped:1;		// [1] 	*** DO NOT MOVE *** (1 bit)
  uint8_t  __padding:3;		//	(3 bits)
#define FLAGL_BITS		4
  uint8_t  flagL    :4;		//	(4 bits)
  uint8_t  flagH;
  uint16_t l_data;              //
  int32_t  unclip;		//
  union {                       //*[2]  The same ID for a bamptr for clipped and a bamptr for unclipped that were created for the same read
    uint64_t bamid;		// needed for finding the bamptr in the clipped bamptr array that is corresponding to a given unclipped bamptr
				//   if unmapped, there is no unclipped bamptr, therefore, bamid is not used.
    uint64_t qnameid;		// needed for sortsam. if unmapped, there is no corresponding unclipped bamptr, which has qnameid
  };
  bam_vaddr_t bamva;            // [3]  virtual address of BAM record in the repository
} baminfoC1_t;

typedef struct {
  uint8_t  unclipped:1;		// [1] 	*** DO NOT MOVE *** (1 bit)
  uint8_t  __padding:7;
} baminfo1stByte_t;

typedef struct {
//  bam1_t *bam__;
  union{
    struct {
      struct {
        baminfoU1_t info1;      // [1]-[5]	*** DO NOT MOVE ***
      };
      struct bamptr *mate_bamp;	// [6]
    };
    uint64_t __align__[6];
  };
} baminfoU_t;

typedef struct {
  union{
    struct {
      baminfoC1_t info1;        // [1]-[3]	*** DO NOT MOVE ***
    };
    uint64_t __align__[3];
  };
} baminfoC_t;

typedef struct bamptr {
#if defined(DEBUG_POS)
  char *dump_qname;
#endif
//  uint8_t unclipped;
#if !defined(BAMP_ARRAY)
  struct bamptr *clipped_next;
  struct bamptr *unclipped_next;// unclipped pos
#endif
  union {
//    struct {
//      struct bamptr *mate__;
//    };
    // declare array[0] so that sizeof(bamptr_t) does not include the size
    baminfo1stByte_t _unclipped[0];
    baminfoC_t baminfoC[0];
    baminfoU_t baminfoU[0];
  };
} bamptr_t;

#if defined(BAMP_ARRAY)
struct bamptr_array {
union {
  struct bamptr_array *free_next;
  struct{
    int size;
    int max;            // to extend an array quickly since the array length at (i,j)=(0,0), where unmapped reads are mapped, could be very long
    bamptr_t bp[];
  };
};
};
#endif

typedef uint64_t vid_t;
#if defined(BAMP_ARRAY)
typedef struct bamptr_array bamptr_array_t;
#endif

typedef struct bucket_array bucket_array_t;

typedef struct bucket bucket_t;

#define container_of(ptr, type, member) ({ \
				                const typeof( ((type *)0)->member ) *__mptr = (ptr); \
				                (type *)( (char *)__mptr - offsetof(type,member) );})

#define KEY_BITS	(TID_BITS+POS_BITS)
#define KEY_MASK	(-1UL<<(sizeof(vid_t)*8-KEY_BITS)>>(sizeof(vid_t)*8-KEY_BITS))
#define TID_MASK	(-1UL<<(sizeof(vid_t)*8-TID_BITS)>>(sizeof(vid_t)*8-TID_BITS))
#define POS_MASK	(-1UL<<(sizeof(vid_t)*8-POS_BITS)>>(sizeof(vid_t)*8-POS_BITS))
#define MAX_TID		TID_MASK
#define MAX_POS		POS_MASK
#define MAX_KEY		KEY_MASK

#define BUCKET_A_BITS	(KEY_BITS-BUCKET_BITS)
#define BUCKET_MASK	(-1UL<<(sizeof(vid_t)*8-BUCKET_BITS)>>(sizeof(vid_t)*8-BUCKET_BITS))
#define BUCKET_A_MASK	(-1UL<<(sizeof(vid_t)*8-BUCKET_A_BITS)>>(sizeof(vid_t)*8-BUCKET_A_BITS))
#define BUCKET_SIZE	(BUCKET_MASK + 1)
#define BUCKET_ARRAY_LENGTH	(BUCKET_A_MASK + 1)
#define get_index1(id)	(((BUCKET_A_MASK<<BUCKET_BITS)&(id))>>BUCKET_BITS)
#define get_index2(id)	(BUCKET_MASK&(id))

struct func_vector_v1 {
  uint64_t (*bucket_array_get_max)(bucket_array_t*);
  uint64_t (*bucket_array_get_min)(bucket_array_t*);
  bamptr_array_t** (*bucket_get_bamptr_array_top)(bucket_t *);
  int (*bucket_get_max)(bucket_t *);
  int (*bucket_get_min)(bucket_t *);
  uint64_t (*gen_id_clipped)(vid_t, vid_t);
  bucket_t* (*get_bucket)(int, bucket_array_t*, int);
  int* (*get_cpu_map)(void);
  bamptr_t* (*get_fifo_list_from_bucket)(bucket_t*, int, int);
  int  (*get_n_physical_cores)(void);
  uint32_t (*get_offset_unclipped_pos_from_index)(int, int);
  void (*md_free)(void *, size_t, long);
  void*(*md_malloc)(size_t, long, long);
  void*(*md_realloc)(void *, size_t, long);
  long (*md_register_mem_id)(const char*);

  uint8_t (*get_libid)(const char*);
  uint8_t (*bam_get_libid)(bam_hdr_t *h, bam1_t *b) ;

  void (*get_qname)(uint64_t qnameid, char *str, long l_qname, long *len);
  bamptr_t* (*bamptr_get_U)(bamptr_t *, int, int);
  int (*picard_sortsam_cmpfunc)(const void *, const void *, void *args);

  int n_threads;
  bam_hdr_t *header;
  bucket_array_t *bucket_array;
  bucket_array_t *bucketU_array;
};

#define sizeof_bamptr(unclipped)	(sizeof(bamptr_t) + (unclipped ? sizeof(baminfoU_t) : sizeof(baminfoC_t)))
#define bamptr_get_bamptr(bamp, for_mate)       (assert_u(bamp)(void*)(for_mate ? (bamp)->baminfoU[0].mate_bamp : bamp))
#define bamptr_get_tid(bamp, for_mate) 	(assert_u(bamp)for_mate ? (bamp)->baminfoU[0].info1.mtid : (bamp)->baminfoU[0].info1.tid)
#define bamptr_get_pos(bamp, for_mate) 	(assert_u(bamp)for_mate ? (bamp)->baminfoU[0].info1.mpos : (bamp)->baminfoU[0].info1.pos) 
#define get_bamptr_at(ba, i, unclipped)	((bamptr_t*)((char*)(ba)->bp + sizeof_bamptr(unclipped)*(i)))
#define bamptrC_get_bamid(bamp) 	(assert_c(bamp)assert_c_mapped(bamp)(bamp)->baminfoC[0].info1.bamid)
#define bamptrU_get_bamid(bamp) 	(assert_u(bamp)(bamp)->baminfoU[0].info1.id)
#define bamptr_set_dup(bamp)		{assert_c(bamp)(bamp)->baminfoC[0].info1.flagH |= (BAM_FDUP>>FLAGL_BITS);}
//#define bamptr_set_rev(bamp)		{assert_c(bamp)(bamp)->baminfoC[0].info1.rev=1;}
#if defined(DEBUG_POS)
#define dump_bamptr_get_qname(bamp, for_mate)	((bamp)->dump_qname)
#else
#define dump_bamptr_get_qname(bamp, for_mate)	FIXME
#endif
#define dump_bamptr_get_mtid(bamp)   (assert_u(bamp)(bamp)->baminfoU[0].mate_bamp ? bamptr_get_tid((bamp)->baminfoU[0].mate_bamp) : -8888 )
#define bamptr_get_qnameid(bamp)	(assert_u(bamp)(bamp)->baminfoU[0].info1.qnameid)
#define bamptrC_get_qnameid(bamp)	(assert_c(bamp)assert_c_unmapped(bamp)(bamp)->baminfoC[0].info1.qnameid)
#define FLAG_NOT_VALID 0x8000 /* not used in SAM*/
#define bamptr_get_flag(bamp)		(assert_u(bamp)(bamp)->baminfoU[0].info1.flag)
#define bamptrC_get_flag(bamp)		(assert_c(bamp)((bamp)->baminfoC[0].info1.flagH<<FLAGL_BITS|(bamp)->baminfoC[0].info1.flagL))
#define bamptrC_get_rev(bamp)           (assert_c(bamp)((bamp)->baminfoC[0].info1.flagH & (BAM_FREVERSE>>FLAGL_BITS)))
#define bamptr_get_lib(h, bamp)		(assert_u(bamp)(bamp)->baminfoU[0].info1.libid)
#define bamptr_get_offset_unclipped_pos(bamp)	(assert_u(bamp)(bamp)->baminfoU[0].info1.offset_unclipped_pos)
#define bamptr_get_mate(bamp)		(assert_u(bamp)(bamp)->baminfoU[0].mate_bamp)
#define bamptr_get_dup(bamp)            (assert_c(bamp)(0 != ((bamp)->baminfoC[0].info1.flagH & (BAM_FDUP>>FLAGL_BITS))))
#define bamptr_get_unclip(bamp)         (assert_c(bamp)(bamp)->baminfoC[0].info1.unclip)
#define bamptr_get_mapq(bamp)           (assert_u(bamp)((bamp)->baminfoU[0].info1.mapqH<<MAPQL_BITS|(bamp)->baminfoU[0].info1.mapqL))
#define bamptr_get_tlen(bamp)		(assert_u(bamp)(bamp)->baminfoU[0].info1.tlen)

#if defined(DEBUG_POS)  // FIXME
void assert_bamptr__(bamptr_t *bamp, int is_unclipped) {
  if ((is_unclipped && !bamp->_unclipped[0].unclipped) ||
      (!is_unclipped && bamp->_unclipped[0].unclipped)) {
    fprintf(stderr, "(E) assert_bamptr is_unclipped %d bamp->unclipped %d\n", is_unclipped, bamp->_unclipped[0].unclipped);
   *(volatile int*)0;
    exit(-1);
  }
}
void assert_bamptr_mapped__(bamptr_t *bamp, int is_unclipped) {
  if ((is_unclipped && (0 != (bamp->baminfoU[0].info1.flag & BAM_FUNMAP)))) {
    fprintf(stderr, "(E) assert_bamptr_mapped is_unclipped %d unmap %d\n", is_unclipped, (0 != (bamp->baminfoU[0].info1.flag & BAM_FUNMAP)));
    *(volatile int *)0;
    exit(-1);
  } else if (!is_unclipped&& (0 != (bamp->baminfoC[0].info1.flagL & BAM_FUNMAP))) {
    fprintf(stderr, "(E) assert_bamptr_mapped is_unclipped %d unmap %d\n", is_unclipped, (0 != (bamp->baminfoC[0].info1.flagL & BAM_FUNMAP)));
    *(volatile int *)0;
    exit(-1);
  }
}
void assert_bamptr_unmapped__(bamptr_t *bamp, int is_unclipped) {
  if ((is_unclipped && (0 == (bamp->baminfoU[0].info1.flag & BAM_FUNMAP)))) {
    fprintf(stderr, "(E) assert_bamptr_unmapped is_unclipped %d map %d\n", is_unclipped, (0 == (bamp->baminfoU[0].info1.flag & BAM_FUNMAP)));
    *(volatile int *)0;
    exit(-1);
  } else if (!is_unclipped&& (0 == (bamp->baminfoC[0].info1.flagL & BAM_FUNMAP))) {
    fprintf(stderr, "(E) assert_bamptr_unmapped is_unclipped %d map %d\n", is_unclipped, (0 == (bamp->baminfoC[0].info1.flagL & BAM_FUNMAP)));
    *(volatile int *)0;
    exit(-1);
  }
}
#define assert_u(bamptr)		assert_bamptr__(bamptr, 1),
#define assert_c(bamptr)		assert_bamptr__(bamptr, 0),
#define assert_c_mapped(bamptr)		assert_bamptr_mapped__(bamptr, 0),
#define assert_c_unmapped(bamptr)	assert_bamptr_unmapped__(bamptr, 0),
#else
#define assert_u(bamptr)		
#define assert_c(bamptr)		
#define assert_c_mapped(bamptr)		
#define assert_c_unmapped(bamptr)
#endif


