#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/buffer_cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_BLOCK_ENTRIES 123
#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))


enum direct_t
  {
    NORMAL_DIRECT,
    INDIRECT,
    DOUBLE_INDIRECT,
    OUT_LIMIT
  };

struct sector_location
  {
    int directness;
    int index1;
    int index2;
  };

struct inode_indirect_block
  {
    block_sector_t map_table[INDIRECT_BLOCK_ENTRIES];
  };

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    //block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    //uint32_t unused[125];               /* Not used. */
    bool is_dir;
    block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
    block_sector_t indirect_block_sec;
    block_sector_t double_indirect_block_sec;
    
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    //struct inode_disk data;             /* Inode content. */
    struct lock extend_lock;
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
/*
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}
*/

static bool register_sector (struct inode_disk *, block_sector_t, struct sector_location);
static void free_inode_sectors (struct inode_disk *);
static bool get_disk_inode (const struct inode *, struct inode_disk *);
static void locate_byte (off_t, struct sector_location *);
static bool inode_update_file_length (struct inode_disk *, off_t, off_t);


static block_sector_t byte_to_sector (const struct inode_disk *inode_disk, off_t pos)
{

  block_sector_t return_sec;
  
  if (pos < inode_disk->length){
    struct inode_indirect_block *idblock1;
    struct inode_indirect_block *idblock2;
    struct sector_location sec_loc;
    locate_byte (pos, &sec_loc);
    switch (sec_loc.directness){
      case NORMAL_DIRECT:;
        return_sec = inode_disk->direct_map_table[sec_loc.index1];
        break;
      
      case INDIRECT:;
        idblock1  = (struct inode_indirect_block *)malloc(BLOCK_SECTOR_SIZE);

        if (idblock1 != NULL){

          if (inode_disk->indirect_block_sec == -1)
            return_sec = -1;

          else{
            bc_read(inode_disk->indirect_block_sec, idblock1, 0, sizeof(struct inode_indirect_block), 0);
            return_sec = idblock1->map_table[sec_loc.index1];
          }
        }

        else
          return -1;
        
        free (idblock1);
        break;

      case DOUBLE_INDIRECT:;
        idblock1  = (struct inode_indirect_block *)malloc(BLOCK_SECTOR_SIZE);
        if (idblock1 == NULL)
          return -1;//
        
        idblock2  = (struct inode_indirect_block *)malloc(BLOCK_SECTOR_SIZE);
        if (idblock2 == NULL){
          free(idblock1);
          return -1;//-1
        }

        if (inode_disk->double_indirect_block_sec == -1)
          return_sec = -1;
        
        else{
          bc_read(inode_disk->double_indirect_block_sec, idblock1, 0, sizeof(struct inode_indirect_block), 0);
          if (idblock1->map_table[sec_loc.index1] == -1)
            return_sec = -1;
          else{
            block_sector_t temp = idblock1->map_table[sec_loc.index1];

            bc_read(temp, idblock2, 0, sizeof(struct inode_indirect_block), 0);
            return_sec = idblock2->map_table[sec_loc.index2];
          }        
        }

                  
        free(idblock1);
        free(idblock2);
        break;
    }
    return return_sec;
  }
  
  else{
    return -1;//-1
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, uint32_t is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      memset(disk_inode, -1, sizeof(struct inode_disk));
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
     
      if(!inode_update_file_length (disk_inode, 0, length)){
        free(disk_inode);
        return false;
      }
      
      bc_write(sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
      free (disk_inode);
      success = true;
  }

  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  //block_read (fs_device, inode->sector, &inode->data);
  lock_init(&inode->extend_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          struct inode_disk *disk_inode = calloc (1, sizeof(*disk_inode));
          get_disk_inode (inode, disk_inode);
          free_inode_sectors (disk_inode);
          free_map_release (inode->sector, 1);
          free (disk_inode);          

        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  lock_acquire (&inode->extend_lock);
  struct inode_disk *disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode == NULL){
    return 0;
  }

  get_disk_inode (inode, disk_inode);

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);

      if (sector_idx == -1)
        break;

      lock_release (&inode->extend_lock);

      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0){
        lock_acquire (&inode->extend_lock);
        break;
      }
      bc_read (sector_idx, buffer, bytes_read, chunk_size, sector_ofs);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
      lock_acquire (&inode->extend_lock);
    }
  free (disk_inode);
  lock_release (&inode->extend_lock);
  return bytes_read;
}



/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  struct inode_disk *disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode == NULL)
    return 0;
  get_disk_inode (inode, disk_inode);  

  lock_acquire (&inode->extend_lock);

  int old_length = disk_inode->length;
  int write_end = offset + size - 1;

  if (write_end > old_length - 1){
    inode_update_file_length (disk_inode, old_length, write_end + 1);
    bc_write (inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
  }
  
  while (size > 0) 
  {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);
      lock_release(&inode->extend_lock);

      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0){
        lock_acquire (&inode->extend_lock);
        break;
      }
      bc_write (sector_idx, buffer, bytes_written, chunk_size, sector_ofs);
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
      lock_acquire (&inode->extend_lock);
  }
  free (disk_inode);
  lock_release (&inode->extend_lock);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct inode_disk *disk_inode = calloc(1, sizeof(*disk_inode));
  bc_read(inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
  off_t length = disk_inode->length;
  free(disk_inode);
  return length;
  //return inode->data.length;
}

// Lê do disco o inode de INODE e preenche INODE_DISK
static bool get_disk_inode (const struct inode *inode, struct inode_disk *inode_disk){
  return bc_read (inode->sector, inode_disk, 0, sizeof *inode_disk, 0);
}

// Calcula a localização (direto/indireto/duplo) e índices para o offset POS
static void locate_byte (off_t pos, struct sector_location *sec_loc){
  off_t pos_sector = pos/BLOCK_SECTOR_SIZE;

  if (pos_sector < DIRECT_BLOCK_ENTRIES)
  {
    sec_loc->directness = NORMAL_DIRECT;
    sec_loc->index1 = pos_sector;
  }
  
  else if (pos_sector < (off_t) (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES))
  {  
    sec_loc->directness = INDIRECT;
    sec_loc->index1 = (pos_sector-DIRECT_BLOCK_ENTRIES);
  }
  
  else if (pos_sector < (off_t) (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES*INDIRECT_BLOCK_ENTRIES))
  {
    sec_loc->directness = DOUBLE_INDIRECT;
    sec_loc->index1 = (pos_sector-DIRECT_BLOCK_ENTRIES-INDIRECT_BLOCK_ENTRIES) / INDIRECT_BLOCK_ENTRIES;
    sec_loc->index2 =  (pos_sector-DIRECT_BLOCK_ENTRIES-INDIRECT_BLOCK_ENTRIES) % INDIRECT_BLOCK_ENTRIES;
  }
  else
    sec_loc->directness = OUT_LIMIT;
}


// Converte um índice de tabela em deslocamento (bytes) dentro do bloco
static inline off_t map_table_offset (int index){
  off_t byte;
  byte = index * 4;
  return byte;
}


// Associa NEW_SECTOR à posição indicada por sec_loc, alocando/escrevendo blocos indiretos quando necessário
static bool register_sector (struct inode_disk *inode_disk, block_sector_t new_sector, struct sector_location sec_loc){
  struct inode_indirect_block *idblock1;
  struct inode_indirect_block *idblock2;
  block_sector_t *table_sector;

  switch (sec_loc.directness)
  {
    
    case NORMAL_DIRECT:;
      inode_disk->direct_map_table[sec_loc.index1] = new_sector;
      break;

    case INDIRECT:;
      idblock1 = malloc(BLOCK_SECTOR_SIZE);
    
      if (idblock1 != NULL){
        memset(idblock1, -1, sizeof(struct inode_indirect_block));
        if (inode_disk->indirect_block_sec != -1)
        {
          if(!bc_read(inode_disk->indirect_block_sec, idblock1, 0, sizeof(struct inode_indirect_block), 0)){
            free(idblock1);
            return false;
          }
        }

        else{
          if(!free_map_allocate(1, &inode_disk->indirect_block_sec)){
            free(idblock1);
            return false;
          }
        }

        if (idblock1->map_table[sec_loc.index1] == -1)
          idblock1->map_table[sec_loc.index1] = new_sector;
        
        if(!bc_write(inode_disk->indirect_block_sec, idblock1, 0, sizeof(struct inode_indirect_block), 0)){
          free(idblock1);
          return false;
        }
      }   
      
      else 
        return false;
      
      free (idblock1);
      break;

    case DOUBLE_INDIRECT:;
      bool update = false;
      
      idblock1 = malloc (BLOCK_SECTOR_SIZE);
      if (idblock1 == NULL)
        return false;

      idblock2 = malloc (BLOCK_SECTOR_SIZE);
      if (idblock2 == NULL){
        free (idblock1);
        return false;
      }

      memset(idblock1, -1, sizeof(struct inode_indirect_block));
      memset(idblock2, -1, sizeof(struct inode_indirect_block));

      if (inode_disk->double_indirect_block_sec != -1){
        if(!bc_read(inode_disk->double_indirect_block_sec, idblock1, 0, sizeof(struct inode_indirect_block), 0)){
          free(idblock1);
          free(idblock2);
          return false;
        } 
      }
      else{
        if(!free_map_allocate(1, &inode_disk->double_indirect_block_sec)){
          free(idblock1);
          free(idblock2);
          return false;
        }
      }
      if (idblock1->map_table[sec_loc.index1] != -1){
        if(!bc_read(idblock1->map_table[sec_loc.index1], idblock2, 0, sizeof(struct inode_indirect_block), 0)){
          free(idblock1);
          free(idblock2);
          return false;
        }  
      }
      else{
        if(!free_map_allocate(1, &idblock1->map_table[sec_loc.index1])){
          free(idblock1);
          free(idblock2);
          return false;
        }
        update = true;
      }

      if (idblock2->map_table[sec_loc.index2] == -1)
        idblock2->map_table[sec_loc.index2] = new_sector;
      
      if (update){
        if(!bc_write(inode_disk->double_indirect_block_sec, idblock1, 0, sizeof(struct inode_indirect_block), 0)){
          free(idblock1);
          free(idblock2);
          return false;
        }
      }
      
      if(!bc_write(idblock1->map_table[sec_loc.index1], idblock2, 0, sizeof(struct inode_indirect_block), 0)){
        free(idblock1);
        free(idblock2);
        return false;
      }

      free(idblock1);
      free(idblock2);
      break;

    default:
      return false;
  }
  return true;
}


// Estende `inode_disk` para END_POS alocando setores necessários entre START_POS e END_POS
static bool inode_update_file_length(struct inode_disk * inode_disk, off_t start_pos, off_t end_pos){
  off_t size = end_pos - start_pos;
  if (size < 0)
    return false;
  if (size == 0)
    return true;
  
  struct inode_indirect_block *zeros = malloc(BLOCK_SECTOR_SIZE);
  memset(zeros, 0, BLOCK_SECTOR_SIZE);

  inode_disk -> length = end_pos;
  int chunk_size = BLOCK_SECTOR_SIZE;

  int offset = start_pos;

  block_sector_t sector_idx;
  
  while(size>0){
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;
    if (sector_ofs > 0){
      offset = offset/ BLOCK_SECTOR_SIZE * BLOCK_SECTOR_SIZE;
      offset += chunk_size;
      continue; 
    }
    else{
      sector_idx = byte_to_sector(inode_disk, offset);
      if (free_map_allocate(1, &sector_idx)){
        struct sector_location sec_loc;
        locate_byte(offset, &sec_loc);
        register_sector(inode_disk, sector_idx, sec_loc);
      }
      else{
        free(zeros);
        return false;
      }
    }
      bc_write(sector_idx, zeros, 0, BLOCK_SECTOR_SIZE, 0);
    size -= chunk_size;
    offset += chunk_size;
  }  
  free(zeros);
  return true;
}


// Libera todos os setores associados ao inode_disk (diretos, indiretos e duplos)
static void free_inode_sectors (struct inode_disk *inode_disk){
  if (inode_disk->double_indirect_block_sec != -1){    
    
    int i;
    struct inode_indirect_block *idblock1 = malloc(BLOCK_SECTOR_SIZE);
    struct inode_indirect_block *idblock2 = malloc(BLOCK_SECTOR_SIZE);
    bc_read (inode_disk->double_indirect_block_sec, idblock1, 0, sizeof(struct inode_indirect_block), 0);
    for (i=0; i<INDIRECT_BLOCK_ENTRIES; i++){
      if (idblock1->map_table[i] == -1)
        return;    
      
      int j;
      bc_read (idblock1->map_table[i], idblock2, 0, sizeof(struct inode_indirect_block), 0);
      for (j=0; j<INDIRECT_BLOCK_ENTRIES; j++){
        if (idblock2->map_table[j] == -1){
          free(idblock1);
          free(idblock2);
          return;
        }
        free_map_release (idblock2->map_table[j], 1);  
      }
      free_map_release (idblock1->map_table[i], 1);
    }
    free_map_release (inode_disk->double_indirect_block_sec, 1);
    free (idblock1);
    free (idblock2);
  }
  if (inode_disk->indirect_block_sec != -1){
    
    int k;
    struct inode_indirect_block *idblock1 = malloc(BLOCK_SECTOR_SIZE);
    bc_read (inode_disk->indirect_block_sec, idblock1, 0, sizeof(struct inode_indirect_block), 0);
    for (k=0; k<INDIRECT_BLOCK_ENTRIES; k++){
      if (idblock1->map_table[k] == -1){
        free (idblock1);
        return;
      }
      free_map_release (idblock1->map_table[k], 1);  
    }
    free_map_release (inode_disk->indirect_block_sec, 1);
    free (idblock1);
  }
  int l;

  for (l = 0; l < DIRECT_BLOCK_ENTRIES; l++)
  {
    if (inode_disk->direct_map_table[l] == -1)
      return;
    free_map_release (inode_disk->direct_map_table[l], 1);
  }

}


bool inode_is_dir (const struct inode *inode){
// Retorna true se o inode representa um diretório

  bool result = false;
  if(inode->removed){
      return result;
  }

  struct inode_disk *disk_inode;
  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode==NULL){
    return result;
  }

  if (get_disk_inode(inode, disk_inode)==NULL){
    free(disk_inode);
    return result;
  }

  result = disk_inode->is_dir; 
  free(disk_inode);
  return result;
} 


// Retorna o número de vezes que o inode está atualmente aberto
int inode_open_cnt (struct inode *inode){
  return inode->open_cnt;
}