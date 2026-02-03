#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include <stdlib.h>
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/buffer_cache.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "lib/string.h"

#define PATH_MAX 256

/* Partition that contains the file system. */
struct block *fs_device;
struct hash dentry_cache;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  bc_init();
  inode_init ();
  free_map_init ();

  
  if (format) 
    do_format ();

  free_map_open ();
  thread_current ()->current_dir = dir_open_root ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  bc_term();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;

  char file_name [NAME_MAX + 1];
  struct dir *dir = parse_path(name, file_name);
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, 0)
                  && dir_add (dir, file_name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  char file_name[NAME_MAX + 1];
  struct inode *inode = NULL;

  struct dir *dir = parse_path (name, file_name);
  if (dir == NULL)
    return NULL;

  if (!dir_lookup (dir, file_name, &inode))
    {
      dir_close (dir);
      return NULL;
    }

  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  bool success = false;
  char file_name[NAME_MAX + 1];
  struct inode *inode;
  struct dir *current_dir = NULL;

  // Resolve o caminho e obtém o diretório pai + último componente
  struct dir *dir = parse_path (name, file_name);
  if (dir == NULL){
    return success;
  }

  if (!dir_lookup (dir, file_name, &inode)){
    dir_close(dir);
    return success;
  }

  if (inode_is_dir(inode)){
    
    struct dir *t_dir = thread_current()->current_dir;
    struct inode *inode_ = dir_get_inode(t_dir);

    if(inode_ == inode){
      dir_close(dir);
      return success;
    }
    
    int cnt = inode_open_cnt(inode);
    // Não remove diretório ainda referenciado por mais de um descritor
    if (cnt > 1){
      dir_close(dir);
      return success;
    }
    current_dir = dir_open(inode);
    if (current_dir == NULL){
      dir_close(dir);
      return success;
    }

    if (dir_is_empty (current_dir)){
      success = dir_remove (dir, file_name);
    }
    dir_close (current_dir);
  }

  else{ 
    if (dir_remove (dir, file_name))
      success = true;
  }

  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  
  struct dir *root_dir = dir_open_root();
  // Criando as entradas "." e ".." no diretório raiz
  dir_add(root_dir, ".", ROOT_DIR_SECTOR);
  dir_add(root_dir, "..", ROOT_DIR_SECTOR);

  dir_close (root_dir);

  free_map_close ();
  printf ("done.\n");
}

// Resolve um caminho em diretório base e último componente (file_name)
struct dir *parse_path (const char *path_name, char *file_name){
  struct dir *dir_handle;

  if (path_name == NULL || file_name == NULL) {
    return NULL;
  }

  if (strlen (path_name) == 0) {
    return NULL;
  }

  char path_buf[PATH_MAX + 1];
  strlcpy (path_buf, path_name, sizeof path_buf);

  if (path_buf[0] == '/') {
    dir_handle = dir_open_root ();
  } else {
    dir_handle = dir_reopen (thread_current ()->current_dir);
  }

  char *current, *next, *save_ptr;

  current = strtok_r (path_buf, "/", &save_ptr);
  next = strtok_r (NULL, "/", &save_ptr);

  if (current != NULL && next == NULL && strlen (current) > NAME_MAX) {
    dir_close (dir_handle);
    return NULL;
  }

  while (current != NULL && next != NULL) {
    struct inode *inode = NULL;

    if (strlen (current) > NAME_MAX || strlen (next) > NAME_MAX) {
      dir_close (dir_handle);
      return NULL;
    }

    if (!dir_lookup (dir_handle, current, &inode)) {
      dir_close (dir_handle);
      return NULL;
    }

    dir_close (dir_handle);
    if (!inode_is_dir (inode)) {
      return NULL;
    }

    dir_handle = dir_open (inode);
    current = next;
    next = strtok_r (NULL, "/", &save_ptr);
  }
  
  if (current == NULL) {
    strlcpy (file_name, ".", 2);
  } else {
    strlcpy (file_name, current, NAME_MAX + 1);
    if (strlen (current) > NAME_MAX) {
      dir_close (dir_handle);
      return NULL;
    }
  }

  return dir_handle;
}


// Altera o diretório de trabalho atual para o diretório indicado em dir
bool filesys_change_dir (const char *dir)
{
  if (strlen (dir) == 0) {
    return false;
  }

  char path_buf[PATH_MAX + 1];
  strlcpy (path_buf, dir, sizeof path_buf);

  char leaf[NAME_MAX + 1];
  bool success = false;
  struct inode *inode = NULL;
  struct dir *base_dir = NULL;
  struct dir *new_dir = NULL;
  
  base_dir = parse_path (path_buf, leaf);
  if (base_dir == NULL) {
    return success;
  }
  
  if (!dir_lookup (base_dir, leaf, &inode)) {
    dir_close (base_dir);
    return success;
  }

  dir_close (base_dir);

  new_dir = dir_open (inode);
  if (thread_current ()->current_dir == NULL) {
    thread_current ()->current_dir = new_dir;
  } else {
    dir_close (thread_current ()->current_dir);
    thread_current ()->current_dir = new_dir;
  }

  success = true;
  return success; 
}


// Cria um novo diretório no caminho indicado por name
bool filesys_create_dir (const char *name)
{
  block_sector_t new_dir_sector = 0;

  char leaf_name[NAME_MAX + 1];
  struct dir *parent_dir = parse_path (name, leaf_name);

  bool success = (parent_dir != NULL
                  && free_map_allocate (1, &new_dir_sector)
                  && dir_create (new_dir_sector, 16)
                  && dir_add (parent_dir, leaf_name, new_dir_sector));
  if (!success && new_dir_sector != 0) { 
    free_map_release (new_dir_sector, 1);
  }

  if (success) {
    struct inode *child_inode = inode_open (new_dir_sector);
    struct dir *child_dir = dir_open (child_inode);

    dir_add (child_dir, ".", new_dir_sector);

    struct inode *parent_inode = dir_get_inode (parent_dir);
    block_sector_t parent_sector = inode_get_inumber (parent_inode);

    dir_add (child_dir, "..", parent_sector);
    dir_close (child_dir);
  }

  dir_close (parent_dir);
  return success;
}


// Inicializa a hash de dentry cache
void dentry_init (struct hash *dentry_cache){
  hash_init (dentry_cache, dentry_hash_function, dentry_less_function, NULL); 
}

// Destroi a hash de dentry cache, chamando o destrutor dos elementos
void dentry_destroy (struct hash *dentry_cache){
  hash_destroy (dentry_cache, dentry_destructor);
}

// Destrutor de uma entrada de dentry cache
void dentry_destructor (struct hash_elem *e, void *aux UNUSED){
  struct dc_entry *entry = hash_entry (e, struct dc_entry, elem);
  free (entry);
}

// Função de hash para entradas de dentry cache, baseada em path
static unsigned dentry_hash_function (const struct hash_elem *e, void *aux UNUSED){
  struct dc_entry *entry = hash_entry (e, struct dc_entry, elem);
  return hash_string (entry->full_path);
}

// Compara duas entradas de dentry cache lexicograficamente pelo path
static bool dentry_less_function (const struct hash_elem *e_a, const struct hash_elem *e_b, void *aux UNUSED){
  struct dc_entry *left_entry;
  struct dc_entry *right_entry;
	
  left_entry = hash_entry (e_a, struct dc_entry, elem);
  right_entry = hash_entry (e_b, struct dc_entry, elem);

  char *left_path = left_entry->full_path;
  char *right_path = right_entry->full_path;

  if (strcmp (left_path, right_path) < 0) {
    return true;
  } else {
    return false;
  }
}

// Insere uma entrada no dentry cache, retornando true em caso de inserção nova
bool dentry_insertion (struct hash *dentry_cache, struct dc_entry *entry){
  struct hash_elem *elem = &entry->elem;	
  struct hash_elem *old = hash_insert (dentry_cache, elem);
  return (old == NULL);
}
 
// Remove uma entrada específica do dentry cache, liberando sua memória
bool dentry_deletion (struct hash *dentry_cache, struct dc_entry *entry){
  struct hash_elem *elem = &entry->elem;

  struct hash_elem *found = hash_delete (dentry_cache, elem);
  if (found != NULL) {
    free (entry->full_path);
    free (entry);
  }
  return (found != NULL);
}

// Procura uma entrada de dentry cache pelo caminho completo
struct dc_entry *dentry_search (const char *path){
  struct dc_entry probe;

  probe.full_path = (char *) path;
  struct hash_elem *e = hash_find (&dentry_cache, &probe.elem);   

  if (!e) {
    return NULL;
  }

  return hash_entry (e, struct dc_entry, elem);
}

// Procura a entrada de dentry cache correspondente ao diretório pai de path
struct dc_entry *dentry_parent_search (const char *path){
  int i;
  int path_length;
  struct dc_entry probe;
  
  path_length = strlen (path);

  char *path_copy = malloc (path_length + 1);
  

  strlcpy (path_copy, path, path_length + 1);
  
  for (i = path_length; i >= 0; i--) {
    if (path[i] == '/') {
      break;
    }
  }
  char *parent_path = malloc (i + 1);
  strlcpy (parent_path, path_copy, i + 1);

  probe.full_path = parent_path;
  struct hash_elem *e = hash_find (&dentry_cache, &probe.elem);   

  if (!e) {
    free (path_copy);
    free (parent_path);
    return NULL;
  }
  free (path_copy);
  free (parent_path);

  return hash_entry (e, struct dc_entry, elem);
}
