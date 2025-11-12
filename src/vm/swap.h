#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <bitmap.h>
#include <stddef.h>

#include "threads/synch.h"
#include "vm/page.h"

void swap_init (void);
void swap_fetch_page (void *kaddr, size_t swap_slot);
size_t swap_store_page (void *kaddr);

#endif