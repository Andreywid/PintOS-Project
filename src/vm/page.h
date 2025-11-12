#ifndef VM_PAGE_H
#define VM_PAGE_H

#define VM_BIN	0
#define VM_FILE	1
#define VM_ANON	2 

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <hash.h>
#include "filesys/file.h"
#include "threads/palloc.h"


struct vm_entry{
	
	struct hash_elem elem_hash;
	struct file *file;	

	uint8_t page_type;
	void *virtual_addr;
	
	bool can_write;
	bool load_flag;
	
	off_t offset;
	uint32_t data_size;
	uint32_t zero_size; 

	// Memory Mapped File
	struct list_elem elem_mmap;
	
	// Swapping
	size_t swap_slot;
};

// Frame structure
struct page{
	void *kaddr;
	struct thread *thread;
	struct vm_entry *vme;
	struct list_elem frame;
	bool pinned;
};

// Memory Mapped File structure
struct mmap_file{
	int mapid;
	struct file * file;
	struct list_elem elem;
	struct list vme_list;
};

// Virtual memory table functions
void vm_table_init (struct hash *vm);
void vm_table_destroy (struct hash *vm);
void vm_entry_free (struct hash_elem *e, void *aux);
bool vm_entry_insert (struct hash *vm, struct vm_entry *vme);
bool vm_entry_delete (struct hash *vm, struct vm_entry *vme);
struct vm_entry *vm_entry_find (void *virtual_addr);
bool vm_load_file_segment (void *kaddr, struct vm_entry *vme);

// Frame management functions
void frame_list_init (void);
void frame_track_insert (struct page *page);
void frame_track_remove (struct page *page);
struct page *frame_allocate_page (enum palloc_flags flags);
void frame_free_by_kaddr (void *kaddr);
void frame_release (struct page *page);
struct list_elem *frame_clock_next (void);
void frame_select_victim (void);
struct page *frame_lookup (void *kaddr);

#endif