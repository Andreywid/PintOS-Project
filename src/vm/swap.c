#include "vm/swap.h"

#include "devices/block.h"
#include "threads/vaddr.h"
#include "threads/synch.h"

#include <bitmap.h>
#include <stdint.h>
#include <stdlib.h>


static struct lock swap_guard;
static struct block *swap_area;
static struct bitmap *swap_usage;

// Calcula o número de setores por página
static inline size_t page_sector_count (void)
{
  return PGSIZE / BLOCK_SECTOR_SIZE;
}

// Garante que o dispositivo de swap está inicializado
static inline struct block *ensure_swap_device (void)
{
  if (swap_area == NULL)
    swap_area = block_get_role (BLOCK_SWAP);
  return swap_area;
}

// Inicializa o sistema de swap
void swap_init(){
	swap_area = block_get_role(BLOCK_SWAP);
	swap_usage = bitmap_create (block_size (swap_area) / page_sector_count());
	lock_init(&swap_guard);
}

// Recupera uma página do swap para o endereço kaddr
void swap_fetch_page (void *kaddr, size_t swap_slot){

	uint8_t *page_buf = kaddr;
	const size_t sector_count = page_sector_count();
	const size_t first_sector = swap_slot * sector_count;

	ensure_swap_device();
	lock_acquire(&swap_guard);

	if (!bitmap_test(swap_usage, swap_slot)){
		lock_release(&swap_guard);
		EXIT(-1);	
	}

	for (size_t sector = 0; sector < sector_count; ++sector){
		block_read(swap_area, first_sector + sector, page_buf + sector * BLOCK_SECTOR_SIZE);
	}

	bitmap_reset(swap_usage, swap_slot);
	lock_release(&swap_guard);
}

// Armazena uma página no swap e retorna o slot utilizado
size_t swap_store_page (void *kaddr){

	const uint8_t *page_buf = kaddr;
	const size_t sector_count = page_sector_count();

	ensure_swap_device();
	lock_acquire (&swap_guard);
	size_t swap_slot = bitmap_scan_and_flip(swap_usage, 0, 1, false);
	if (swap_slot == BITMAP_ERROR){
		lock_release(&swap_guard);
		EXIT(-1);
	}

	const size_t first_sector = swap_slot * sector_count;
	for (size_t sector = 0; sector < sector_count; ++sector){
		block_write(swap_area, first_sector + sector, page_buf + sector * BLOCK_SECTOR_SIZE);
	}

	lock_release(&swap_guard);
	return swap_slot;
}