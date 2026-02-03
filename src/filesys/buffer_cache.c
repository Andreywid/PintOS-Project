#include "filesys/buffer_cache.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include <string.h>
#include <debug.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define BUFFER_CACHE_ENTRY_NB 64

void *buffer_cache_area;

static struct buffer_head cache_table[BUFFER_CACHE_ENTRY_NB];
static struct buffer_head *clock_ptr;

// Verifica se um setor é válido no dispositivo de arquivos
static bool sector_is_valid (block_sector_t sector_idx){
  if (fs_device == NULL)
    return false;

  if (sector_idx == (block_sector_t) -1)
    return false;

  return sector_idx < block_size (fs_device);
}

// Lê bytes de um setor usando o buffer cache, carregando do disco se necessário
bool bc_read (block_sector_t sector_idx, void *dst, off_t bytes_read, int chunk_size, int sector_ofs){
  if (!sector_is_valid (sector_idx))
    {
      memset ((uint8_t *) dst + bytes_read, 0, chunk_size);
      return false;
    }

  struct buffer_head *bh = bc_lookup (sector_idx);

  if (bh == NULL)
    {
      bh = cache_table;
      int idx;
      for (idx = 0; idx < BUFFER_CACHE_ENTRY_NB; idx++)
        {
          if (!bh->usage)
            {
              lock_acquire (&bh->bc_lock);
              clock_ptr = bh;
              clock_ptr++;
              break;
            }
          bh++;
        }

      if (idx == BUFFER_CACHE_ENTRY_NB)
        bh = bc_select_victim ();

      bh->usage  = true;
      bh->dirty  = false;
      bh->sector = sector_idx;

      block_read (fs_device, sector_idx, bh->data);
    }

  memcpy ((uint8_t *) dst + bytes_read,
          (uint8_t *) bh->data + sector_ofs, chunk_size);

  lock_release (&bh->bc_lock);
  bh->clock = true;
  return true;
}

// Escreve bytes em um setor via buffer cache, marcando a entrada como suja
bool bc_write (block_sector_t sector_idx, void *src, off_t bytes_written, int chunk_size, int sector_ofs){
  if (!sector_is_valid (sector_idx))
    return false;

  struct buffer_head *bh = bc_lookup (sector_idx);

  if (bh == NULL)
    {
      bh = cache_table;
      int idx;
      for (idx = 0; idx < BUFFER_CACHE_ENTRY_NB; idx++)
        {
          if (!bh->usage)
            {
              lock_acquire (&bh->bc_lock);
              clock_ptr = bh;
              clock_ptr++;
              break;
            }
          bh++;
        }

      if (idx == BUFFER_CACHE_ENTRY_NB)
        bh = bc_select_victim ();

      bh->usage  = true;
      bh->dirty  = false;
      bh->sector = sector_idx;

      block_read (fs_device, sector_idx, bh->data);
    }

  bh->clock = true;
  bh->dirty = true;

  memcpy ((uint8_t *) bh->data + sector_ofs,
          (uint8_t *) src + bytes_written, chunk_size);

  lock_release (&bh->bc_lock);
  return true;
}

// Despeja no disco o conteúdo sujo de uma entrada específica do cache
void
bc_flush_entry (struct buffer_head *bh) {
  if (!sector_is_valid (bh->sector))
    return;

  block_write (fs_device, bh->sector, bh->data);
  bh->dirty = false;
}

// Inicializa a área e as entradas do buffer cache
void bc_init (void) {
  buffer_cache_area = malloc (BUFFER_CACHE_ENTRY_NB * BLOCK_SECTOR_SIZE);
  memset (buffer_cache_area, 0, BUFFER_CACHE_ENTRY_NB * BLOCK_SECTOR_SIZE);

  struct buffer_head *bh = cache_table;
  for (int i = 0; i < BUFFER_CACHE_ENTRY_NB; i++)
    {
      bh->data   = (uint8_t *) buffer_cache_area + i * BLOCK_SECTOR_SIZE;
      bh->sector = (block_sector_t) -1;
      lock_init (&bh->bc_lock);
      bh++;
    }

  clock_ptr = cache_table;
}

// Encerra o buffer cache, executando flush geral e liberando memória
void bc_term (void) {
  bc_flush_all_entries ();
  free (buffer_cache_area);
}

// Faz flush de todas as entradas sujas do buffer cache para o disco
void bc_flush_all_entries (void) {
  struct buffer_head *bh = cache_table;

  for (int i = 0; i < BUFFER_CACHE_ENTRY_NB; i++)
    {
      if (bh->dirty)
        {
          lock_acquire (&bh->bc_lock);
          bc_flush_entry (bh);
          lock_release (&bh->bc_lock);
        }
      bh++;
    }
}

// Seleciona uma entrada vítima usando o algoritmo de clock, flushando se suja
struct buffer_head *bc_select_victim (void) {
  for (int i = 0; i < 5 * BUFFER_CACHE_ENTRY_NB; i++)
    {
      if (clock_ptr == cache_table + BUFFER_CACHE_ENTRY_NB)
        clock_ptr = cache_table;

      lock_acquire (&clock_ptr->bc_lock);

      if (!clock_ptr->clock)
        {
          if (clock_ptr->dirty)
            bc_flush_entry (clock_ptr);

          return clock_ptr++;
        }

      clock_ptr->clock = false;
      lock_release (&clock_ptr->bc_lock);
      clock_ptr++;
    }
  return cache_table;
}

// Procura no cache uma entrada correspondente ao setor informado
struct buffer_head *bc_lookup (block_sector_t sector){
  struct buffer_head *bh = cache_table;

  for (int i = 0; i < BUFFER_CACHE_ENTRY_NB; i++){
    if (bh->sector == sector){
      lock_acquire (&bh->bc_lock);
      return bh;
    }
    bh++;
  }

  return NULL;
}
