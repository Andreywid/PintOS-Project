#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include "vm/swap.h"
#include "lib/user/syscall.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
void setup_user_stack_args(char *argv[], void **esp, int argc);
extern struct lock frame_list_lock;

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  char* save_ptr;
  char* str_cmd = strtok_r(file_name, " ", &save_ptr);

  if (filesys_open(str_cmd)==NULL){
    palloc_free_page(fn_copy);
    return -1;
  }
  /* Create a new thread to execute FILE_NAME. */
  struct thread *cur = thread_current();
  tid = thread_create (str_cmd, PRI_DEFAULT, start_process, fn_copy);
  sema_down (& cur -> load_lock);

  if (tid == TID_ERROR){
    palloc_free_page (fn_copy); 
  }

  struct list_elem *e = NULL;
  struct thread *child_temp = NULL;

  for (e = list_begin(& (cur -> child_list)); e != list_end(&(cur->child_list)); e = list_next(e)){
	child_temp = list_entry (e, struct thread, child_elem);
	if (child_temp -> exit_status == -1 )
		return process_wait(tid);
  }
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;
  vm_table_init(&thread_current()->vm);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  char file_name_use[128];
  
  strlcpy(file_name_use, file_name, strlen(file_name)+1);
  int num_token = 0;
  char *token, *save_ptr;

  for (token = strtok_r (file_name_use, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr)){
	  num_token ++;
  }
  
  char **token_array = (char **)malloc(sizeof(char *)* num_token);
  strlcpy(file_name_use, file_name, strlen(file_name) + 1);
  int i = 0;
  for (token = strtok_r (file_name_use, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr)){
	  token_array[i] = token;
	  i++;
  }
  
  success = load (token_array[0], &if_.eip, &if_.esp);
  struct thread *cur = thread_current();
  if(success){
	  setup_user_stack_args(token_array, &if_.esp, num_token);
	  free(token_array);
  }

  palloc_free_page (file_name);
  sema_up (& cur->parent->load_lock);
  
  if (!success){
	  free(token_array);
	  EXIT(-1);
  }
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED) 
{
  //Implementação do process_wait onde espera o filho terminar
  // e retorna seu exit_status, ou -1 se inválido
  struct thread *cur = thread_current();
  struct thread *child = NULL;
  int exit_status = -1;
    
  struct list_elem *elem;
  // Procura o filho com o tid especificado
  for (elem = list_begin(&cur->child_list);
       elem != list_end(&cur->child_list);
       elem = list_next(elem)) {

    child = list_entry(elem, struct thread, child_elem);
    if (child->tid == child_tid) {
      // Espera o filho terminar
      sema_down(&(child->child_lock));
      // Pega o exit_status do filho
      exit_status = child->exit_status;
      // Remove o filho da lista de filhos
      list_remove(&(child->child_elem));
      // Libera o lock de memória do filho
      sema_up(&(child->mem_lock));
      break;
    }

  }
  return exit_status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  dir_close (cur->current_dir);

  struct list_elem * e;

  for(e = list_begin(&cur->mmap_list); e!= list_end(&cur->mmap_list); ){
	  struct mmap_file *mmap_f = list_entry(e, struct mmap_file, elem);
	  e = vm_perform_munmap(mmap_f);
  }
  vm_table_destroy(&cur->vm);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
    for (int i = 2; i < 128; i++) {
      if (cur->FD[i] != NULL){
        file_close(cur->FD[i]);
      } 
    }

    sema_up(&(cur->child_lock));
    sema_down(&(cur->mem_lock));
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();


  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  //file_close (file);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  //file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      struct vm_entry *vme;
      vme = (struct vm_entry *)malloc(sizeof(struct vm_entry));
      if (vme == NULL)
        return false;
      
      memset(vme, 0, sizeof(struct vm_entry));
      // Inicializa os campos do vm_entry
      vme->virtual_addr = upage;
      vme->page_type = VM_BIN;
      vme->file = file;
      vme->offset = ofs;
      vme->data_size = page_read_bytes;
      vme->zero_size = page_zero_bytes;
      vme->load_flag = 0;
      vme->can_write = writable;	
      
      vm_entry_insert(&thread_current()->vm, vme);

        /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      ofs += page_read_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  struct page *kpage;
  bool success = false;

  kpage = frame_allocate_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
  {
    success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage->kaddr, true);
    if (success){
      *esp = PHYS_BASE;
      struct vm_entry *vme = (struct vm_entry *)malloc(sizeof(struct vm_entry));

      memset(vme, 0, sizeof(struct vm_entry));
      vme->page_type = VM_ANON;
      vme->virtual_addr = ((uint8_t *) PHYS_BASE) - PGSIZE;
      vme->can_write = 1;	
      vme->load_flag = 1;
      vm_entry_insert(&thread_current()->vm, vme);
      kpage->vme = vme;
      kpage->pinned = 0;
    }
    else{
      frame_free_by_kaddr (kpage->kaddr);
      return false;
	  }
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

// Monta a pilha do processo com os argumentos
void setup_user_stack_args(char* argv[], void **esp, int argc) {
  int argv_addrs[128];
  int pad = 0;

  // Copia as strings para a pilha
  for (int i = argc - 1; i >= 0; i--) {
    int arg_len = (int)strlen(argv[i]) + 1;
    *esp -= arg_len;
    memcpy(*esp, argv[i], arg_len);
    pad += arg_len;
    argv_addrs[i] = (int)*esp;
  }

  // colocando para 4 bytes
  pad = (pad % 4 == 0) ? 0 : (4 - (pad % 4));
  *esp -= pad;
  memset(*esp, 0, pad);

  *esp -= sizeof(uint32_t);
  memset(*esp, 0, sizeof(uint32_t));

  // Empilha os ponteiros argv[i] na pilha
  for (int i = argc - 1; i >= 0; i--) {
    *esp -= sizeof(uint32_t);
    memcpy(*esp, &argv_addrs[i], sizeof(uint32_t));
  }
  // Empilha o ponteiro argv
  int argv_ptr = (int)*esp;
  *esp -= sizeof(uint32_t);
  memcpy(*esp, &argv_ptr, sizeof(uint32_t));

  // Empilha argc
  *esp -= sizeof(uint32_t);
  memcpy(*esp, &argc, sizeof(uint32_t));

  // Endereço de retorno “fake”
  *esp -= sizeof(uint32_t);
  memset(*esp, 0, sizeof(uint32_t));
}
// Resolve a page fault a partir do vm_entry
bool vm_resolve_fault(struct vm_entry *vme){
  struct page *frame = frame_allocate_page(PAL_USER);
  if (frame == NULL)
    return false;

  frame->vme = vme;
  bool ok = false;
  // Carrega a página conforme o tipo da pagina
  switch (vme->page_type){
    case VM_BIN:
    case VM_FILE:
      if (vm_load_file_segment(frame->kaddr, vme) &&
          install_page(vme->virtual_addr, frame->kaddr, vme->can_write)){
        ok = true;
      }
      break;
    case VM_ANON:
      swap_fetch_page(frame->kaddr, vme->swap_slot);
      ok = install_page(vme->virtual_addr, frame->kaddr, vme->can_write);
      break;
    default:
      ok = false;
      break;
  }
  // Marca a página como carregada se sucesso
  if (ok){
    vme->load_flag = 1;
  } else {
    frame_free_by_kaddr(frame->kaddr);
  }
  return ok;
}

// Desmapeia o arquivo mapeado na memória
struct list_elem *vm_perform_munmap(struct mmap_file * mmap_file){
  uint32_t *pagedir = thread_current()->pagedir;
  // Percorre todos os vm_entry associados ao mmap_file
  struct list_elem *elem = list_begin(&mmap_file->vme_list);
  while (elem != list_end(&mmap_file->vme_list)){
    struct vm_entry *vme = list_entry(elem, struct vm_entry, elem_mmap);
    struct list_elem *next = list_next(elem);
    // Se a página foi carregada na memória
    if (vme->load_flag){
      void *kaddr = pagedir_get_page(pagedir, vme->virtual_addr);
      if (pagedir_is_dirty(pagedir, vme->virtual_addr) && kaddr != NULL)
        {
          lock_acquire (&filesys_lock);
          file_write_at(vme->file, kaddr, vme->data_size, vme->offset);
          lock_release (&filesys_lock);
        }
      if (kaddr != NULL)
        frame_free_by_kaddr(kaddr);
      // Remove o mapeamento da tabela de páginas
      pagedir_clear_page(pagedir, vme->virtual_addr);
    }

    list_remove(elem);
    vm_entry_delete(&thread_current()->vm, vme);
    free(vme);
    elem = next;
  }

  struct list_elem *after = list_remove(&mmap_file->elem);
  file_close(mmap_file->file);
  free(mmap_file);
  return after;
}

// Cresce a pilha do processo para o endereço addr
bool vm_grow_stack (void *addr){
  // Aloca um frame de página zero preenchido
  struct page *frame = frame_allocate_page(PAL_USER | PAL_ZERO);
  if (frame == NULL)
    return false;

  void *upage = pg_round_down(addr);
  struct vm_entry *vme = (struct vm_entry *)malloc(sizeof(struct vm_entry));
  if (vme == NULL){
    frame_free_by_kaddr(frame->kaddr);
    return false;
  }
  memset(vme, 0, sizeof *vme);
  vme->virtual_addr = upage;
  vme->page_type = VM_ANON;
  vme->can_write = 1;
  vme->load_flag = 1;

  bool ok = install_page(upage, frame->kaddr, true);
  if (!ok){
    free(vme);
    frame_free_by_kaddr(frame->kaddr);
    return false;
  }
  // Insere o vm_entry na tabela de páginas do processo
  vm_entry_insert(&thread_current()->vm, vme);
  frame->vme = vme;
  return true;
}