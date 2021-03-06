/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * repl_page_buffer.h -
 */

#ifndef _REPL_PAGE_BUFFER_H_
#define _REPL_PAGE_BUFFER_H_

#ident "$Id$"

#include <unistd.h>

#include "storage_common.h"
#include "log_impl.h"

#include "repl_common.h"

#define CIRP_LOGPB_AREA_SIZE	(100)

#define SIZEOF_CIRP_LOGPB \
	(offsetof(CIRP_LOGPB, log_page) + IO_MAX_PAGE_SIZE)

#define LA_MAX_UNFLUSHED_REPL_ITEMS             200

typedef struct cirp_recdes_pool CIRP_RECDES_POOL;
struct cirp_recdes_pool
{
  RECDES *recdes_arr;
  char *area;			/* continuous area for recdes data */
  int next_idx;
  int db_page_size;
  int num_recdes;
  bool is_initialized;
};

typedef struct _cirp_act_log CIRP_ACT_LOG;
struct _cirp_act_log
{
  char path[PATH_MAX];
  int vdes;

  LOG_PAGE *hdr_page;
  LOG_HEADER *log_hdr;
};

typedef struct _cirp_arv_log CIRP_ARV_LOG;
struct _cirp_arv_log
{
  int arv_num;
  char path[PATH_MAX];
  int vdes;

  LOG_PAGE *hdr_page;
  struct log_arv_header *log_hdr;

  int last_deleted_arv_num;
};

typedef struct _cirp_logpb CIRP_LOGPB;
struct _cirp_logpb
{
  int num_fixed;

#if !defined(NDEBUG)
  char fix_file_name[1024];
  int fix_line_number;
  char unfix_file_name[1024];
  int unfix_line_number;
#endif

  bool recently_freed;
  bool in_archive;

  LOG_PAGEID pageid;

  LOG_PAGE log_page;
};

typedef struct _cirp_logpb_area CIRP_LOGPB_AREA;
struct _cirp_logpb_area
{
  CIRP_LOGPB_AREA *next;
  CIRP_LOGPB *area;
};

typedef struct _cirp_logpb_cache CIRP_LOGPB_CACHE;
struct _cirp_logpb_cache
{
  MHT_TABLE *hash_table;
  int num_buffer;
  CIRP_LOGPB **buffer;
  CIRP_LOGPB_AREA *area_head;
};

typedef struct _cirp_log_buffer_msg CIRP_BUF_MGR;
struct _cirp_log_buffer_msg
{
  bool is_initialized;

  char log_path[PATH_MAX];
  char prefix_name[PATH_MAX];
  char host_name[PATH_MAX];
  char log_info_path[PATH_MAX];

  CIRP_ACT_LOG act_log;
  CIRP_ARV_LOG arv_log;
  CIRP_LOGPB_CACHE cache;

  int db_logpagesize;

  char *rec_type;
  LOG_ZIP *undo_unzip;
  LOG_ZIP *redo_unzip;

  int last_nxarv_num;
  time_t last_arv_deleted_time;

  /* recdes pool */
  CIRP_RECDES_POOL la_recdes_pool;
};

#define CIRP_LOG_IS_IN_ARCHIVE(mgr, pageid) \
  ((pageid) < (mgr)->act_log.log_hdr->ha_info.nxarv_pageid)

#define CIRP_LOGAREA_SIZE(mgr)       ((mgr)->db_logpagesize - SSIZEOF(LOG_HDRPAGE))

#define CIRP_IS_VALID_LSA(mgr, lsa) \
  ( ((lsa)->pageid == LOGPB_HEADER_PAGE_ID)        \
      || LSA_ISNULL(lsa)                           \
      || ((lsa)->pageid >= 0 && (lsa)->offset >= 0 && (lsa)->offset < CIRP_LOGAREA_SIZE(mgr)) )

#define CIRP_IS_VALID_LOG_RECORD(mgr, lrec)                             \
    (CIRP_IS_VALID_LSA((mgr), &((lrec)->prev_tranlsa))                  \
      && CIRP_IS_VALID_LSA((mgr), &((lrec)->back_lsa))                  \
      && CIRP_IS_VALID_LSA((mgr), &((lrec)->forw_lsa))                  \
      && ((lrec)->trid == NULL_TRANID || (lrec)->trid >= 0)             \
      && ((lrec)->type > LOG_SMALLER_LOGREC_TYPE && (lrec)->type < LOG_LARGER_LOGREC_TYPE))

#define CIRP_LOG_READ_ADVANCE_WHEN_DOESNT_FIT(mgr, result, length, offset, pageid, pgptr, org_pgptr) \
  do {                                                                         \
    if ((offset)+(length) >= CIRP_LOGAREA_SIZE(mgr)) {                         \
      if ((org_pgptr) != (pgptr)) {                                            \
        cirp_logpb_release (mgr, (pgptr)->hdr.logical_pageid);                 \
      }                                                                        \
      (pgptr) = cirp_logpb_get_log_page((mgr), ++(pageid));                    \
      if ((pgptr) == NULL) {                                                   \
        (result) = er_errid ();                                                \
        assert ((result) != NO_ERROR);                                         \
        break;                                                                 \
      }                                                                        \
      (offset) = 0;                                                            \
    }                                                                          \
  } while(0)

#define CIRP_LOG_READ_ALIGN(mgr, result, offset, pageid, pgptr, org_pgptr)     \
  do {                                                                         \
    (offset) = DB_ALIGN((offset), MAX_ALIGNMENT);                              \
    while ((offset) >= CIRP_LOGAREA_SIZE(mgr)) {                               \
      if ((pgptr) != org_pgptr) {                                              \
        cirp_logpb_release (mgr, (pgptr)->hdr.logical_pageid);                 \
      }                                                                        \
      (pgptr) = cirp_logpb_get_log_page(mgr, ++(pageid));                      \
      if ((pgptr) == NULL) {                                                   \
        (result) = er_errid();                                                 \
        assert ((result) != NO_ERROR);                                         \
        break;                                                                 \
      }                                                                        \
      (offset) -= CIRP_LOGAREA_SIZE(mgr);                                      \
      (offset) = DB_ALIGN((offset), MAX_ALIGNMENT);                            \
    }                                                                          \
  } while(0)

#define CIRP_LOG_READ_ADD_ALIGN(mgr, result, add, offset, pageid, pgptr, org_pgptr) \
  do {                                                                              \
    (offset) += (add);                                                              \
    CIRP_LOG_READ_ALIGN(mgr, result, (offset), (pageid), (pgptr), (org_pgptr));     \
  } while(0)

extern int cirp_logpb_act_log_fetch_hdr (CIRP_BUF_MGR * buf_mgr);

extern int cirp_logpb_remove_archive_log (CIRP_BUF_MGR * buf_mgr,
					  LOG_PAGEID req_pageid);

extern RECDES *cirp_assign_recdes_from_pool (CIRP_BUF_MGR * buf_mgr);
extern int cirp_realloc_recdes_data (CIRP_BUF_MGR * buf_mgr,
				     RECDES * recdes, int data_size);



#define cirp_logpb_get_log_page(mgr, pageid) \
  cirp_logpb_get_log_page_debug (mgr, pageid, __FILE__, __LINE__)
extern LOG_PAGE *cirp_logpb_get_log_page_debug (CIRP_BUF_MGR * buf_mgr,
						LOG_PAGEID pageid,
						const char *file_name,
						int line_number);

#define cirp_logpb_get_page_buffer(mgr, pageid) \
    cirp_logpb_get_page_buffer_debug (mgr, pageid, __FILE__, __LINE__)
extern CIRP_LOGPB *cirp_logpb_get_page_buffer_debug (CIRP_BUF_MGR *
						     buf_mgr,
						     LOG_PAGEID pageid,
						     const char *file_name,
						     int line_number);

#define cirp_logpb_release(mgr, pageid) \
  cirp_logpb_release_debug (mgr, pageid, __FILE__, __LINE__)
extern int cirp_logpb_release_debug (CIRP_BUF_MGR * buf_mgr,
				     LOG_PAGEID pageid,
				     const char *file_name, int line_number);
extern int cirp_logpb_release_all (CIRP_BUF_MGR * buf_mgr,
				   LOG_PAGEID exclude_pageid);
extern int cirp_logpb_decache_range (CIRP_BUF_MGR * buf_mgr,
				     LOG_PAGEID from, LOG_PAGEID to);

extern void cirp_logpb_final (CIRP_BUF_MGR * buf_mgr);
extern int cirp_logpb_initialize (CIRP_BUF_MGR * buf_mgr,
				  const char *db_name, const char *log_path);

#if defined (ENABLE_UNUSED_FUNCTION)
extern int repl_dump_page (FILE * out_fp, LOG_PAGE * log_page);
#endif

extern int cirp_log_get_eot_time (CIRP_BUF_MGR * buf_mgr,
				  time_t * donetime, LOG_PAGE * pgptr,
				  LOG_LSA lsa);
extern int cirp_log_get_ha_server_state (struct log_ha_server_state *state,
					 LOG_PAGE * pgptr, LOG_LSA lsa);

extern int cirp_log_copy_fromlog (CIRP_BUF_MGR * buf_mgr,
				  char *rec_type, char *area, int length,
				  LOG_PAGEID log_pageid, PGLENGTH log_offset,
				  LOG_PAGE * pgptr);

extern CIRP_REPL_ITEM *cirp_make_repl_item_from_log (CIRP_BUF_MGR * buf_mgr,
						     LOG_PAGE * log_pgptr,
						     int log_type,
						     const LOG_LSA * lsa);
extern int cirp_log_get_gid_bitmap_update (CIRP_BUF_MGR * buf_mgr,
					   struct log_gid_bitmap_update *gbu,
					   LOG_PAGE * pgptr, LOG_LSA * lsa);
#endif /* _REPL_PAGE_BUFFER_H_ */
