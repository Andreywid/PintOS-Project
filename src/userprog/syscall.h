#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "vm/page.h"
#include "lib/user/syscall.h"
void syscall_init (void);

void EXIT (int status);
extern struct lock filesys_lock;
#endif /* userprog/syscall.h */
