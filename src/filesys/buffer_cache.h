#ifndef FILESYS_BUFFER_CACHE_H
#define FILESYS_BUFFER_CACHE_H

#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include <stdlib.h>

struct buffer_head{
	
	struct inode* inode;
	struct lock bc_lock;

	bool dirty;
	bool usage;
	bool clock;
	
	block_sector_t sector;
	void* data;
	
};

bool bc_read (block_sector_t sector_idx, void *buffer, off_t bytes_read, int chunk_size, int sector_ofs);
bool bc_write (block_sector_t sector_idx, void *buffer, off_t bytes_written, int chunck_size, int sector_ofs);
void bc_flush_entry (struct buffer_head *p_flush_entry);
void bc_init (void);
void bc_term (void);
void bc_flush_all_entries (void);
struct buffer_head *bc_select_victim (void);
struct buffer_head *bc_lookup (block_sector_t sector);



#endif