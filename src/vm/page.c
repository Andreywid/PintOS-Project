#include "vm/page.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include "threads/palloc.h"
#include <string.h>
#include "vm/swap.h"
#include <hash.h>
#include <debug.h>
#include <stdio.h>

static unsigned vm_entry_hash (const struct hash_elem *, void *aux);
static bool vm_entry_less (const struct hash_elem *, const struct hash_elem *, void *aux);

extern struct lock filesys_lock;

// Inicializa a tabela de páginas virtuais do thread atual
void vm_table_init (struct hash *vm){
	ASSERT (vm != NULL);
	hash_init (vm, vm_entry_hash, vm_entry_less, NULL);
}

// Destrói a tabela de páginas virtuais do thread atual
void vm_table_destroy (struct hash *vm){
	if (vm == NULL)
		return;

	hash_destroy (vm, vm_entry_free);
}

// Libera a memória associada a um vm_entry
void vm_entry_free (struct hash_elem *e, void *aux UNUSED){
	struct vm_entry *victim = hash_entry (e, struct vm_entry, elem_hash);
	free (victim);
}

// Funções de hash para vm_entry
static unsigned vm_entry_hash (const struct hash_elem *e, void *aux UNUSED){
	const struct vm_entry *vme = hash_entry (e, struct vm_entry, elem_hash);
	return hash_bytes (&vme->virtual_addr, sizeof vme->virtual_addr);
}

// Compara dois vm_entry pelo endereço virtual
static bool vm_entry_less (const struct hash_elem *e_a, const struct hash_elem *e_b, void *aux UNUSED){
	const struct vm_entry *left = hash_entry (e_a, struct vm_entry, elem_hash);
	const struct vm_entry *right = hash_entry (e_b, struct vm_entry, elem_hash);
	return left->virtual_addr < right->virtual_addr;
}

// Insere um vm_entry na tabela de páginas virtuais
bool vm_entry_insert (struct hash *vm, struct vm_entry *vme){
	ASSERT (vm != NULL);
	ASSERT (vme != NULL);
	return hash_insert (vm, &vme->elem_hash) == NULL;
}
 
// Remove um vm_entry da tabela de páginas virtuais
bool vm_entry_delete (struct hash *vm, struct vm_entry *vme){
	ASSERT (vm != NULL);
	ASSERT (vme != NULL);
	return hash_delete (vm, &vme->elem_hash) != NULL;
}

// Procura um vm_entry na tabela de páginas virtuais pelo endereço virtual
struct vm_entry *vm_entry_find (void *virtual_addr){
	struct thread *cur = thread_current ();
	struct hash *vm = &cur->vm;
	struct vm_entry probe;

	probe.virtual_addr = pg_round_down (virtual_addr);

	struct hash_elem *found = hash_find (vm, &probe.elem_hash);
	return found != NULL ? hash_entry (found, struct vm_entry, elem_hash) : NULL;
}

// Carrega um segmento de arquivo na memória conforme descrito pelo vm_entry
bool vm_load_file_segment (void *kaddr, struct vm_entry *vme){
	ASSERT (vme != NULL);
	const size_t to_read = vme->data_size;
	const bool already_locked = lock_held_by_current_thread (&filesys_lock);

	if (!already_locked)
		lock_acquire (&filesys_lock);

	const int bytes_read = file_read_at (vme->file, kaddr, to_read, vme->offset);
	if (bytes_read != (int) to_read){
		if (!already_locked)
			lock_release (&filesys_lock);
		return false;
	}

	memset (kaddr + to_read, 0, vme->zero_size);

	if (!already_locked)
		lock_release (&filesys_lock);

	vme->load_flag = true;
	return true;
}

// Gerenciamento de frames de página
struct list frame_pool;
struct lock frame_pool_lock;
struct list_elem *frame_clock_cursor;	

// Inicializa o pool de frames de página
void frame_list_init (void){
	list_init (&frame_pool);
	lock_init (&frame_pool_lock);
	frame_clock_cursor = NULL;
}

// Insere um frame na lista de frames
void frame_track_insert (struct page *page){
	ASSERT (page != NULL);
	list_push_back (&frame_pool, &page->frame);
}

// Remove um frame da lista de frames
void frame_track_remove (struct page *page){
	ASSERT (page != NULL);
	struct list_elem *next = list_remove (&page->frame);
	if (&page->frame == frame_clock_cursor)
		frame_clock_cursor = next;
}

// Aloca um frame de página, selecionando uma "vítima" se necessário
struct page *frame_allocate_page (enum palloc_flags flags){
	lock_acquire (&frame_pool_lock);
	void *kaddr;
	while ((kaddr = palloc_get_page (flags)) == NULL)
		frame_select_victim ();

	struct page *frame = malloc (sizeof *frame);
	memset (frame, 0, sizeof *frame);
	frame->thread = thread_current ();
	frame->kaddr = kaddr;
	frame->vme = NULL;
	frame->pinned = false;
	frame_track_insert (frame);
	lock_release (&frame_pool_lock);
	return frame;
}

// Libera um frame de página pelo endereço kernel
void frame_free_by_kaddr (void *kaddr){
	struct page *victim = NULL;
	lock_acquire (&frame_pool_lock);
	for (struct list_elem *e = list_begin (&frame_pool);
	     e != list_end (&frame_pool);
	     e = list_next (e)){
		struct page *candidate = list_entry (e, struct page, frame);
		if (candidate->kaddr == kaddr){
			victim = candidate;
			break;
		}
	}
	if (victim != NULL)
		frame_release (victim);
	lock_release (&frame_pool_lock);
}

// Libera um frame de página
void frame_release (struct page *page){
	frame_track_remove (page);
	pagedir_clear_page (page->thread->pagedir, pg_round_down (page->vme->virtual_addr));
	palloc_free_page (page->kaddr);
	free (page);
}

// Retorna o próximo elemento na varredura circular da lista de frames
struct list_elem *frame_clock_next (void){
	if (list_empty (&frame_pool))
		return NULL;
	if (frame_clock_cursor == NULL || frame_clock_cursor == list_end (&frame_pool))
		return list_begin (&frame_pool);
	struct list_elem *next = list_next (frame_clock_cursor);
	return next == list_end (&frame_pool) ? list_begin (&frame_pool) : next;
}

// Seleciona e desaloca uma vítima
void frame_select_victim (void){
	frame_clock_cursor = frame_clock_next ();
	ASSERT (frame_clock_cursor != NULL);
	struct page *page = list_entry (frame_clock_cursor, struct page, frame);

	for (;;){
		if (page->vme->virtual_addr <= PHYS_BASE){
			if (!pagedir_is_accessed (page->thread->pagedir, page->vme->virtual_addr))
				break;
			pagedir_set_accessed (page->thread->pagedir, page->vme->virtual_addr, false);
		}
		frame_clock_cursor = frame_clock_next ();
		page = list_entry (frame_clock_cursor, struct page, frame);
	}

	switch (page->vme->page_type){
	case VM_BIN:
		if (pagedir_is_dirty (page->thread->pagedir, page->vme->virtual_addr)){
			page->vme->swap_slot = swap_store_page (page->kaddr);
			page->vme->page_type = VM_ANON;
		}
		break;
	case VM_FILE:
		if (pagedir_is_dirty (page->thread->pagedir, page->vme->virtual_addr))
			file_write_at (page->vme->file, page->vme->virtual_addr,
			              page->vme->data_size, page->vme->offset);
		break;
	case VM_ANON:
		page->vme->swap_slot = swap_store_page (page->kaddr);
		break;
	}
	page->vme->load_flag = false;

	frame_release (page);
}

// Procura um frame pelo endereço kernel
struct page *frame_lookup (void *kaddr){
	for (struct list_elem *e = list_begin (&frame_pool);
	     e != list_end (&frame_pool);
	     e = list_next (e)){
		struct page *page = list_entry (e, struct page, frame);
		if (page->kaddr == kaddr)
			return page;
	}
	return NULL;
}
