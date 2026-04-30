#include "sqlyt.h"

Pager* pager_open(const char* filename) {
  int fd = open(filename,
                O_RDWR |      // Read/Write mode
                    O_CREAT,  // Create file if it does not exist
                S_IWUSR |     // User write permission
                    S_IRUSR   // User read permission
                );

  if (fd == -1) {
    printf("Unable to open file\n");
    exit(EXIT_FAILURE);
  }

  char wal_filename[512];
  snprintf(wal_filename, sizeof(wal_filename), "%s-wal", filename);
  int wal_fd = open(wal_filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
  if (wal_fd == -1) {
    printf("Unable to open WAL file\n");
    exit(EXIT_FAILURE);
  }

  off_t file_length = lseek(fd, 0, SEEK_END);

  Pager* pager = malloc(sizeof(Pager));
  pager->file_descriptor = fd;
  pager->wal_file_descriptor = wal_fd;
  pager->file_length = file_length;
  pager->num_pages = (file_length / PAGE_SIZE);
  pager->wal_frame_count = 0;
  pager->checkpoint_in_progress = false;
  pthread_mutex_init(&pager->wal_mutex, NULL);
  
  if (file_length > 0 && file_length % PAGE_SIZE != 0) {
    if (file_length == sizeof(DbFileHeader)) {
      file_length = PAGE_SIZE;
      ftruncate(fd, PAGE_SIZE);
      pager->file_length = file_length;
      pager->num_pages = 1;
    } else {
      printf("Db file is not a whole number of pages. Corrupt file.\n");
      exit(EXIT_FAILURE);
    }
  }

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    pager->pages[i] = NULL;
    pager->page_to_wal_frame[i] = 0;
    pager->page_dirty[i] = 0;
  }

  off_t wal_length = lseek(wal_fd, 0, SEEK_END);
  uint32_t frame_size = sizeof(WalFrameHeader) + PAGE_SIZE;
  uint32_t complete_frames = wal_length / frame_size;
  
  lseek(wal_fd, 0, SEEK_SET);
  uint32_t last_commit_frame = 0;
  for (uint32_t i = 0; i < complete_frames; i++) {
    WalFrameHeader header;
    ssize_t n = read(wal_fd, &header, sizeof(WalFrameHeader));
    if (n != (ssize_t)sizeof(WalFrameHeader)) {
      /* Truncated or corrupt WAL header — stop replaying */
      complete_frames = i;
      break;
    }
    if (lseek(wal_fd, PAGE_SIZE, SEEK_CUR) == (off_t)-1) {
      complete_frames = i;
      break;
    }
    if (header.page_num < TABLE_MAX_PAGES) {
      pager->page_to_wal_frame[header.page_num] = i + 1;
    }
    if (header.is_commit) {
      last_commit_frame = i + 1;
    }
  }
  
  if (complete_frames > last_commit_frame) {
    ftruncate(wal_fd, last_commit_frame * frame_size);
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) pager->page_to_wal_frame[i] = 0;
    lseek(wal_fd, 0, SEEK_SET);
    for (uint32_t i = 0; i < last_commit_frame; i++) {
      WalFrameHeader header;
      ssize_t n = read(wal_fd, &header, sizeof(WalFrameHeader));
      if (n != (ssize_t)sizeof(WalFrameHeader)) {
        /* Unexpected short read in already-committed frames; treat as corrupt */
        break;
      }
      if (lseek(wal_fd, PAGE_SIZE, SEEK_CUR) == (off_t)-1) {
        /* Seek failed; stop rebuilding the mapping */
        break;
      }
      if (header.page_num < TABLE_MAX_PAGES) {
        pager->page_to_wal_frame[header.page_num] = i + 1;
      }
    }
  }
  pager->wal_frame_count = last_commit_frame;

  return pager;
}

bool header_is_valid(const DbFileHeader* header) {
  if (memcmp(header->magic, DB_HEADER_MAGIC, sizeof(header->magic)) != 0) {
    return false;
  }
  if (header->version != DB_FORMAT_VERSION) {
    return false;
  }
  if (header->master_root_page == 0 ||
      header->master_root_page >= TABLE_MAX_PAGES ||
      header->next_root_page <= header->master_root_page ||
      header->next_root_page >= TABLE_MAX_PAGES) {
    return false;
  }
  if (header->free_page_head >= TABLE_MAX_PAGES) {
    return false;
  }
  return true;
}

uint32_t recover_next_root_page_from_master(Pager* pager,
                                            uint32_t master_root_page) {
  Table master_table;
  Cursor* cursor;
  uint32_t max_root = master_root_page;

  table_init_catalog(&master_table, pager);
  master_table.root_page_num = master_root_page;

  cursor = table_start(&master_table);
  while (!cursor->end_of_table) {
    void* node = get_page(pager, cursor->page_num);
    uint32_t k = *tbl_leaf_key(&master_table, node, cursor->cell_num);
    if (k > max_root) {
      max_root = k;
    }
    cursor_advance(cursor);
  }
  free(cursor);

  if (max_root + 1 >= TABLE_MAX_PAGES) {
    printf("Corrupted catalog: no free root pages left.\n");
    exit(EXIT_FAILURE);
  }
  return max_root + 1;
}

// persist_header_now is removed because it circumvented WAL.

Table* db_open(const char* filename) {
  Pager* pager = pager_open(filename);
  bool is_new_file = (pager->num_pages == 0);

  Table* table = malloc(sizeof(Table));
  table->pager = pager;

  void* header_page = get_page(pager, 0);
  DbFileHeader* header = (DbFileHeader*)header_page;

  if (is_new_file) {
    memset(header, 0, PAGE_SIZE);
    memcpy(header->magic, DB_HEADER_MAGIC, sizeof(header->magic));
    header->version = DB_FORMAT_VERSION;
    header->master_root_page = DB_HEADER_MASTER_ROOT_PAGE;
    header->next_root_page = DB_HEADER_FIRST_USER_ROOT_PAGE;
    header->free_page_head = 0;

    void* root_node = get_page(pager, header->master_root_page);
    initialize_leaf_node(root_node);
    set_node_root(root_node, true);
    mark_page_dirty(pager, 0);
    mark_page_dirty(pager, header->master_root_page);
    pager_commit_transaction_sync(pager);
  } else {
    if (!header_is_valid(header)) {
      uint32_t repaired_master_root = DB_HEADER_MASTER_ROOT_PAGE;
      uint32_t repaired_next_root;
      void* repaired_root = get_page(pager, repaired_master_root);

      if (get_node_type(repaired_root) != NODE_LEAF &&
          get_node_type(repaired_root) != NODE_INTERNAL) {
        initialize_leaf_node(repaired_root);
        set_node_root(repaired_root, true);
      }

      repaired_next_root =
          recover_next_root_page_from_master(pager, repaired_master_root);

      memset(header, 0, PAGE_SIZE);
      memcpy(header->magic, DB_HEADER_MAGIC, sizeof(header->magic));
      header->version = DB_FORMAT_VERSION;
      header->master_root_page = repaired_master_root;
      header->next_root_page = repaired_next_root;
      header->free_page_head = 0;
      mark_page_dirty(pager, 0);
      pager_commit_transaction_sync(pager);

      printf("Recovered database header metadata.\n");
    }
  }

  pager->master_root_page = header->master_root_page;
  pager->next_root_page = header->next_root_page;
  pager->free_page_head = header->free_page_head;
  table_init_catalog(table, pager);

  return table;
}

void pager_flush(Pager* pager, uint32_t page_num, uint32_t is_commit) {
  pthread_mutex_lock(&pager->wal_mutex);
  if (pager->pages[page_num] == NULL) {
    printf("Tried to flush null page\n");
    exit(EXIT_FAILURE);
  }

  uint32_t frame_index = pager->wal_frame_count;
  off_t offset = frame_index * (sizeof(WalFrameHeader) + PAGE_SIZE);
  lseek(pager->wal_file_descriptor, offset, SEEK_SET);

  WalFrameHeader header;
  header.page_num = page_num;
  header.is_commit = is_commit;
  
  if (write(pager->wal_file_descriptor, &header, sizeof(WalFrameHeader)) !=
      sizeof(WalFrameHeader)) {
    printf("Error writing WAL header: %d\n", errno);
    exit(EXIT_FAILURE);
  }
  if (write(pager->wal_file_descriptor, pager->pages[page_num], PAGE_SIZE) !=
      PAGE_SIZE) {
    printf("Error writing WAL page: %d\n", errno);
    exit(EXIT_FAILURE);
  }

  pager->page_to_wal_frame[page_num] = frame_index + 1;
  pager->wal_frame_count += 1;
  pthread_mutex_unlock(&pager->wal_mutex);
}

void* background_checkpoint_task(void* arg) {
  Pager* pager = (Pager*)arg;
  bool checkpoint_failed = false;
  pthread_mutex_lock(&pager->wal_mutex);

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    if (pager->page_to_wal_frame[i] > 0) {
      uint32_t frame_index = pager->page_to_wal_frame[i] - 1;
      off_t wal_offset =
          frame_index * (sizeof(WalFrameHeader) + PAGE_SIZE) +
          sizeof(WalFrameHeader);

      void* page = malloc(PAGE_SIZE);
      if (page == NULL) {
        printf("Error allocating checkpoint page buffer\n");
        checkpoint_failed = true;
        break;
      }

      if (lseek(pager->wal_file_descriptor, wal_offset, SEEK_SET) == (off_t)-1) {
        printf("Error seeking WAL during checkpoint: %d\n", errno);
        free(page);
        checkpoint_failed = true;
        break;
      }

      ssize_t bytes_read = read(pager->wal_file_descriptor, page, PAGE_SIZE);
      if (bytes_read == -1 || (uint32_t)bytes_read != PAGE_SIZE) {
        printf("Error reading WAL page during checkpoint: %d\n", errno);
        free(page);
        checkpoint_failed = true;
        break;
      }

      if (lseek(pager->file_descriptor, i * PAGE_SIZE, SEEK_SET) == (off_t)-1) {
        printf("Error seeking database file during checkpoint: %d\n", errno);
        free(page);
        checkpoint_failed = true;
        break;
      }

      ssize_t bytes_written = write(pager->file_descriptor, page, PAGE_SIZE);
      if (bytes_written == -1 || (uint32_t)bytes_written != PAGE_SIZE) {
        printf("Error writing database page during checkpoint: %d\n", errno);
        free(page);
        checkpoint_failed = true;
        break;
      }

      free(page);
    }
  }

  if (!checkpoint_failed) {
    if (ftruncate(pager->wal_file_descriptor, 0) == -1) {
      printf("Error truncating WAL after checkpoint: %d\n", errno);
      checkpoint_failed = true;
    } else {
      pager->wal_frame_count = 0;
      for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->page_to_wal_frame[i] = 0;
      }
    }
  }

  pager->checkpoint_in_progress = false;
  pthread_mutex_unlock(&pager->wal_mutex);

  if (checkpoint_failed) {
    exit(EXIT_FAILURE);
  }
  return NULL;
}

void pager_checkpoint(Pager* pager) {
  background_checkpoint_task(pager);
}

void pager_commit_transaction_sync(Pager* pager) {
  uint32_t dirty_count = 0;
  uint32_t last_dirty_page = 0;
  
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    if (pager->page_dirty[i]) {
      dirty_count++;
      last_dirty_page = i;
    }
  }
  
  if (dirty_count == 0) {
    return;
  }

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    if (pager->page_dirty[i]) {
      pager_flush(pager, i, (i == last_dirty_page) ? 1 : 0);
      pager->page_dirty[i] = 0;
    }
  }

  pthread_mutex_lock(&pager->wal_mutex);
  bool should_checkpoint =
      (pager->wal_frame_count >= WAL_CHECKPOINT_THRESHOLD &&
       !pager->checkpoint_in_progress);
  if (should_checkpoint) {
    pager->checkpoint_in_progress = true;
  }
  pthread_mutex_unlock(&pager->wal_mutex);

  if (should_checkpoint) {
    /* Synchronous checkpoint to keep benchmarking deterministic and avoid
       races with shutdown closing file descriptors while a detached thread
       is still running. */
    background_checkpoint_task(pager);
  }
}

void db_close(Table* table) {
  Pager* pager = table->pager;

  // Mark all in-memory pages dirty so they are committed as a single WAL
  // transaction (only the last frame has is_commit=1), preventing partial
  // commit visibility on crash during close.
  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (pager->pages[i] != NULL) {
      pager->page_dirty[i] = 1;
    }
  }
  pager_commit_transaction_sync(pager);

  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (pager->pages[i] == NULL) {
      continue;
    }
    free(pager->pages[i]);
    pager->pages[i] = NULL;
  }

  pager_checkpoint(pager); // Sync WAL to DB before exiting

  int result = close(pager->file_descriptor);
  close(pager->wal_file_descriptor);
  if (result == -1) {
    printf("Error closing db file.\n");
    exit(EXIT_FAILURE);
  }
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    void* page = pager->pages[i];
    if (page) {
      free(page);
      pager->pages[i] = NULL;
    }
  }
  free(pager);
  free(table);
}

/*
free pages are recycled, through a free page list
*/
uint32_t get_unused_page_num(Pager* pager) {
  /* Prefer reusing a free page if available. */
  void* header_page = get_page(pager, 0);
  DbFileHeader* header = (DbFileHeader*)header_page;
  if (header->version == DB_FORMAT_VERSION && header->free_page_head != 0) {
    uint32_t page_num = header->free_page_head;
    void* free_page = get_page(pager, page_num);
    uint32_t next = 0;
    memcpy(&next, (uint8_t*)free_page + PAGE_SIZE - sizeof(uint32_t),
           sizeof(uint32_t));
    header->free_page_head = next;
    pager->free_page_head = next;
    mark_page_dirty(pager, 0);
    /* Clear stale on-page pointers/headers before reuse. */
    memset(free_page, 0, PAGE_SIZE);
    mark_page_dirty(pager, page_num);
    /* Prevent accidental reads from an old WAL frame mapping for this page. */
    pthread_mutex_lock(&pager->wal_mutex);
    pager->page_to_wal_frame[page_num] = 0;
    pthread_mutex_unlock(&pager->wal_mutex);
    return page_num;
  }

  mark_page_dirty(pager, pager->num_pages);
  return pager->num_pages;
}

void pager_free_page(Pager* pager, uint32_t page_num) {
  if (page_num == 0 || page_num >= TABLE_MAX_PAGES) {
    return;
  }
  if (page_num == pager->master_root_page) {
    return;
  }

  void* header_page = get_page(pager, 0);
  DbFileHeader* header = (DbFileHeader*)header_page;
  if (header->version != DB_FORMAT_VERSION) {
    return;
  }

  /* Push the page onto the freelist.
     Store next pointer at the end of the page so the node header can be made safe. */
  void* page = get_page(pager, page_num);
  uint32_t next = header->free_page_head;
  /* Make the freed page look like an empty leaf node to avoid out-of-bounds
     traversal if it's accidentally referenced due to a higher-level bug. */
  memset(page, 0, PAGE_SIZE);
  ((uint8_t*)page)[0] = (uint8_t)NODE_LEAF; /* node type */
  ((uint8_t*)page)[1] = 0;                 /* is_root */
  /* parent pointer (bytes 2..5) already zeroed */
  /* leaf_node_num_cells at offset 6..9 and next_leaf at 10..13 are zero */

  memcpy((uint8_t*)page + PAGE_SIZE - sizeof(uint32_t), &next, sizeof(uint32_t));
  header->free_page_head = page_num;
  pager->free_page_head = page_num;

  mark_page_dirty(pager, page_num);
  mark_page_dirty(pager, 0);
}

