#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

struct lock lock_file;
#define ARG1 (*(uint32_t *)(f->esp + 4))
#define ARG2 (*(uint32_t *)(f->esp + 8))
#define ARG3 (*(uint32_t *)(f->esp + 12))

static void syscall_handler (struct intr_frame *);
static void verify_addr (const void *addr);

void
syscall_init (void) 
{
  lock_init(&lock_file);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  switch (*(uint32_t *)(f->esp)) {
    
    // Processo/Execução
    case SYS_HALT: {
      shutdown_power_off();
      break;
    }

    case SYS_EXIT: {
      verify_addr(f->esp + 4);
      EXIT((int)ARG1);
      break;
    }

    case SYS_EXEC: {
      verify_addr(f->esp + 4);
      f->eax = process_execute((const char *)ARG1);
      break;
    }

    case SYS_WAIT: {
      verify_addr(f->esp + 4);
      f->eax = process_wait((tid_t)ARG1);
      break;
    }
    
    // Sistema de Arquivos
    case SYS_CREATE: {
      verify_addr(f->esp + 4);
      if (!(ARG1)) {
        EXIT(-1);
      }
      f->eax = filesys_create((const char *)ARG1, (unsigned)ARG2);
      break;
    }

    case SYS_REMOVE: {
      verify_addr(f->esp + 4);
      const char *file = (const char *)ARG1;
      if (!file) {
        EXIT(-1);
      }
      f->eax = filesys_remove(file);
      break;
    }

    case SYS_OPEN: {
      verify_addr(f->esp + 4);
      const char *file = (const char *)ARG1;
      if (!file) {
        EXIT(-1);
      }
      verify_addr(file);
    
      lock_acquire(&lock_file);
    
      struct file *opened = filesys_open(file);
      int res = -1;
    
      if (opened) {
        for (int i = 3; i < 128; i++) {
          if (!thread_current()->FD[i]) {
            if (!strcmp(thread_current()->name, file)) {
              file_deny_write(opened);
            }
            thread_current()->FD[i] = opened;
            res = i;
            break;
          }
        }
        // Se não achou slot de FD, evita vazamento.
        if (res == -1) {
          file_close(opened);
        }
      }
    
      lock_release(&lock_file);
      f->eax = res;
      break;
    }
    
    case SYS_FILESIZE: {
      verify_addr(f->esp + 4);
      int fd = (int)ARG1;
      if (!thread_current()->FD[fd]) {
        EXIT(-1);
      }
      if (!fd) {
        f->eax = -1;
        break;
      }
      f->eax = file_length(thread_current()->FD[fd]);
      break;
    }

    case SYS_READ: {
      verify_addr(f->esp + 4);
      int fd = (int)ARG1;
      void *buffer = (void *)ARG2;
      unsigned size = (unsigned)ARG3;

      if (!buffer) {
        EXIT(-1);
      }
      if (!thread_current()->FD[fd]) {
        EXIT(-1);
      }
      verify_addr(buffer);

      if (!fd) {
        lock_acquire(&lock_file);
        for (int i = 0; i < (int)size; i++) {
          *((uint8_t *)buffer + i) = input_getc();
        }
        lock_release(&lock_file);
        f->eax = (int)size;
      } else if (fd > 2) {
        lock_acquire(&lock_file);
        int res = file_read(thread_current()->FD[fd], buffer, size);
        lock_release(&lock_file);
        f->eax = res;
      } else {
        f->eax = -1;
      }
      break;
    }

    case SYS_WRITE: {
      int fd = (int)ARG1;
      const void *buffer = (const void *)ARG2;
      unsigned size = (unsigned)ARG3;

      if (!buffer) {
        EXIT(-1);
      }
      verify_addr(buffer);

      if (fd == 1) {
        lock_acquire(&lock_file);
        putbuf(buffer, size);
        lock_release(&lock_file);
        f->eax = (int)size;
      } else if (fd > 2) {
        if (!thread_current()->FD[fd]) {
          EXIT(-1);
        }
        lock_acquire(&lock_file);
        int res = file_write(thread_current()->FD[fd], buffer, size);
        lock_release(&lock_file);
        f->eax = res;
      } else {
        f->eax = -1;
      }
      break;
    }

    case SYS_SEEK: {
      verify_addr(f->esp + 4);
      int fd = (int)ARG1;
      unsigned pos = (unsigned)ARG2;
      if (!thread_current()->FD[fd]) {
        EXIT(-1);
      }
      file_seek(thread_current()->FD[fd], pos);
      break;
    }

    case SYS_TELL: {
      verify_addr(f->esp + 4);
      int fd = (int)ARG1;
      if (!thread_current()->FD[fd]) {
        EXIT(-1);
      }
      f->eax = file_tell(thread_current()->FD[fd]);
      break;
    }

    case SYS_CLOSE: {
      verify_addr(f->esp + 4);
      int fd = (int)ARG1;
      if (!thread_current()->FD[fd]) {
        EXIT(-1);
      }
      file_close(thread_current()->FD[fd]);
      thread_current()->FD[fd] = NULL;
      break;
    }

    default:
      return;
  }
}

void EXIT (int status) {
  printf("%s: exit(%d)\n", thread_current()->name, status);
  thread_current()->exit_status = status;
  for (int i = 3; i < 128; i++) {
    if (thread_current()->FD[i]) {
      file_close(thread_current()->FD[i]);
      thread_current()->FD[i] = NULL;
    }
  }
  thread_exit();
}

static void verify_addr (const void *addr) {
  if (addr == NULL || !is_user_vaddr(addr)) {
    EXIT(-1);
  }
}
