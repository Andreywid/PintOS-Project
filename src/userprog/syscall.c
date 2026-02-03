#include "userprog/syscall.h"
#include <stdio.h>
#include "userprog/process.h"
#include <syscall-nr.h>
#include <string.h>
#include "devices/shutdown.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/synch.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/input.h"
#include "userprog/pagedir.h"

extern struct lock frame_pool_lock;
struct lock filesys_lock; // Lock para operações do sistema de arquivos
#define ARG1 (*(uint32_t *)(f->esp + 4))
#define ARG2 (*(uint32_t *)(f->esp + 8))
#define ARG3 (*(uint32_t *)(f->esp + 12))

static void syscall_handler (struct intr_frame *);
static void verify_addr (const void *addr);
void pin_user_buffer (void *offset, int size, bool for_write);
void unpin_user_buffer (void *offset, int size);

void
syscall_init (void) 
{
  lock_init(&filesys_lock);
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
      char file_name[128];
      memcpy(file_name, (const char *)ARG1, strlen((const char *)ARG1)+1);
      pid_t pid = process_execute(file_name);
	  f->eax = pid;
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
    
      lock_acquire(&filesys_lock);
    
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
    
      lock_release(&filesys_lock);
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
      verify_addr (f->esp+4);
      verify_addr (f->esp+8);
      verify_addr (f->esp+12);
      f->eax = read((int)ARG1, (void *)ARG2, (unsigned)ARG3);
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
      void *page_base = pg_round_down ((void *)buffer);
      pin_user_buffer (page_base, size, false);

      if (fd == 1) {
        lock_acquire(&filesys_lock);
        putbuf(buffer, size);
        lock_release(&filesys_lock);
        f->eax = (int)size;
      } else if (fd > 2) {
        if (!thread_current()->FD[fd]) {
          EXIT(-1);
        }
        lock_acquire(&filesys_lock);
        int res = file_write(thread_current()->FD[fd], buffer, size);
        lock_release(&filesys_lock);
        f->eax = res;
      } else {
        f->eax = -1;
      }
      unpin_user_buffer (page_base, size);
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

    case SYS_MMAP: {
      verify_addr(f->esp + 4);
      verify_addr(f->esp + 8);
      f->eax = mmap((int)ARG1, (void *)ARG2);
      break;
    }
    
    case SYS_MUNMAP: {
      verify_addr(f->esp + 4);
      munmap((int)ARG1);
      break;
    }
    	
    case SYS_ISDIR: {
      verify_addr (f->esp+4);
      f->eax = isdir((int)ARG1);
      break;
    }

    case SYS_CHDIR: {
      verify_addr(f->esp+4);
      f->eax = chdir((const char *)ARG1);
      break;
    }

    case SYS_MKDIR: {
      verify_addr(f->esp+4);
      f->eax = mkdir((const char *)ARG1);
      break;
    }

    case SYS_READDIR: {
      verify_addr(f->esp+4);
      verify_addr(f->esp+8);
      f->eax = readdir((int)ARG1, (char*)ARG2);
      break;
    }

    case SYS_INUMBER: {
      verify_addr(f->esp+4);
      f->eax = inumber((int)ARG1);
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
 
int read (int fd, void *buffer, unsigned size)
{
  verify_addr (buffer);

  uint8_t *page_base = pg_round_down (buffer);
  struct thread *cur = thread_current ();
  int result = -1;

  pin_user_buffer (page_base, size, true);
  lock_acquire (&filesys_lock);

  const int fd_limit = 128;

  if (fd == 0)
    {
      for (unsigned i = 0; i < size; i++)
        ((uint8_t *) buffer)[i] = input_getc ();
      result = (int) size;
    }
  else if (fd >= 0 && fd < fd_limit)
    {
      struct file *opened = cur->FD[fd];
      if (opened != NULL)
        result = file_read (opened, buffer, size);
    }

  lock_release (&filesys_lock);
  unpin_user_buffer (page_base, size);
  return result;
}
// Mapeia o arquivo identificado por fd no endereço addr
int mmap (int fd, void *addr)
{
  struct thread *cur = thread_current ();

  if (addr == NULL || pg_ofs (addr) != 0 || fd <= 1)
    return -1;

  struct file *origin = cur->FD[fd];
  if (origin == NULL)
    return -1;

  struct file *file = file_reopen (origin);
  if (file == NULL)
    return -1;

  off_t remaining = file_length (file);
  if (remaining == 0)
    {
      lock_acquire (&filesys_lock);
      file_close (file);
      lock_release (&filesys_lock);
      return -1;
    }

  struct mmap_file *mapping = malloc (sizeof *mapping);
  if (mapping == NULL)
    {
      lock_acquire (&filesys_lock);
      file_close (file);
      lock_release (&filesys_lock);
      return -1;
    }

  memset (mapping, 0, sizeof *mapping);
  mapping->file = file;
  mapping->mapid = cur->next_mapid++;
  list_init (&mapping->vme_list);
  list_push_back (&cur->mmap_list, &mapping->elem);

  uint8_t *upage = addr;
  off_t offset = 0;
  while (remaining > 0)
    {
      size_t to_read = remaining < PGSIZE ? remaining : PGSIZE;
      size_t to_zero = PGSIZE - to_read;

      if (vm_entry_find (upage) != NULL)
        {
          vm_perform_munmap (mapping);
          return -1;
        }

      struct vm_entry *entry = malloc (sizeof *entry);
      if (entry == NULL)
        {
          vm_perform_munmap (mapping);
          return -1;
        }

      memset (entry, 0, sizeof *entry);
      entry->virtual_addr = upage;
      entry->page_type = VM_FILE;
      entry->file = file;
      entry->offset = offset;
      entry->data_size = to_read;
      entry->zero_size = to_zero;
      entry->load_flag = 0;
      entry->can_write = 1;

      vm_entry_insert (&cur->vm, entry);
      list_push_back (&mapping->vme_list, &entry->elem_mmap);

      remaining -= to_read;
      offset += to_read;
      upage += PGSIZE;
    }

  return mapping->mapid;
}
// Desmapeia o mapeamento identificado por mapid
void munmap (int mapid)
{
  struct thread *cur = thread_current ();

  if (list_empty (&cur->mmap_list))
    return;

  struct list_elem *e = list_begin (&cur->mmap_list);
  while (e != list_end (&cur->mmap_list))
    {
      struct mmap_file *mapping = list_entry (e, struct mmap_file, elem);
      if (mapping->mapid != mapid)
        {
          e = list_next (e);
          continue;
        }

      e = vm_perform_munmap (mapping);
    }
}
// Marca as páginas do buffer como presas na memória
void pin_user_buffer (void *offset, int size, bool for_write)
{
  if (size <= 0)
    return;

  struct thread *cur = thread_current ();
  uint8_t *start = offset;
  uint8_t *limit = start + size;
  // Percorre todas as páginas do buffer
  for (; start < limit; start += PGSIZE)
    {
      struct vm_entry *entry = vm_entry_find (start);
      if (entry == NULL)
      {
        // Sem vm_entry: deve haver página mapeada presente
        void *kaddr0 = pagedir_get_page (cur->pagedir, start);
        if (kaddr0 == NULL)
          EXIT (-1);
        if (for_write && !pagedir_is_writable (cur->pagedir, start))
          EXIT (-1);
        // Página já residente: marque como pinned
        lock_acquire (&frame_pool_lock);
        struct page *frame0 = frame_lookup (kaddr0);
        if (frame0){
          frame0->pinned = true;
        }
        lock_release (&frame_pool_lock);
        continue;
      }
      if (for_write && !entry->can_write){
        EXIT (-1);
      }
      if (!entry->load_flag && !vm_resolve_fault (entry))
      {
        EXIT (-1);
      }

      lock_acquire (&frame_pool_lock);
      void *kaddr = pagedir_get_page (cur->pagedir, start);
      if (kaddr == NULL) {
        lock_release (&frame_pool_lock);
        EXIT (-1);
      }
      struct page *frame = frame_lookup (kaddr);
      frame->pinned = true;
      lock_release (&frame_pool_lock);
    }
}
// Desmarca as páginas do buffer como presas na memória
void unpin_user_buffer (void *offset, int size)
{
  if (size <= 0)
    return;

  struct thread *cur = thread_current ();
  uint8_t *start = offset;
  uint8_t *limit = start + size;
  // Percorre todas as páginas do buffer
  for (; start < limit; start += PGSIZE)
    {
      struct vm_entry *entry = vm_entry_find (start);
      if (entry == NULL) {
        void *kaddr0 = pagedir_get_page (cur->pagedir, start);
        if (kaddr0 == NULL){
          continue;
        }
        lock_acquire (&frame_pool_lock);
        struct page *frame0 = frame_lookup (kaddr0);
        if (frame0){
          frame0->pinned = false;
        }
        lock_release (&frame_pool_lock);
        continue;
      }
      if (!entry->load_flag){
        continue;
      }

      lock_acquire (&frame_pool_lock);
      void *kaddr = pagedir_get_page (cur->pagedir, start);
      if (kaddr){
          struct page *frame = frame_lookup (kaddr);
          if (frame){
            frame->pinned = false;
          }
      }
      lock_release (&frame_pool_lock);
    }
}

// Verifica se o descritor de arquivo fd é um diretório
bool isdir (int fd)
{
	struct file *file;
  struct inode *inode;
	bool isdir;
  
  	file = thread_current()->FD[fd];
  	if (file == NULL)
    	EXIT (-1);

  	inode = file_get_inode (file);
  	isdir = inode_is_dir (inode);
  	return isdir;
}

// Muda o diretório atual para o especificado em path_o
bool chdir (const char *path_o)
{
  if (path_o == NULL)
    return false;
  return filesys_change_dir(path_o);
}

// Cria um novo diretório no caminho especificado
bool mkdir(const char *dir){
	return filesys_create_dir(dir);
}

bool readdir(int fd, char name[READDIR_MAX_LEN + 1]){
  // Lê entradas de diretório preservando a posição via file->pos
  struct file *file = thread_current()->FD[fd];
  if (file == NULL)
    EXIT (-1);

  struct inode *inode = file_get_inode(file);
  if (!inode_is_dir(inode))
    return false;

  // Usa a posição de arquivo para acompanhar o progresso no diretório
  off_t pos = file_tell(file);
  bool ok = dir_readdir_at(inode, &pos, name);
  if (ok)
    file_seek(file, pos);
  return ok;
}

// Retorna o número do inode do arquivo associado ao descritor de arquivo fd
int inumber(int fd){

	struct file *file;
  	struct inode *inode;
  
    file = thread_current()->FD[fd];
    if (file == NULL)
      	EXIT (-1);

  	inode = file_get_inode (file);

   	int inum = inode_get_inumber(inode);
   	return inum;
}