#include "sqlyt.h"

void mark_page_dirty(Pager* pager, uint32_t page_num) {
  if (page_num < TABLE_MAX_PAGES) {
    pager->page_dirty[page_num] = 1;
  }
}

void pager_commit_transaction_sync(Pager* pager);

/*
 * Common Node Header Layout
 */
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
const uint8_t COMMON_NODE_HEADER_SIZE =
    NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

/*
 * Internal Node Header Layout
 */
const uint32_t INTERNAL_NODE_NUM_KEYS_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET =
    INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;
const uint32_t INTERNAL_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                           INTERNAL_NODE_NUM_KEYS_SIZE +
                                           INTERNAL_NODE_RIGHT_CHILD_SIZE;

/*
 * Internal Node Body Layout
 */
const uint32_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CELL_SIZE =
    INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;
/* Keep this small for testing */
const uint32_t INTERNAL_NODE_MAX_KEYS = 20;

/*
 * Leaf Node Header Layout
 */
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_NEXT_LEAF_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NEXT_LEAF_OFFSET =
    LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                       LEAF_NODE_NUM_CELLS_SIZE +
                                       LEAF_NODE_NEXT_LEAF_SIZE;

/*
 * Leaf Node Body Layout
 */
const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_KEY_OFFSET = 0;
#define LEAF_NODE_SPACE_FOR_CELLS (PAGE_SIZE - LEAF_NODE_HEADER_SIZE)

NodeType get_node_type(void* node) {
  uint8_t value = *(((uint8_t*)node) + NODE_TYPE_OFFSET);
  return (NodeType)value;
}

void set_node_type(void* node, NodeType type) {
  uint8_t value = type;
  *(((uint8_t*)node) + NODE_TYPE_OFFSET) = value;
}

bool is_node_root(void* node) {
  uint8_t value = *(((uint8_t*)node) + IS_ROOT_OFFSET);
  return (bool)value;
}

void set_node_root(void* node, bool is_root) {
  uint8_t value = is_root;
  *(((uint8_t*)node) + IS_ROOT_OFFSET) = value;
}

uint32_t* node_parent(void* node) {
  return (uint32_t*)((uint8_t*)node + PARENT_POINTER_OFFSET);
}

uint32_t* internal_node_num_keys(void* node) {
  return (uint32_t*)((uint8_t*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

uint32_t* internal_node_right_child(void* node) {
  return (uint32_t*)((uint8_t*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
  return (uint32_t*)((uint8_t*)node + INTERNAL_NODE_HEADER_SIZE +
                     cell_num * INTERNAL_NODE_CELL_SIZE);
}

static uint32_t* internal_node_child_ptr(void* node, uint32_t child_num) {
  uint32_t num_keys = *internal_node_num_keys(node);
  if (child_num > num_keys) {
    printf("Tried to access child_num %d > num_keys %d\n", child_num, num_keys);
    exit(EXIT_FAILURE);
  } else if (child_num == num_keys) {
    return internal_node_right_child(node);
  } else {
    return internal_node_cell(node, child_num);
  }
}

uint32_t* internal_node_child(void* node, uint32_t child_num) {
  uint32_t* p = internal_node_child_ptr(node, child_num);
  uint32_t v = *p;
  if (v == INVALID_PAGE_NUM) {
    if (child_num == *internal_node_num_keys(node)) {
      printf("Tried to access right child of node, but was invalid page\n");
    } else {
      printf("Tried to access child %d of node, but was invalid page\n", child_num);
    }
    exit(EXIT_FAILURE);
  }
  if (v >= TABLE_MAX_PAGES) {
    printf("Tried to access child %u out of bounds at index %u (num_keys=%u)\n",
           v, child_num, *internal_node_num_keys(node));
    exit(EXIT_FAILURE);
  }
  return p;
}

uint32_t* internal_node_key(void* node, uint32_t key_num) {
  return (uint32_t*)((uint8_t*)internal_node_cell(node, key_num) +
                     INTERNAL_NODE_CHILD_SIZE);
}

uint32_t* leaf_node_num_cells(void* node) {
  return (uint32_t*)((uint8_t*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

uint32_t* leaf_node_next_leaf(void* node) {
  return (uint32_t*)((uint8_t*)node + LEAF_NODE_NEXT_LEAF_OFFSET);
}

void table_init_catalog(Table* t, Pager* pager) {
  t->pager = pager;
  t->root_page_num = pager->master_root_page;
  t->is_catalog = true;
  memset(&t->schema, 0, sizeof(t->schema));
  memset(&t->row_layout, 0, sizeof(t->row_layout));
  t->value_size = CATALOG_VALUE_SIZE;
  t->cell_size = LEAF_NODE_KEY_SIZE + t->value_size;
  t->max_cells = LEAF_NODE_SPACE_FOR_CELLS / t->cell_size;
}

bool compute_row_layout(const SqlSchema* schema, RowLayout* L) {
  L->value_size = 0;
  for (uint32_t i = 0; i < schema->column_count; i++) {
    L->col_offset[i] = L->value_size;
    if (schema->column_type[i] == COLUMN_TYPE_INT) {
      L->col_size[i] = sizeof(uint32_t);
    } else {
      L->col_size[i] = schema->column_max[i];
    }
    L->value_size += L->col_size[i];
  }
  uint32_t cell = LEAF_NODE_KEY_SIZE + L->value_size;
  if (L->value_size == 0 || cell > LEAF_NODE_SPACE_FOR_CELLS) {
    return false;
  }
  return true;
}

bool table_init_user(Table* t, Pager* pager, uint32_t root_page,
                     const SqlSchema* schema) {
  t->pager = pager;
  t->root_page_num = root_page;
  t->is_catalog = false;
  memcpy(&t->schema, schema, sizeof(SqlSchema));
  if (!compute_row_layout(schema, &t->row_layout)) {
    return false;
  }
  t->value_size = t->row_layout.value_size;
  t->cell_size = LEAF_NODE_KEY_SIZE + t->value_size;
  t->max_cells = LEAF_NODE_SPACE_FOR_CELLS / t->cell_size;
  return t->max_cells > 0;
}

static inline void* tbl_leaf_cell(Table* t, void* node, uint32_t cell_num) {
  return (uint8_t*)node + LEAF_NODE_HEADER_SIZE + cell_num * t->cell_size;
}

uint32_t* tbl_leaf_key(Table* t, void* node, uint32_t cell_num) {
  return (uint32_t*)tbl_leaf_cell(t, node, cell_num);
}

void* tbl_leaf_value(Table* t, void* node, uint32_t cell_num) {
  return (uint8_t*)tbl_leaf_cell(t, node, cell_num) + LEAF_NODE_KEY_SIZE;
}

static inline uint32_t* leaf_key_sized(void* node, uint32_t cell_num,
                                       uint32_t cell_size) {
  return (uint32_t*)((uint8_t*)node + LEAF_NODE_HEADER_SIZE +
                     cell_num * cell_size);
}

void* get_page(Pager* pager, uint32_t page_num) {
  if (page_num >= TABLE_MAX_PAGES) {
    printf("Tried to fetch page number out of bounds. %d >= %d\n", page_num,
           TABLE_MAX_PAGES);
    exit(EXIT_FAILURE);
  }

  if (pager->pages[page_num] == NULL) {
    // Cache miss. Allocate memory and load from file.
    void* page = malloc(PAGE_SIZE);
    uint32_t num_pages = pager->file_length / PAGE_SIZE;

    // We might save a partial page at the end of the file
    if (pager->file_length % PAGE_SIZE) {
      num_pages += 1;
    }

    // Hold wal_mutex while checking the WAL mapping and reading from WAL to
    // prevent a concurrent checkpoint from truncating the WAL mid-read.
    pthread_mutex_lock(&pager->wal_mutex);
    if (pager->page_to_wal_frame[page_num] > 0) {
      uint32_t frame_index = pager->page_to_wal_frame[page_num] - 1;
      off_t offset =
          frame_index * (sizeof(WalFrameHeader) + PAGE_SIZE) +
          sizeof(WalFrameHeader);
      lseek(pager->wal_file_descriptor, offset, SEEK_SET);
      ssize_t bytes_read = read(pager->wal_file_descriptor, page, PAGE_SIZE);
      pthread_mutex_unlock(&pager->wal_mutex);
      if (bytes_read == -1) {
        printf("Error reading WAL file\n");
        free(page);
        exit(EXIT_FAILURE);
      }
    } else {
      pthread_mutex_unlock(&pager->wal_mutex);
      if (page_num < num_pages) {
        lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
        ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
        if (bytes_read == -1) {
          printf("Error reading file: %d\n", errno);
          free(page);
          exit(EXIT_FAILURE);
        } else if ((uint32_t)bytes_read < PAGE_SIZE) {
          memset((char*)page + bytes_read, 0, PAGE_SIZE - (uint32_t)bytes_read);
        }
      } else {
        memset(page, 0, PAGE_SIZE);
      }
    }

    pager->pages[page_num] = page;

    if (page_num >= pager->num_pages) {
      pager->num_pages = page_num + 1;
    }
  }

  return pager->pages[page_num];
}

uint32_t get_node_max_key(Table* table, void* node) {
  if (get_node_type(node) == NODE_LEAF) {
    uint32_t n = *leaf_node_num_cells(node);
    if (n == 0) {
      return 0;
    }
    return *tbl_leaf_key(table, node, n - 1);
  }
  void* right_child = get_page(table->pager, *internal_node_right_child(node));
  return get_node_max_key(table, right_child);
}

void print_constants() {
  Table sample;
  memset(&sample, 0, sizeof(sample));
  sample.value_size = CATALOG_VALUE_SIZE;
  sample.cell_size = LEAF_NODE_KEY_SIZE + CATALOG_VALUE_SIZE;
  sample.max_cells = LEAF_NODE_SPACE_FOR_CELLS / sample.cell_size;
  printf("CATALOG_VALUE_SIZE: %zu\n", (size_t)CATALOG_VALUE_SIZE);
  printf("COMMON_NODE_HEADER_SIZE: %d\n", COMMON_NODE_HEADER_SIZE);
  printf("LEAF_NODE_HEADER_SIZE: %d\n", LEAF_NODE_HEADER_SIZE);
  printf("catalog cell_size: %u\n", sample.cell_size);
  printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", LEAF_NODE_SPACE_FOR_CELLS);
  printf("catalog max_cells: %u\n", sample.max_cells);
}

void indent(uint32_t level) {
  for (uint32_t i = 0; i < level; i++) {
    printf("  ");
  }
}

void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level,
                uint32_t leaf_cell_size) {
  void* node = get_page(pager, page_num);
  uint32_t num_keys, child;

  switch (get_node_type(node)) {
    case (NODE_LEAF):
      num_keys = *leaf_node_num_cells(node);
      indent(indentation_level);
      printf("- leaf (size %d)\n", num_keys);
      for (uint32_t i = 0; i < num_keys; i++) {
        indent(indentation_level + 1);
        printf("- %d\n", *leaf_key_sized(node, i, leaf_cell_size));
      }
      break;
    case (NODE_INTERNAL):
      num_keys = *internal_node_num_keys(node);
      indent(indentation_level);
      printf("- internal (size %d)\n", num_keys);
      if (num_keys > 0) {
        for (uint32_t i = 0; i < num_keys; i++) {
          child = *internal_node_child(node, i);
          print_tree(pager, child, indentation_level + 1, leaf_cell_size);

          indent(indentation_level + 1);
          printf("- key %d\n", *internal_node_key(node, i));
        }
        child = *internal_node_right_child(node);
        print_tree(pager, child, indentation_level + 1, leaf_cell_size);
      }
      break;
  }
}

void catalog_write_value(uint32_t root_page_key, const char* table_name,
                         const char* schema_text, void* dest_value) {
  uint8_t* p = dest_value;
  memcpy(p, &root_page_key, sizeof(uint32_t));
  p += sizeof(uint32_t);
  memset(p, 0, CATALOG_TABLE_NAME_SIZE);
  strncpy((char*)p, table_name, CATALOG_TABLE_NAME_SIZE - 1);
  p += CATALOG_TABLE_NAME_SIZE;
  memset(p, 0, CATALOG_SCHEMA_STORAGE);
  strncpy((char*)p, schema_text, CATALOG_SCHEMA_STORAGE - 1);
}

void catalog_read_value(const void* value, uint32_t* root_page_key,
                        char* table_name_out, char* schema_out,
                        size_t schema_cap) {
  const uint8_t* p = value;
  memcpy(root_page_key, p, sizeof(uint32_t));
  p += sizeof(uint32_t);
  memset(table_name_out, 0, MAX_TABLE_NAME_LENGTH + 1);
  strncpy(table_name_out, (const char*)p, MAX_TABLE_NAME_LENGTH);
  table_name_out[MAX_TABLE_NAME_LENGTH] = '\0';
  p += CATALOG_TABLE_NAME_SIZE;
  if (schema_out != NULL && schema_cap > 0) {
    memset(schema_out, 0, schema_cap);
    strncpy(schema_out, (const char*)p, schema_cap - 1);
  }
}

void initialize_leaf_node(void* node) {
  set_node_type(node, NODE_LEAF);
  set_node_root(node, false);
  *leaf_node_num_cells(node) = 0;
  *leaf_node_next_leaf(node) = 0;  // 0 represents no sibling
}

void initialize_internal_node(void* node) {
  set_node_type(node, NODE_INTERNAL);
  set_node_root(node, false);
  *internal_node_num_keys(node) = 0;
  /*
  Necessary because the root page number is 0; by not initializing an internal 
  node's right child to an invalid page number when initializing the node, we may
  end up with 0 as the node's right child, which makes the node a parent of the root
  */
  *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
  void* node = get_page(table->pager, page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  Cursor* cursor = malloc(sizeof(Cursor));
  cursor->table = table;
  cursor->page_num = page_num;
  cursor->end_of_table = false;

  // Binary search
  uint32_t min_index = 0;
  uint32_t one_past_max_index = num_cells;
  while (one_past_max_index != min_index) {
    uint32_t index = (min_index + one_past_max_index) / 2;
    uint32_t key_at_index = *tbl_leaf_key(table, node, index);
    if (key == key_at_index) {
      cursor->cell_num = index;
      return cursor;
    }
    if (key < key_at_index) {
      one_past_max_index = index;
    } else {
      min_index = index + 1;
    }
  }

  cursor->cell_num = min_index;
  return cursor;
}

uint32_t internal_node_find_child(void* node, uint32_t key) {
  /*
  Return the index of the child which should contain
  the given key.
  */

  uint32_t num_keys = *internal_node_num_keys(node);

  /* Binary search */
  uint32_t min_index = 0;
  uint32_t max_index = num_keys; /* there is one more child than key */

  while (min_index != max_index) {
    uint32_t index = (min_index + max_index) / 2;
    uint32_t key_to_right = *internal_node_key(node, index);
    if (key_to_right >= key) {
      max_index = index;
    } else {
      min_index = index + 1;
    }
  }

  return min_index;
}

Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
  void* node = get_page(table->pager, page_num);

  uint32_t child_index = internal_node_find_child(node, key);
  uint32_t child_num = *internal_node_child(node, child_index);
  void* child = get_page(table->pager, child_num);
  switch (get_node_type(child)) {
    case NODE_LEAF:
      return leaf_node_find(table, child_num, key);
    case NODE_INTERNAL:
      return internal_node_find(table, child_num, key);
    default:
      printf("Corrupt node type.\n");
      exit(EXIT_FAILURE);
  }
}

/*
Return the position of the given key.
If the key is not present, return the position
where it should be inserted
*/
Cursor* table_find(Table* table, uint32_t key) {
  uint32_t root_page_num = table->root_page_num;
  void* root_node = get_page(table->pager, root_page_num);

  if (get_node_type(root_node) == NODE_LEAF) {
    return leaf_node_find(table, root_page_num, key);
  } else {
    return internal_node_find(table, root_page_num, key);
  }
}

Cursor* table_start(Table* table) {
  Cursor* cursor = table_find(table, 0);

  void* node = get_page(table->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);
  cursor->end_of_table = (num_cells == 0);

  return cursor;
}

void internal_node_split_and_insert(Table* table, uint32_t parent_page_num, uint32_t child_page_num);

void* cursor_value(Cursor* cursor) {
  uint32_t page_num = cursor->page_num;
  void* page = get_page(cursor->table->pager, page_num);
  return tbl_leaf_value(cursor->table, page, cursor->cell_num);
}

void cursor_advance(Cursor* cursor) {
  uint32_t page_num = cursor->page_num;
  void* node = get_page(cursor->table->pager, page_num);

  cursor->cell_num += 1;
  if (cursor->cell_num >= (*leaf_node_num_cells(node))) {
    /* Advance to next leaf node */
    uint32_t next_page_num = *leaf_node_next_leaf(node);
    if (next_page_num == 0) {
      /* This was rightmost leaf */
      cursor->end_of_table = true;
    } else {
      cursor->page_num = next_page_num;
      cursor->cell_num = 0;
    }
  }
}

void create_new_root(Table* table, uint32_t right_child_page_num) {
  /*
  Handle splitting the root.
  Old root copied to new page, becomes left child.
  Address of right child passed in.
  Re-initialize root page to contain the new root node.
  New root node points to two children.
  */

  void* root = get_page(table->pager, table->root_page_num);
  void* right_child = get_page(table->pager, right_child_page_num);
  uint32_t left_child_page_num = get_unused_page_num(table->pager);
  void* left_child = get_page(table->pager, left_child_page_num);

  if (get_node_type(root) == NODE_INTERNAL) {
    initialize_internal_node(right_child);
    initialize_internal_node(left_child);
  }

  /* Left child has data copied from old root */
  memcpy(left_child, root, PAGE_SIZE);
  set_node_root(left_child, false);

  if (get_node_type(left_child) == NODE_INTERNAL) {
    void* child;
    for (int i = 0; i < *internal_node_num_keys(left_child); i++) {
      child = get_page(table->pager, *internal_node_child(left_child,i));
      *node_parent(child) = left_child_page_num;
    }
    child = get_page(table->pager, *internal_node_right_child(left_child));
    *node_parent(child) = left_child_page_num;
  }

  /* Root node is a new internal node with one key and two children */
  initialize_internal_node(root);
  set_node_root(root, true);
  *internal_node_num_keys(root) = 1;
  *internal_node_child(root, 0) = left_child_page_num;
  uint32_t left_child_max_key = get_node_max_key(table, left_child);
  *internal_node_key(root, 0) = left_child_max_key;
  *internal_node_right_child(root) = right_child_page_num;
  *node_parent(left_child) = table->root_page_num;
  *node_parent(right_child) = table->root_page_num;
}

void internal_node_split_and_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num);

void internal_node_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num) {
  mark_page_dirty(table->pager, parent_page_num);
  /*
  Add a new child/key pair to parent that corresponds to child
  */

  void* parent = get_page(table->pager, parent_page_num);
  void* child = get_page(table->pager, child_page_num);
  uint32_t child_max_key = get_node_max_key(table, child);
  uint32_t index = internal_node_find_child(parent, child_max_key);

  uint32_t original_num_keys = *internal_node_num_keys(parent);

  if (original_num_keys >= INTERNAL_NODE_MAX_KEYS) {
    internal_node_split_and_insert(table, parent_page_num, child_page_num);
    return;
  }

  uint32_t right_child_page_num = *internal_node_right_child(parent);
  /*
  An internal node with a right child of INVALID_PAGE_NUM is empty
  */
  if (right_child_page_num == INVALID_PAGE_NUM) {
    *internal_node_right_child(parent) = child_page_num;
    return;
  }

  void* right_child = get_page(table->pager, right_child_page_num);
  /*
  If we are already at the max number of cells for a node, we cannot increment
  before splitting. Incrementing without inserting a new key/child pair
  and immediately calling internal_node_split_and_insert has the effect
  of creating a new key at (max_cells + 1) with an uninitialized value
  */
  *internal_node_num_keys(parent) = original_num_keys + 1;

  if (child_max_key > get_node_max_key(table, right_child)) {
    /* Replace right child */
    *internal_node_child_ptr(parent, original_num_keys) = right_child_page_num;
    *internal_node_key(parent, original_num_keys) =
        get_node_max_key(table, right_child);
    *internal_node_right_child(parent) = child_page_num;
  } else {
    /* Make room for the new cell */
    for (uint32_t i = original_num_keys; i > index; i--) {
      void* destination = internal_node_cell(parent, i);
      void* source = internal_node_cell(parent, i - 1);
      memcpy(destination, source, INTERNAL_NODE_CELL_SIZE);
    }
    *internal_node_child_ptr(parent, index) = child_page_num;
    *internal_node_key(parent, index) = child_max_key;
  }
}

void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
  uint32_t old_child_index = internal_node_find_child(node, old_key);
  *internal_node_key(node, old_child_index) = new_key;
}

static uint32_t leaf_node_min_cells(const Table* table) {
  return (table->max_cells + 1) / 2;
}

static uint32_t internal_node_child_page_at(void* node, uint32_t child_index) {
  uint32_t num_keys = *internal_node_num_keys(node);
  if (child_index == num_keys) {
    uint32_t c = *internal_node_right_child(node);
    if (c != INVALID_PAGE_NUM && c >= TABLE_MAX_PAGES) {
      printf("Right child out of bounds: %u\n", c);
      exit(EXIT_FAILURE);
    }
    return c;
  }
  return *internal_node_child(node, child_index);
}

static bool internal_node_find_child_index(void* node, uint32_t child_page_num,
                                           uint32_t* child_index_out) {
  uint32_t num_keys = *internal_node_num_keys(node);
  for (uint32_t i = 0; i < num_keys; i++) {
    if (*internal_node_child(node, i) == child_page_num) {
      *child_index_out = i;
      return true;
    }
  }
  if (*internal_node_right_child(node) == child_page_num) {
    *child_index_out = num_keys;
    return true;
  }
  return false;
}

static void internal_node_recompute_keys(Table* table, void* node) {
  uint32_t num_keys = *internal_node_num_keys(node);
  for (uint32_t i = 0; i < num_keys; i++) {
    uint32_t child_page_num = *internal_node_child(node, i);
    void* child = get_page(table->pager, child_page_num);
    *internal_node_key(node, i) = get_node_max_key(table, child);
  }
}

void internal_rebalance_after_delete(Table* table, uint32_t internal_page_num);

static void internal_node_remove_child(Table* table, uint32_t parent_page_num,
                                       uint32_t child_index) {
  void* parent = get_page(table->pager, parent_page_num);
  uint32_t num_keys = *internal_node_num_keys(parent);

  if (child_index > num_keys) {
    return;
  }

  mark_page_dirty(table->pager, parent_page_num);

  if (child_index == num_keys) {
    if (num_keys == 0) {
      *internal_node_right_child(parent) = INVALID_PAGE_NUM;
    } else {
      *internal_node_right_child(parent) = *internal_node_child(parent, num_keys - 1);
      *internal_node_num_keys(parent) = num_keys - 1;
    }
  } else {
    for (uint32_t i = child_index; i + 1 < num_keys; i++) {
      memmove(internal_node_cell(parent, i), internal_node_cell(parent, i + 1),
              INTERNAL_NODE_CELL_SIZE);
    }
    *internal_node_num_keys(parent) = num_keys - 1;
  }

  internal_node_recompute_keys(table, parent);
  internal_rebalance_after_delete(table, parent_page_num);
}

void internal_rebalance_after_delete(Table* table, uint32_t internal_page_num) {
  void* node = get_page(table->pager, internal_page_num);
  if (get_node_type(node) != NODE_INTERNAL) {
    return;
  }

  uint32_t num_keys = *internal_node_num_keys(node);
  if (is_node_root(node)) {
    if (num_keys != 0) {
      return;
    }

    uint32_t child_page_num = *internal_node_right_child(node);
    if (child_page_num == INVALID_PAGE_NUM) {
      return;
    }

    void* child = get_page(table->pager, child_page_num);
    memcpy(node, child, PAGE_SIZE);
    set_node_root(node, true);
    mark_page_dirty(table->pager, internal_page_num);
    mark_page_dirty(table->pager, child_page_num);
    /* child_page_num is no longer referenced after root collapse */
    pager_free_page(table->pager, child_page_num);

    if (get_node_type(node) == NODE_INTERNAL) {
      uint32_t root_num_keys = *internal_node_num_keys(node);
      for (uint32_t i = 0; i <= root_num_keys; i++) {
        uint32_t descendant_page_num = internal_node_child_page_at(node, i);
        void* descendant = get_page(table->pager, descendant_page_num);
        *node_parent(descendant) = internal_page_num;
        mark_page_dirty(table->pager, descendant_page_num);
      }
    }
    return;
  }

  if (num_keys != 0) {
    return;
  }

  uint32_t child_page_num = *internal_node_right_child(node);
  if (child_page_num == INVALID_PAGE_NUM) {
    return;
  }

  uint32_t parent_page_num = *node_parent(node);
  void* parent = get_page(table->pager, parent_page_num);
  uint32_t child_index;
  if (!internal_node_find_child_index(parent, internal_page_num, &child_index)) {
    return;
  }

  if (child_index == *internal_node_num_keys(parent)) {
    *internal_node_right_child(parent) = child_page_num;
  } else {
    *internal_node_child(parent, child_index) = child_page_num;
  }

  void* child = get_page(table->pager, child_page_num);
  *node_parent(child) = parent_page_num;
  mark_page_dirty(table->pager, parent_page_num);
  mark_page_dirty(table->pager, child_page_num);
  mark_page_dirty(table->pager, internal_page_num);
  internal_node_recompute_keys(table, parent);
  /* internal_page_num is now unreachable */
  pager_free_page(table->pager, internal_page_num);
}

static void rebalance_leaf_after_delete(Cursor* cursor) {
  Table* table = cursor->table;
  uint32_t page_num = cursor->page_num;
  void* node = get_page(table->pager, page_num);

  if (is_node_root(node)) {
    return;
  }

  uint32_t parent_page_num = *node_parent(node);
  void* parent = get_page(table->pager, parent_page_num);
  uint32_t child_index;
  if (!internal_node_find_child_index(parent, page_num, &child_index)) {
    return;
  }

  uint32_t min_cells = leaf_node_min_cells(table);
  uint32_t num_cells = *leaf_node_num_cells(node);
  uint32_t parent_num_keys = *internal_node_num_keys(parent);

  if (num_cells >= min_cells) {
    internal_node_recompute_keys(table, parent);
    mark_page_dirty(table->pager, parent_page_num);
    return;
  }

  if (child_index > 0) {
    uint32_t left_page_num = internal_node_child_page_at(parent, child_index - 1);
    void* left_node = get_page(table->pager, left_page_num);
    if (get_node_type(left_node) != NODE_LEAF) {
      // Tree is not perfectly height-balanced; don't corrupt by treating an internal node as a leaf.
      internal_node_recompute_keys(table, parent);
      mark_page_dirty(table->pager, parent_page_num);
      return;
    }
    uint32_t left_cells = *leaf_node_num_cells(left_node);

    if (left_cells > min_cells) {
      memmove(tbl_leaf_cell(table, node, 1), tbl_leaf_cell(table, node, 0),
              num_cells * table->cell_size);
      memcpy(tbl_leaf_cell(table, node, 0),
             tbl_leaf_cell(table, left_node, left_cells - 1), table->cell_size);
      *leaf_node_num_cells(left_node) = left_cells - 1;
      *leaf_node_num_cells(node) = num_cells + 1;
      mark_page_dirty(table->pager, left_page_num);
      mark_page_dirty(table->pager, page_num);
      internal_node_recompute_keys(table, parent);
      mark_page_dirty(table->pager, parent_page_num);
      return;
    }
  }

  if (child_index < parent_num_keys) {
    uint32_t right_page_num = internal_node_child_page_at(parent, child_index + 1);
    void* right_node = get_page(table->pager, right_page_num);
    if (get_node_type(right_node) != NODE_LEAF) {
      // Tree is not perfectly height-balanced; don't corrupt by treating an internal node as a leaf.
      internal_node_recompute_keys(table, parent);
      mark_page_dirty(table->pager, parent_page_num);
      return;
    }
    uint32_t right_cells = *leaf_node_num_cells(right_node);

    if (right_cells > min_cells) {
      memcpy(tbl_leaf_cell(table, node, num_cells), tbl_leaf_cell(table, right_node, 0),
             table->cell_size);
      memmove(tbl_leaf_cell(table, right_node, 0), tbl_leaf_cell(table, right_node, 1),
              (right_cells - 1) * table->cell_size);
      *leaf_node_num_cells(right_node) = right_cells - 1;
      *leaf_node_num_cells(node) = num_cells + 1;
      mark_page_dirty(table->pager, right_page_num);
      mark_page_dirty(table->pager, page_num);
      internal_node_recompute_keys(table, parent);
      mark_page_dirty(table->pager, parent_page_num);
      return;
    }
  }

  if (child_index > 0) {
    uint32_t left_page_num = internal_node_child_page_at(parent, child_index - 1);
    void* left_node = get_page(table->pager, left_page_num);
    uint32_t left_cells = *leaf_node_num_cells(left_node);

    for (uint32_t i = 0; i < num_cells; i++) {
      memcpy(tbl_leaf_cell(table, left_node, left_cells + i),
             tbl_leaf_cell(table, node, i), table->cell_size);
    }
    *leaf_node_num_cells(left_node) = left_cells + num_cells;
    *leaf_node_next_leaf(left_node) = *leaf_node_next_leaf(node);
    *leaf_node_num_cells(node) = 0;
    mark_page_dirty(table->pager, left_page_num);
    mark_page_dirty(table->pager, page_num);
    internal_node_remove_child(table, parent_page_num, child_index);
    pager_free_page(table->pager, page_num);
    cursor->page_num = left_page_num;
    cursor->cell_num = left_cells;
    return;
  }

  if (child_index < parent_num_keys) {
    uint32_t right_page_num = internal_node_child_page_at(parent, child_index + 1);
    void* right_node = get_page(table->pager, right_page_num);
    uint32_t right_cells = *leaf_node_num_cells(right_node);

    for (uint32_t i = 0; i < right_cells; i++) {
      memcpy(tbl_leaf_cell(table, node, num_cells + i),
             tbl_leaf_cell(table, right_node, i), table->cell_size);
    }
    *leaf_node_num_cells(node) = num_cells + right_cells;
    *leaf_node_next_leaf(node) = *leaf_node_next_leaf(right_node);
    *leaf_node_num_cells(right_node) = 0;
    mark_page_dirty(table->pager, right_page_num);
    mark_page_dirty(table->pager, page_num);
    internal_node_remove_child(table, parent_page_num, child_index + 1);
    pager_free_page(table->pager, right_page_num);
  }
}

void internal_node_split_and_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num) {
  mark_page_dirty(table->pager, parent_page_num);
  uint32_t old_page_num = parent_page_num;
  void* old_node = get_page(table->pager,parent_page_num);
  uint32_t old_max = get_node_max_key(table, old_node);

  void* child = get_page(table->pager, child_page_num); 
  uint32_t child_max = get_node_max_key(table, child);

  uint32_t new_page_num = get_unused_page_num(table->pager);

  /*
  Declaring a flag before updating pointers which
  records whether this operation involves splitting the root -
  if it does, we will insert our newly created node during
  the step where the table's new root is created. If it does
  not, we have to insert the newly created node into its parent
  after the old node's keys have been transferred over. We are not
  able to do this if the newly created node's parent is not a newly
  initialized root node, because in that case its parent may have existing
  keys aside from our old node which we are splitting. If that is true, we
  need to find a place for our newly created node in its parent, and we
  cannot insert it at the correct index if it does not yet have any keys
  */
  uint32_t splitting_root = is_node_root(old_node);

  void* parent;
  void* new_node;
  if (splitting_root) {
    create_new_root(table, new_page_num);
    parent = get_page(table->pager,table->root_page_num);
    /*
    If we are splitting the root, we need to update old_node to point
    to the new root's left child, new_page_num will already point to
    the new root's right child
    */
    old_page_num = *internal_node_child(parent,0);
    old_node = get_page(table->pager, old_page_num);
  } else {
    parent = get_page(table->pager,*node_parent(old_node));
    new_node = get_page(table->pager, new_page_num);
    initialize_internal_node(new_node);
  }
  
  uint32_t* old_num_keys = internal_node_num_keys(old_node);

  uint32_t cur_page_num = *internal_node_right_child(old_node);
  void* cur = get_page(table->pager, cur_page_num);

  /*
  First put right child into new node and set right child of old node to invalid page number
  */
  internal_node_insert(table, new_page_num, cur_page_num);
  *node_parent(cur) = new_page_num;
  *internal_node_right_child(old_node) = INVALID_PAGE_NUM;
  /*
  For each key until you get to the middle key, move the key and the child to the new node
  */
  for (int i = INTERNAL_NODE_MAX_KEYS - 1; i > INTERNAL_NODE_MAX_KEYS / 2; i--) {
    cur_page_num = *internal_node_child(old_node, i);
    cur = get_page(table->pager, cur_page_num);

    internal_node_insert(table, new_page_num, cur_page_num);
    *node_parent(cur) = new_page_num;

    (*old_num_keys)--;
  }

  /*
  Set child before middle key, which is now the highest key, to be node's right child,
  and decrement number of keys
  */
  *internal_node_right_child(old_node) = *internal_node_child(old_node,*old_num_keys - 1);
  (*old_num_keys)--;

  /*
  Determine which of the two nodes after the split should contain the child to be inserted,
  and insert the child
  */
  uint32_t max_after_split = get_node_max_key(table, old_node);

  uint32_t destination_page_num = child_max < max_after_split ? old_page_num : new_page_num;

  internal_node_insert(table, destination_page_num, child_page_num);
  *node_parent(child) = destination_page_num;

  update_internal_node_key(parent, old_max, get_node_max_key(table, old_node));

  if (!splitting_root) {
    internal_node_insert(table,*node_parent(old_node),new_page_num);
    *node_parent(new_node) = *node_parent(old_node);
  }
}

void leaf_node_split_and_insert(Cursor* cursor, uint32_t key,
                                const void* value) {
  mark_page_dirty(cursor->table->pager, cursor->page_num);
  Table* t = cursor->table;
  uint32_t max_c = t->max_cells;
  uint32_t right_split = (max_c + 1) / 2;
  uint32_t left_split = (max_c + 1) - right_split;

  void* old_node = get_page(t->pager, cursor->page_num);
  uint32_t old_max = get_node_max_key(t, old_node);
  uint32_t new_page_num = get_unused_page_num(t->pager);
  void* new_node = get_page(t->pager, new_page_num);
  initialize_leaf_node(new_node);
  *node_parent(new_node) = *node_parent(old_node);
  *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
  *leaf_node_next_leaf(old_node) = new_page_num;

  for (int32_t i = (int32_t)max_c; i >= 0; i--) {
    void* destination_node;
    if ((uint32_t)i >= left_split) {
      destination_node = new_node;
    } else {
      destination_node = old_node;
    }
    uint32_t index_within_node =
        (destination_node == new_node) ? ((uint32_t)i - left_split) : (uint32_t)i;
    void* destination = tbl_leaf_cell(t, destination_node, index_within_node);

    if ((uint32_t)i == cursor->cell_num) {
      memcpy(tbl_leaf_value(t, destination_node, index_within_node), value,
             t->value_size);
      *tbl_leaf_key(t, destination_node, index_within_node) = key;
    } else if ((uint32_t)i > cursor->cell_num) {
      memcpy(destination, tbl_leaf_cell(t, old_node, (uint32_t)i - 1),
             t->cell_size);
    } else {
      memcpy(destination, tbl_leaf_cell(t, old_node, (uint32_t)i),
             t->cell_size);
    }
  }

  *(leaf_node_num_cells(old_node)) = left_split;
  *(leaf_node_num_cells(new_node)) = right_split;

  if (is_node_root(old_node)) {
    create_new_root(t, new_page_num);
    return;
  }
  uint32_t parent_page_num = *node_parent(old_node);
  uint32_t new_max = get_node_max_key(t, old_node);
  void* parent = get_page(t->pager, parent_page_num);

  update_internal_node_key(parent, old_max, new_max);
  internal_node_insert(t, parent_page_num, new_page_num);
}

void leaf_node_delete(Cursor* cursor) {
  uint32_t page_num = cursor->page_num;
  void* node = get_page(cursor->table->pager, page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  if (num_cells == 0 || cursor->cell_num >= num_cells) {
    return;
  }

  if (cursor->cell_num < num_cells - 1) {
    void* dest = tbl_leaf_cell(cursor->table, node, cursor->cell_num);
    void* src = tbl_leaf_cell(cursor->table, node, cursor->cell_num + 1);
    size_t bytes_to_move = (num_cells - cursor->cell_num - 1) * cursor->table->cell_size;
    memmove(dest, src, bytes_to_move);
  }

  *leaf_node_num_cells(node) -= 1;
  mark_page_dirty(cursor->table->pager, page_num);
  rebalance_leaf_after_delete(cursor);
}

void leaf_node_insert(Cursor* cursor, uint32_t key, const void* value) {
  mark_page_dirty(cursor->table->pager, cursor->page_num);
  Table* t = cursor->table;
  void* node = get_page(t->pager, cursor->page_num);

  uint32_t num_cells = *leaf_node_num_cells(node);
  if (num_cells >= t->max_cells) {
    leaf_node_split_and_insert(cursor, key, value);
    return;
  }

  if (cursor->cell_num < num_cells) {
    for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
      memcpy(tbl_leaf_cell(t, node, i), tbl_leaf_cell(t, node, i - 1),
             t->cell_size);
    }
  }

  *(leaf_node_num_cells(node)) += 1;
  *tbl_leaf_key(t, node, cursor->cell_num) = key;
  memcpy(tbl_leaf_value(t, node, cursor->cell_num), value, t->value_size);
}

ExecuteResult insert_row_into_table(Table* table, uint32_t key,
                                    const void* value) {
  Cursor* cursor = table_find(table, key);

  void* node = get_page(table->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  if (cursor->cell_num < num_cells) {
    uint32_t key_at_index = *tbl_leaf_key(table, node, cursor->cell_num);
    if (key_at_index == key) {
      free(cursor);
      return EXECUTE_DUPLICATE_KEY;
    }
  }

  leaf_node_insert(cursor, key, value);
  free(cursor);
  return EXECUTE_SUCCESS;
}

