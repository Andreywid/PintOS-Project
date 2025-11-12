#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "vm/page.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

bool vm_resolve_fault(struct vm_entry *vme);
bool vm_grow_stack(void *addr);
struct list_elem *vm_perform_munmap(struct mmap_file *mmap_file); 

#endif /* userprog/process.h */
