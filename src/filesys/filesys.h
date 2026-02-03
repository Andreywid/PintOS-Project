#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"
#include "devices/block.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

extern struct hash dentry_cache;

struct dc_entry {
    char *full_path;             /* caminho absoluto ou relativo normalizado */
    block_sector_t inode_sector; /* setor do inode associado */
    struct hash_elem elem;       /* nó na tabela hash */
};


/* Block device that contains the file system. */
extern struct block *fs_device;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);

struct dir *parse_path(const char *path_name, char *file_name);
bool filesys_change_dir(const char *dir);
bool filesys_create_dir(const char *dir);

void dentry_init (struct hash *dentry_cache);
void dentry_destroy (struct hash *dentry_cache);
void dentry_destructor (struct hash_elem *e, void *aux);
static unsigned dentry_hash_function (const struct hash_elem *e, void *aux);
static bool dentry_less_function (const struct hash_elem *e_a, const struct hash_elem *e_b, void *aux);
bool dentry_insertion (struct hash *dentry_cache, struct dc_entry *entry);
bool dentry_deletion (struct hash *dentry_cache, struct dc_entry *entry);
struct dc_entry *dentry_search (const char *path);
struct dc_entry *dentry_parent_search (const char *path);

#endif /* filesys/filesys.h */
