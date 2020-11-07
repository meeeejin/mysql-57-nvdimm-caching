#include "pmem_mmap_obj.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

#include "ut0mutex.h"
#include "sync0types.h"
#include "mtr0log.h"
#include "trx0undo.h"

// gloabl persistent memmory region
unsigned char* gb_pm_mmap;
int gb_pm_mmap_fd;
PMEM_MMAP_MTRLOG_BUF* mmap_mtrlogbuf = NULL;

// recovery
bool is_pmem_recv = false;
uint64_t pmem_recv_offset = 0;
uint64_t pmem_recv_size = 0;


unsigned char* pm_mmap_create(const char* path, const uint64_t pool_size) {
  
  if (access(path, F_OK) != 0) {
    gb_pm_mmap_fd = open(path, O_RDWR | O_CREAT, 0777); 
    if (gb_pm_mmap_fd < 0) {
      PMEMMMAP_ERROR_PRINT("pm_mmap_file open failed\n");
      return NULL;
    }

    // set file size as pool_size
    if (truncate(path, pool_size) == -1) {
      PMEMMMAP_ERROR_PRINT("pm_mmap_file turncate failed\n");
    }

    gb_pm_mmap = (unsigned char *) mmap(NULL, pool_size, PROT_READ|PROT_WRITE, MAP_SHARED, gb_pm_mmap_fd, 0);
    if (gb_pm_mmap == MAP_FAILED) {
      PMEMMMAP_ERROR_PRINT("pm_mmap mmap() failed\n");
    }
    memset(gb_pm_mmap, 0x00, pool_size);

  } else {
    // TODO(jhaprk) add the recovery logic
    PMEMMMAP_INFO_PRINT("Start mtr recvoery process\n");
    gb_pm_mmap_fd = open(path, O_RDWR, 0777);

    if (gb_pm_mmap_fd < 0) {
      PMEMMMAP_ERROR_PRINT("pm_mmap_file open failed\n");
      return NULL;
    }

    gb_pm_mmap = (unsigned char *) mmap(NULL, pool_size, PROT_READ|PROT_WRITE, MAP_SHARED, gb_pm_mmap_fd, 0);
    if (gb_pm_mmap == MAP_FAILED) {
      PMEMMMAP_ERROR_PRINT("pm_mmap mmap() faild recovery failed\n");
    }

		// get file construct
		PMEM_MMAP_MTRLOGFILE_HDR* recv_mmap_mtrlog_fil_hdr = (PMEM_MMAP_MTRLOGFILE_HDR*) 
																													malloc(PMEM_MMAP_LOGFILE_HEADER_SZ);
		pm_mmap_read_logfile_header(recv_mmap_mtrlog_fil_hdr);	
			
		// debug
		fprintf(stderr, "[check] size: %lu, lsn: %lu, ckpt_lsn: %lu, ckpt_offset: %lu\n",
						recv_mmap_mtrlog_fil_hdr->size, recv_mmap_mtrlog_fil_hdr->flushed_lsn, 
						recv_mmap_mtrlog_fil_hdr->ckpt_lsn, recv_mmap_mtrlog_fil_hdr->ckpt_offset);

		// recvoery check
    PMEM_MMAP_MTRLOG_HDR* recv_mmap_mtrlog_hdr = (PMEM_MMAP_MTRLOG_HDR*) malloc(PMEM_MMAP_MTRLOG_HDR_SIZE);
    memcpy(recv_mmap_mtrlog_hdr, gb_pm_mmap+recv_mmap_mtrlog_fil_hdr->ckpt_offset, PMEM_MMAP_MTRLOG_HDR_SIZE);
		
		if (recv_mmap_mtrlog_fil_hdr->size == PMEM_MMAP_MTR_FIL_HDR_SIZE 
				|| recv_mmap_mtrlog_hdr->need_recv == false) {
			PMEMMMAP_INFO_PRINT("Normal Shutdown case, don't need to recveory; Recovery process is terminated\n");
		} else {
			// TODO(jhpark): real recovery process
			is_pmem_recv = true;
			pmem_recv_offset = pm_mmap_recv_check(recv_mmap_mtrlog_fil_hdr);
			pmem_recv_size = recv_mmap_mtrlog_fil_hdr->size;
			
			// jhpark: check buffer!!!!!
			// pm_mmap_recv_flush_buffer();

			PMEMMMAP_INFO_PRINT("recovery offset: %lu\n", pmem_recv_offset);
		} 

		// step1. allocate mtr_recv_sys
		// step2. 1) get header infor mation and 2) get info from mtr log region
		// step3. reconstruct undo page

    // Get header information from exsiting nvdimm log file
    //size_t recv_prev_offset = recv_mmap_mtrlog_hdr->prev;
    //memset(recv_mmap_mtrlog_hdr, 0x00, PMEM_MMAP_MTRLOG_HDR_SIZE);
    //memcpy(recv_mmap_mtrlog_hdr, gb_pm_mmap+recv_prev_offset, PMEM_MMAP_MTRLOG_HDR_SIZE);

    // debug
    //fprintf(stderr, "size: %lu\n", recv_size);
    //fprintf(stderr, "len: %lu\n", recv_mmap_mtrlog_hdr->len);
    //fprintf(stderr, "lsn: %lu\n", recv_mmap_mtrlog_hdr->lsn);
    //fprintf(stderr, "need_recovery: %d\n", recv_mmap_mtrlog_hdr->need_recv);
    
		free(recv_mmap_mtrlog_fil_hdr);
    free(recv_mmap_mtrlog_hdr);
  }

  // Force to set NVIMMM
  setenv("PMEM_IS_PMEM_FORCE", "1", 1);
  PMEMMMAP_INFO_PRINT("Current kernel does not recognize NVDIMM as the persistenct memory \
      We force to set the environment variable PMEM_IS_PMEM_FORCE \
      We call mync() instead of mfense()\n");

  return gb_pm_mmap;
}

void pm_mmap_mtrlogbuf_mem_free() {
  if (mmap_mtrlogbuf != NULL) {
    // TODO(jhpark): free mtr recovery system
    free(mmap_mtrlogbuf);
    mmap_mtrlogbuf = NULL;
  }
}

void pm_mmap_free(const uint64_t pool_size) {
	// free mtrMutex 
	pthread_mutex_destroy(&mmap_mtrlogbuf->mtrMutex);
	munmap(gb_pm_mmap, pool_size);
	close(gb_pm_mmap_fd);
	// free mtr log system
	pm_mmap_mtrlogbuf_mem_free();
	PMEMMMAP_INFO_PRINT("munmap persistent memroy region\n");
}


// allocate mtr log buffer
int pm_mmap_mtrlogbuf_init(const size_t size) {

  mmap_mtrlogbuf = static_cast<PMEM_MMAP_MTRLOG_BUF*>(malloc(sizeof(PMEM_MMAP_MTRLOG_BUF)));

	// TODO(jhpark): add mcs-lock version
	pthread_mutex_init(&mmap_mtrlogbuf->mtrMutex, NULL);
  mmap_mtrlogbuf->size = size;
  mmap_mtrlogbuf->cur_offset = PMEM_MMAP_MTR_FIL_HDR_SIZE;
	mmap_mtrlogbuf->prev_offset = PMEM_MMAP_MTR_FIL_HDR_SIZE;
	mmap_mtrlogbuf->ckpt_offset = PMEM_MMAP_MTR_FIL_HDR_SIZE;
  PMEMMMAP_INFO_PRINT("MTR LOG BUFFER structure initialization finished!\n"); 
  return 0;
}

// get mtr log header information
void pm_mmap_read_logfile_header(PMEM_MMAP_MTRLOGFILE_HDR* mtrlog_fil_hdr) {
	mtrlog_fil_hdr->size = mach_read_from_8(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_SIZE_OFFSET);
	mtrlog_fil_hdr->flushed_lsn = mach_read_from_8(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_LSN);
	mtrlog_fil_hdr->ckpt_lsn = mach_read_from_8(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_CKPT_LSN);
	mtrlog_fil_hdr->ckpt_offset = mach_read_from_8(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_CKPT_OFFSET);
}

// write header offset (i.e., size) of latest mtr log
void pm_mmap_write_logfile_header_size(uint64_t size) {
  byte hdr_size[8];
  mach_write_to_8(hdr_size, size);
  memcpy(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_SIZE_OFFSET, hdr_size, 8);
	flush_cache(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_SIZE, 8);
}

// write oldeset_lsn of NVDIMM-caching page when NVDIMM-caching page is flushed 
void pm_mmap_write_logfile_header_lsn(uint64_t lsn) {
	byte hdr_lsn[8];
	mach_write_to_8(hdr_lsn, (uint64_t)lsn);
	memcpy(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_LSN, hdr_lsn, 8);
	flush_cache(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_LSN, 8);
}

// write checkpoint info 
void pm_mmap_write_logfile_header_ckpt_info(uint64_t offset, uint64_t lsn) {
	byte ckpt_offset[8], ckpt_lsn[8];
	mach_write_to_8(ckpt_offset, offset);
	mach_write_to_8(ckpt_lsn, (uint64_t)lsn);
	memcpy(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_CKPT_LSN, ckpt_lsn, 8);
	memcpy(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_CKPT_OFFSET, ckpt_offset, 8);
	flush_cache(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_CKPT_LSN, 16);
}

// (eager) checkpoint mtr log return ckpt_offset
void pm_mmap_log_commit(ulint cur_space, ulint cur_page, ulint cur_offset) {
	
	size_t offset = mmap_mtrlogbuf->ckpt_offset;
	bool ckpt_flag = true;

	while (true) {

		if (offset >= cur_offset) {
			break;
		}

		PMEM_MMAP_MTRLOG_HDR *tmp_mmap_hdr = (PMEM_MMAP_MTRLOG_HDR *) malloc(PMEM_MMAP_MTRLOG_HDR_SIZE);
		memset(tmp_mmap_hdr, 0, sizeof(PMEM_MMAP_MTRLOG_HDR));
		memcpy(tmp_mmap_hdr, gb_pm_mmap + offset, PMEM_MMAP_MTRLOG_HDR_SIZE);
		ut_ad(tmp_mmap_hdr == NULL);

		// (jhpark): At this point, call checkpoint and then call commit
		// no need to commit
		if (tmp_mmap_hdr->len == 0) {
			break;
		}	
	
		if(tmp_mmap_hdr->need_recv == false) {
			mmap_mtrlogbuf->ckpt_offset = offset;
			offset += PMEM_MMAP_MTRLOG_HDR_SIZE + tmp_mmap_hdr->len;
			continue;
		}

		if (cur_space == tmp_mmap_hdr->space &&
				cur_page == tmp_mmap_hdr->page_no) {

			// 1. unset recovery flag
			bool val = false;
			memcpy(gb_pm_mmap + offset, &val, (size_t)offsetof(PMEM_MMAP_MTRLOG_HDR, len));
			// 2. keep ckpt_offset (continuous)
			offset += PMEM_MMAP_MTRLOG_HDR_SIZE + tmp_mmap_hdr->len;

			if (ckpt_flag) {
				mmap_mtrlogbuf->ckpt_offset = offset;
				// write ckpt info (lsn parameter is unused)
				//fprintf(stderr, "write ckpt info offset: %lu\n", offset);
				pm_mmap_write_logfile_header_ckpt_info(offset, 0);
			}
//			fprintf(stderr, "[mtr-commit] YES ckpt_offset: %lu space: %lu page_no: %lu\n"
//							,mmap_mtrlogbuf->ckpt_offset,tmp_mmap_hdr->space, tmp_mmap_hdr->page_no);
		} else {
//			fprintf(stderr, "[mtr-commit] NO space: %lu page_no: %lu\n"
//							,tmp_mmap_hdr->space, tmp_mmap_hdr->page_no);
			
			offset += PMEM_MMAP_MTRLOG_HDR_SIZE + tmp_mmap_hdr->len;
			ckpt_flag = false;
		}
		// free tmp_mmap_hdr
		free(tmp_mmap_hdr);
	}
	
}

uint64_t pm_mmap_log_checkpoint(uint64_t cur_offset) {
	size_t finish_offset = mmap_mtrlogbuf->ckpt_offset;

	///////////////////////////////////////////////////
	// recovery test
//	PMEMMMAP_ERROR_PRINT("RECOVERY TEST !!!");
//	exit(1);
	///////////////////////////////////////////////////

	// invalidate all offset;
	memset(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_SIZE, 0x00, (finish_offset - PMEM_MMAP_MTR_FIL_HDR_SIZE));
	// copy existing offset;
	if (cur_offset < finish_offset) {
		PMEMMMAP_ERROR_PRINT("offset error at checkpoint!");
	}

	memcpy(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_SIZE, gb_pm_mmap + finish_offset, (cur_offset - finish_offset));
	mmap_mtrlogbuf->ckpt_offset = PMEM_MMAP_MTR_FIL_HDR_SIZE;

	// debug	
	//fprintf(stderr, "[mtr-checkpoint] ckpt_offset: %lu, len: %lu\n", finish_offset, (cur_offset - finish_offset));

	//PMEM_MMAP_MTRLOG_HDR tmp_hdr;
	//memcpy(&tmp_hdr, gb_pm_mmap + mmap_mtrlogbuf->ckpt_offset, PMEM_MMAP_MTRLOG_HDR_SIZE);
	//fprintf(stderr, "[mtr-checkpoint] after checkpoint need_recv: %lu len: %lu lsn: %lu\n"
	//							, tmp_hdr.need_recv, tmp_hdr.len, tmp_hdr.lsn);

	return PMEM_MMAP_MTR_FIL_HDR_SIZE + (cur_offset - finish_offset);
}

// write mtr log
ssize_t pm_mmap_mtrlogbuf_write( 
    const uint8_t* buf,
    unsigned long int n,
    unsigned long int lsn) 
{
  unsigned long int ret = 0;

	pthread_mutex_lock(&mmap_mtrlogbuf->mtrMutex);

  // (jhpark): original version of mtr logging relies on external offset value
  //           which is persisted on every updates
  //           cur_offset points offset which header will be appended
  if (mmap_mtrlogbuf == NULL) {
    PMEMMMAP_ERROR_PRINT("mmap_mtrlogbuf is NULL!\n");
		return -1;
  }

  // jhpark: Force to trigger mtr_log_checkpoint if the offset becomes larger than threshold
	size_t offset = mmap_mtrlogbuf->cur_offset;
	int limit = mmap_mtrlogbuf->size * 0.75;

	// checkopint occurred
  if (offset + n > limit) {
		// debug
		//PMEMMMAP_INFO_PRINT("[WRNING] mmap_mtrlogbuf is FULL!\n");
		ut_ad(offset < mmap_mtrlogbuf->size);

		offset = pm_mmap_log_checkpoint(offset);
		mmap_mtrlogbuf->prev_offset = offset;
		mmap_mtrlogbuf->cur_offset = offset;
	}

	// Fill the mmap_mtr log header info.
  PMEM_MMAP_MTRLOG_HDR* mmap_mtr_hdr = (PMEM_MMAP_MTRLOG_HDR*) malloc(PMEM_MMAP_MTRLOG_HDR_SIZE);
  mmap_mtr_hdr->len = n;
  // lsn from log_sys
  mmap_mtr_hdr->lsn = lsn;
  // lsn from mtr_log_sys
  //mmap_mtr_hdr->mtr_lsn = mmap_mtrlogbuf->mtr_sys_lsn+1;
	//mmap_mtrlogbuf->mtr_sys_lsn++;
	// prev hdr offset
	mmap_mtr_hdr->prev_offset = mmap_mtrlogbuf->prev_offset;
	// set recv_flag
	mmap_mtr_hdr->need_recv = true;

#ifdef UNIV_LOG_HEADER
	// log << header << persist(log) << payload << persist(log)
 	// (jhpark) : header version brought from 
 	//            [Persistent Memory I/O Primitives](https://arxiv.org/pdf/1904.01614.pdf)
 
	// check mtr log type
	const byte *ptr = buf;
	const byte *end_ptr = buf+n;
	ulint cur_space = 0, cur_page = 0;
	mlog_id_t type;

	type = (mlog_id_t)((ulint)*ptr & ~MLOG_SINGLE_REC_FLAG);
	ptr++;
	cur_space = mach_parse_compressed(&ptr, end_ptr);
	if (ptr != NULL) {
		cur_page = mach_parse_compressed(&ptr, end_ptr);
		//fprintf(stderr, "mtr log type: %lu space: %lu page: %lu\n", type, cur_space, cur_page);
	}

	mmap_mtr_hdr->space = cur_space;
	mmap_mtr_hdr->page_no = cur_page;
 
  // write header + payload
  uint64_t org_offset = offset;
  memcpy(gb_pm_mmap+offset, mmap_mtr_hdr, (size_t)PMEM_MMAP_MTRLOG_HDR_SIZE);
  offset += PMEM_MMAP_MTRLOG_HDR_SIZE;
  memcpy(gb_pm_mmap+offset, buf, n);

	// add space_id and page_no in the header
	if (type == MLOG_2BYTES) {
		ulint page_off = mach_read_from_2(ptr);
		ptr += 2;
		ulint state = mach_parse_compressed(&ptr, end_ptr);
		
		// At this point, we can remove mtr log for this undo page
		if (state >= TRX_UNDO_CACHED) {
			ut_ad(state != TRX_UNDO_PREPARED);

			pm_mmap_log_commit(cur_space, cur_page, offset+n);
		} 
	}
	

  // persistent barrier
  flush_cache(gb_pm_mmap+org_offset, (size_t)(PMEM_MMAP_MTRLOG_HDR_SIZE + n));

  // jhpark: ALWAYS write offset of current mtr log.
	//				 In recovery, we must starts to read from this offset.
	//				 Also, maintain cur_offset in mtr_sys object to track the "next_write_to_offset"
  pm_mmap_write_logfile_header_size(org_offset);
	mmap_mtrlogbuf->prev_offset = mmap_mtrlogbuf->cur_offset;
  mmap_mtrlogbuf->cur_offset = offset + n;
  free(mmap_mtr_hdr);
#endif

/*
#elif UNIV_LOG_ZERO
  	// log << header << cnt << payload << persist(log)
  	volatile int org_offset = offset;
  	memcpy(pdata+offset, mmap_mtr_hdr, (size_t)PMEM_MMAP_MTRLOG_HDR_SIZE);
  	offset += PMEM_MMAP_MTRLOG_HDR_SIZE;
  	int cnt = __builtin_popcount((uint64_t)buf);
  	memcpy(pdata+offset, &cnt, sizeof(cnt));
  	offset += sizeof(cnt);
  	memcpy(pdata+offset, buf, (size_t)n);
  	// persistent barrier
  	flush_cache(pdata+org_offset, (size_t)(PMEM_MMAP_MTRLOG_HDR_SIZE+sizeof(cnt)+n));
  	mmap_mtrlogbuf->cur_offset = offset + n;
  	// persistent barrier (for now, just ignore)
  	//flush_cache(&mmap_mtrlogbuf->cur_offset, sizeof(mmap_mtrlogbuf->cur_offset));
#endif
*/

	pthread_mutex_unlock(&mmap_mtrlogbuf->mtrMutex);
  ret = n;
  return ret;
}

// commit mtr log 
void pm_mmap_mtrlogbuf_commit(unsigned char* rec, unsigned long cur_rec_size ,ulint space, ulint page_no) {
	// TODO(jhaprk): Keep page modification finish log for recovery	
	// For current mtr logging version, we jsut ignore this function
	//return;
	flush_cache(rec, cur_rec_size);

/*
	if (mmap_mtrlogbuf == NULL) return;
	
	//fprintf(stderr, "[mtr-commit] space: %lu page_no: %lu\n", space, page_no);
	// 1. check current cur_offset
	size_t cur_offset = mmap_mtrlogbuf->cur_offset;
	// 2. check current ckpt_offset
	size_t ckpt_offset = mmap_mtrlogbuf->ckpt_offset;
	// 3. remove stale log data
	memset(gb_pm_mmap + PMEM_MMAP_MTR_FIL_HDR_SIZE, 0x00, cur_offset - PMEM_MMAP_MTR_FIL_HDR_SIZE);
	//fprintf(stderr, "cur_offset: %lu ckpt_offset: %lu\n", cur_offset, ckpt_offset);
	mmap_mtrlogbuf->cur_offset = PMEM_MMAP_MTR_FIL_HDR_SIZE;
	mmap_mtrlogbuf->prev_offset = PMEM_MMAP_MTR_FIL_HDR_SIZE;
	// really needed?
	pm_mmap_write_logfile_header_size(PMEM_MMAP_MTR_FIL_HDR_SIZE);
*/

}


// compare mtr log with given space_id, and page_no
// offset is start offset of "log body" of mtr log
bool pm_mmap_mtrlogbuf_identify(size_t offset, size_t n, ulint space, ulint page_no) {
	// mtr log structure: [type(1)] [space_id(4)] [page_no(4)]
	// <WARNING> mach_write_compressed used when writing space_id and page_no
	// + 1 means jump over MTR_LOG_TYPE
	ulint cur_space, cur_page;
	const byte *ptr = gb_pm_mmap+offset;
	const byte *end_ptr = gb_pm_mmap+offset+n;
	ptr++;

	cur_space = mach_parse_compressed(&ptr, end_ptr);
	if (ptr != NULL) {
		cur_page = mach_parse_compressed(&ptr, end_ptr);
	}

	//fprintf(stderr, "[mtr identify] space(%lu) : %lu pange_no(%lu) : %lu\n", space, cur_space, page_no, cur_page);
	return ((cur_space == space) && (cur_page == page_no));
}

void pm_mmap_mtrlogbuf_unset_recv_flag(size_t offset) {
	memcpy(gb_pm_mmap + offset, false, sizeof(bool));
	// need flush? No we can recover by using commit log
}

void pm_mmap_mtrlogbuf_commit_v1(ulint space, ulint page_no) {
	// 1. start to inspect mtr log from latest ckpt_offset
	// 2. check specific mtr log with spaced_id and page_no
	// 2.1 (yes) check need_recv is set goto 3.1
	// 2.2 (no) check need recv is set goto 3.2
	// 3.1. update ckpt_offset to current offset
	// 4. move to next mtr log (until cur_offset)
	
	if (mmap_mtrlogbuf == NULL) return;

	size_t offset = mmap_mtrlogbuf->ckpt_offset;
	while (offset != mmap_mtrlogbuf->cur_offset) {

		fprintf(stderr, "offset : %lu cur_offset: %lu\n", offset, mmap_mtrlogbuf->cur_offset);
		PMEM_MMAP_MTRLOG_HDR mmap_mtr_hdr;
		memcpy(&mmap_mtr_hdr, gb_pm_mmap + offset, (size_t) PMEM_MMAP_MTRLOG_HDR_SIZE);
		uint64_t data_len = mmap_mtr_hdr.len;
		bool need_recv = mmap_mtr_hdr.need_recv;
	
		fprintf(stderr, "[mtr info] data_len : %lu lsn: %lu need_recv : %d\n", 
						data_len, mmap_mtr_hdr.lsn, need_recv);
	
		// move next
		uint64_t org_offset = offset;
		offset += PMEM_MMAP_MTRLOG_HDR_SIZE;

		if (pm_mmap_mtrlogbuf_identify(offset, data_len, space, page_no)) {
			pm_mmap_mtrlogbuf_unset_recv_flag(org_offset);
		}
		if (need_recv) {
			mmap_mtrlogbuf->ckpt_offset = org_offset;
		}
		offset += data_len;
	}
	fprintf(stderr, "break out ! ckpt_offset: %lu\n", mmap_mtrlogbuf->ckpt_offset);
}

