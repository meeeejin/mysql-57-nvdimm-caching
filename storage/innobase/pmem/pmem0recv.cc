#include "pmem_mmap_obj.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <stddef.h>

#include "mtr0log.h"
#include "buf0buf.h"
#include "log0recv.h"
#include "mtr0mtr.h"
#include "fil0fil.h"

#include "os0file.h"
#include "page0page.h"
#include "mtr0types.h"
#include "trx0rec.h"

extern unsigned char* gb_pm_mmap;
extern uint64_t pmem_recv_size;

uint64_t pm_mmap_recv_check(PMEM_MMAP_MTRLOGFILE_HDR* log_fil_hdr) {
	size_t tmp_offset = log_fil_hdr->ckpt_offset;
	while (true) {
		fprintf(stderr, "current tmp_offset: %lu:(%lu)\n", tmp_offset, log_fil_hdr->size);
		if (tmp_offset >= log_fil_hdr->size) {
			break;
		}

		PMEM_MMAP_MTRLOG_HDR *recv_mmap_hdr = (PMEM_MMAP_MTRLOG_HDR *) malloc(PMEM_MMAP_MTRLOG_HDR_SIZE);
		memcpy(recv_mmap_hdr, gb_pm_mmap + tmp_offset, PMEM_MMAP_MTRLOG_HDR_SIZE);
		ut_ad(recv_mmap_hdr == NULL);

		fprintf(stderr, "[recovery] need_recv: %d len: %lu lsn: %lu prev_offset: %lu space: %lu page_no: %lu\n"
										,recv_mmap_hdr->need_recv, recv_mmap_hdr->len, recv_mmap_hdr->lsn,
										recv_mmap_hdr->prev_offset,
										recv_mmap_hdr->space, recv_mmap_hdr->page_no);

    if (recv_mmap_hdr->need_recv == false) {
      fprintf(stderr, "Hmm? current log doesn't need to recvoery!\n");
      tmp_offset += recv_mmap_hdr->len;
      free(recv_mmap_hdr);
      continue;
    } else {
			free(recv_mmap_hdr);
			return tmp_offset;
		}
	}
	// no need to recovery
  return -1;
}

bool pm_mmap_recv(uint64_t start_offset, uint64_t end_offset) {

	mtr_t mtr;
	size_t tmp_offset = start_offset;

	while (true) {
		fprintf(stderr, "current tmp_offset: %lu:(%lu)\n", tmp_offset, end_offset);

		if (tmp_offset >= end_offset) {
			break;
		}

		PMEM_MMAP_MTRLOG_HDR *recv_mmap_hdr = (PMEM_MMAP_MTRLOG_HDR *) malloc(PMEM_MMAP_MTRLOG_HDR_SIZE);
		memcpy(recv_mmap_hdr, gb_pm_mmap + tmp_offset, PMEM_MMAP_MTRLOG_HDR_SIZE);
		ut_ad(recv_mmap_hdr == NULL);

		fprintf(stderr, "[recovery] need_recv: %d len: %lu lsn: %lu prev_offset: %lu space: %lu page_no: %lu\n"
										,recv_mmap_hdr->need_recv, recv_mmap_hdr->len, recv_mmap_hdr->lsn,
										recv_mmap_hdr->prev_offset,
										recv_mmap_hdr->space, recv_mmap_hdr->page_no);

    if (recv_mmap_hdr->need_recv == false) {
      fprintf(stderr, "current log doesn't need to recvoery!\n");
      tmp_offset += (PMEM_MMAP_MTRLOG_HDR_SIZE + recv_mmap_hdr->len);
      free(recv_mmap_hdr);
      continue;
    }
  
		// page generation
		bool found;
		const page_size_t& page_size 
			= fil_space_get_page_size(recv_mmap_hdr->space, &found);
		
		if (!found) {
			fprintf(stderr, "This tablespace with that id (%lu) page (page_no: %lu) does not exist.\n"
			,	recv_mmap_hdr->space, recv_mmap_hdr->page_no);
		}	

		const page_id_t   page_id(recv_mmap_hdr->space, recv_mmap_hdr->page_no);
		if (buf_page_peek(page_id)) {
			buf_block_t*  block;
			mtr_start(&mtr);
			block = buf_page_get( page_id, page_size, RW_X_LATCH, &mtr);
			recv_recover_page(FALSE, block);
			fprintf(stderr, " page (page_no: %lu) is recovered!\n", recv_mmap_hdr->page_no);
			mtr_commit(&mtr); 	
		} else {
			// TODO(jhpark): implement nc version `recv_read_in_area()`
			fprintf(stderr, "current page doesn't exist buffer!\n");
			//ulint page_nos[1]; 
			//page_nos[0] = recv_mmap_hdr->page_no; // just one element
			//buf_read_recv_pages(TRUE, page_id.space(), page_nos, 1);
			//fprintf(stderr, "Recv pages at %lu\n", page_nos[0]); 
		  //recv_read_in_area(page_id);

      // Force recovery UNDO page 
      byte *mlog_data =  (byte*) malloc(recv_mmap_hdr->len);
      memcpy(mlog_data, gb_pm_mmap + tmp_offset + PMEM_MMAP_MTRLOG_HDR_SIZE, sizeof(*mlog_data)); 
      byte* ptr = mlog_data;
      byte* end_ptr = mlog_data + recv_mmap_hdr->len;
      mlog_id_t type;
      type = (mlog_id_t)((ulint)*ptr & ~MLOG_SINGLE_REC_FLAG);
    
      // get current page with space_id and page_no

			mtr_start(&mtr); 
			buf_block_t* block = buf_page_get( page_id, page_size, RW_X_LATCH, &mtr);
      byte* page = block->frame;
     	mtr_commit(&mtr); 

      switch(type) {
        case MLOG_UNDO_INSERT:
          ptr = trx_undo_parse_add_undo_rec(ptr, end_ptr, page);
					IORequest write_request(IORequest::WRITE);
					write_request.disable_compression(); // stil needed?
					int check = 0;
					check = fil_io(write_request, true, page_id, 
					univ_page_size, 0, univ_page_size.physical(), (void*) page, NULL);
      };
      // free mlog data
      free(mlog_data);

		}

    tmp_offset += (PMEM_MMAP_MTRLOG_HDR_SIZE + recv_mmap_hdr->len);
    free(recv_mmap_hdr);
	}

  return true;
}


void pm_mmap_recv_flush_buffer() {
	// step1. grap information of all nc buffer pages (space, page_no)
	// TODO(jhpark): need to modify to get pmem_log_buffer size automatically

	uint64_t cur_offset = 0;
	uint64_t total_buf_size = (1024*1024*1024*2UL);
	unsigned char* cur_gb_pm_buf = gb_pm_mmap + (1024*1024*3UL);

	while (true) {
		if (cur_offset >= total_buf_size) {
			break;
		}

		// In this phase, buffer pool is already allocated,
		// copy these frame to allocated frame of buf_block_t
		// buffer_pool_t -> buf_chunk_t -> buf_block_t -> frame
		// or---!!! just copy as bug_page_t (with UNIV_PAGE_SIZE)
		
		// TODO(jhpark): convert page size as constant variable
		byte* buf = (byte*) malloc(4096*1024); // 4KB page
		memcpy(buf, gb_pm_mmap + cur_offset, (4096*1024));
		// align page
		page_align(buf);

		// check page information
		// refer to btr0btr.ic
		// first check page_no and space_id
		
		unsigned long space_id = mach_read_from_4(buf + FIL_PAGE_SPACE_ID);
		unsigned long page_no = mach_read_from_4(buf + FIL_PAGE_OFFSET);  			
		
		if (space_id == 28 || space_id == 30) {
			//&& page_no == 0)) {
			// perform fil_io
			IORequest write_request(IORequest::WRITE);
			write_request.disable_compression(); // stil needed?

			// similar process, partila updates! 
			write_request.dblwr_recover();
			fprintf(stderr, "[JONGQ] perform fil_io write!!!\n");
			int check = 0;
			check = fil_io(write_request, true, page_id_t(space_id, page_no), 
					univ_page_size, 0 ,univ_page_size.physical(), (void*) buf, NULL);

			fprintf(stderr, "[JONGQ] fil_io check: %d!\n", check);
		}

		cur_offset += (4096*1024);
		free(buf);
	}

    // step2. call fil_io() function to flush current 
    // note that changes on these pages are not atomic 
    // they might have partial updates
}
