#ifndef SQLYT_H
#define SQLYT_H

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  char* buffer;
  size_t buffer_length;
  ssize_t input_length;
} InputBuffer;

typedef char* (*ReadlineGeneratorFn)(const char*, int);
typedef char** (*ReadlineCompletionMatchesFn)(const char*, ReadlineGeneratorFn);
typedef char** (*ReadlineCompletionFn)(const char*, int, int);

typedef struct {
  bool initialized;
  bool enabled;
  void* handle;
  char* (*readline_fn)(const char*);
  void (*add_history_fn)(const char*);
  void (*using_history_fn)(void);
  void (*stifle_history_fn)(int);
  ReadlineCompletionMatchesFn completion_matches_fn;
  ReadlineCompletionFn* attempted_completion_slot;
} ReadlineApi;

extern ReadlineApi g_readline_api;
bool init_readline_if_needed(void);

typedef enum {
  EXECUTE_SUCCESS,
  EXECUTE_DUPLICATE_KEY,
} ExecuteResult;

typedef enum {
  META_COMMAND_SUCCESS,
  META_COMMAND_UNRECOGNIZED_COMMAND,
  META_COMMAND_EXIT
} MetaCommandResult;

#define DEFAULT_ROOT_PATH "./data"
#define DATABASE_FILE_NAME "database.db"
#define MAX_TABLE_NAME_LENGTH 28
#define MAX_SCHEMA_COLUMNS 10
#define MAX_COLUMN_NAME_LENGTH 24
#define MAX_CELL_TEXT 64
#define COLUMN_TYPE_INT 1
#define COLUMN_TYPE_TEXT 2
#define DB_HEADER_MAGIC "SQLYTDB1"
#define DB_FORMAT_VERSION 3
#define DB_HEADER_MASTER_ROOT_PAGE 1
#define DB_HEADER_FIRST_USER_ROOT_PAGE 2

/* Catalog (sqlyt_master) row: btree key = root_page; value layout is fixed. */
#define CATALOG_TABLE_NAME_SIZE 32
#define CATALOG_SCHEMA_STORAGE 512
#define CATALOG_VALUE_SIZE \
  (sizeof(uint32_t) + CATALOG_TABLE_NAME_SIZE + CATALOG_SCHEMA_STORAGE)
#define SCHEMA_TEXT_MAX CATALOG_SCHEMA_STORAGE
#define SQL_LINE_BUF 1024
#define MAX_INSERT_ROWS 32

typedef struct {
  uint32_t column_count;
  char column_names[MAX_SCHEMA_COLUMNS][MAX_COLUMN_NAME_LENGTH + 1];
  uint32_t column_type[MAX_SCHEMA_COLUMNS];
  uint32_t column_max[MAX_SCHEMA_COLUMNS];
  char create_sql[SCHEMA_TEXT_MAX + 1];
} SqlSchema;

typedef struct {
  uint32_t value_size;
  uint32_t col_offset[MAX_SCHEMA_COLUMNS];
  uint32_t col_size[MAX_SCHEMA_COLUMNS];
} RowLayout;

typedef enum {
  SQL_STMT_CREATE_DATABASE,
  SQL_STMT_CREATE_TABLE,
  SQL_STMT_INSERT,
  SQL_STMT_SELECT,
  SQL_STMT_DELETE,
  SQL_STMT_UPDATE,
  SQL_STMT_DROP_TABLE
} SqlStatementType;

typedef struct Table Table;
typedef struct Cursor Cursor;

typedef struct {
  SqlStatementType type;
  char table_name[MAX_TABLE_NAME_LENGTH + 1];
  uint32_t id;
  char update_column[MAX_COLUMN_NAME_LENGTH + 1];
  char update_value[256];
  uint32_t row_count;
  uint32_t row_value_count[MAX_INSERT_ROWS];
  uint32_t row_ids[MAX_INSERT_ROWS];
  bool row_auto_id[MAX_INSERT_ROWS];
  char values[MAX_INSERT_ROWS][MAX_SCHEMA_COLUMNS][256];
  SqlSchema schema;
} SqlStatement;

typedef enum {
  SQL_EXEC_OK,
  SQL_EXEC_CREATE_DB_FAILED,
  SQL_EXEC_RESERVED_TABLE_NAME,
  SQL_EXEC_TABLE_EXISTS,
  SQL_EXEC_ROW_LAYOUT_TOO_LARGE,
  SQL_EXEC_TABLE_NAME_TOO_LONG,
  SQL_EXEC_SCHEMA_METADATA_TOO_LONG,
  SQL_EXEC_DUPLICATE_KEY,
  SQL_EXEC_TABLE_NOT_FOUND,
  SQL_EXEC_WRONG_VALUE_COUNT,
  SQL_EXEC_SCHEMA_INVALID_PK,
  SQL_EXEC_INSERT_VALIDATION_FAILED,
  SQL_EXEC_ROW_PACK_FAILED,
  SQL_EXEC_SELECT_LAYOUT_INVALID,
  SQL_EXEC_DELETE_RECORD_NOT_FOUND,
  SQL_EXEC_UPDATE_COLUMN_NOT_FOUND,
  SQL_EXEC_UPDATE_PK_NOT_ALLOWED,
  SQL_EXEC_UPDATE_RECORD_NOT_FOUND,
  SQL_EXEC_RESERVED_DROP_TABLE,
} SqlExecuteResult;

typedef struct {
  char root_path[512];
  bool has_active_database;
  char active_database[MAX_TABLE_NAME_LENGTH + 1];
  Table* database;
} Session;

typedef struct {
  bool quiet;          /* suppress prompts and normal output */
  bool timer_enabled;  /* print per-statement elapsed time */
  bool in_transaction; /* defer commit until .commit */
} RunOptions;

extern RunOptions g_run_options;

#define PAGE_SIZE 4096
#define TABLE_MAX_PAGES 2000
#define INVALID_PAGE_NUM UINT32_MAX
#define WAL_CHECKPOINT_THRESHOLD 100

typedef struct {
  uint32_t page_num;
  uint32_t is_commit;
} WalFrameHeader;

typedef struct {
  int file_descriptor;
  uint32_t file_length;
  uint32_t num_pages;
  uint32_t master_root_page;
  uint32_t next_root_page;
  uint32_t free_page_head;
  void* pages[TABLE_MAX_PAGES];
  int wal_file_descriptor;
  uint32_t page_to_wal_frame[TABLE_MAX_PAGES];
  uint32_t wal_frame_count;
  uint8_t page_dirty[TABLE_MAX_PAGES];
  bool checkpoint_in_progress;
  pthread_mutex_t wal_mutex;
} Pager;

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t master_root_page;
  uint32_t next_root_page;
  /* Singly-linked list of free pages (0 = none). Stored inside free pages. */
  uint32_t free_page_head;
} DbFileHeader;

struct Table {
  Pager* pager;
  uint32_t root_page_num;
  bool is_catalog;
  uint32_t value_size;
  uint32_t cell_size;
  uint32_t max_cells;
  SqlSchema schema;
  RowLayout row_layout;
};

struct Cursor {
  Table* table;
  uint32_t page_num;
  uint32_t cell_num;
  bool end_of_table;
};

typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

extern const uint32_t NODE_TYPE_SIZE;
extern const uint32_t NODE_TYPE_OFFSET;
extern const uint32_t IS_ROOT_SIZE;
extern const uint32_t IS_ROOT_OFFSET;
extern const uint32_t PARENT_POINTER_SIZE;
extern const uint32_t PARENT_POINTER_OFFSET;
extern const uint8_t COMMON_NODE_HEADER_SIZE;
extern const uint32_t INTERNAL_NODE_NUM_KEYS_SIZE;
extern const uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET;
extern const uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE;
extern const uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET;
extern const uint32_t INTERNAL_NODE_HEADER_SIZE;
extern const uint32_t INTERNAL_NODE_KEY_SIZE;
extern const uint32_t INTERNAL_NODE_CHILD_SIZE;
extern const uint32_t INTERNAL_NODE_CELL_SIZE;
extern const uint32_t INTERNAL_NODE_MAX_KEYS;
extern const uint32_t LEAF_NODE_NUM_CELLS_SIZE;
extern const uint32_t LEAF_NODE_NUM_CELLS_OFFSET;
extern const uint32_t LEAF_NODE_NEXT_LEAF_SIZE;
extern const uint32_t LEAF_NODE_NEXT_LEAF_OFFSET;
extern const uint32_t LEAF_NODE_HEADER_SIZE;
extern const uint32_t LEAF_NODE_KEY_SIZE;
extern const uint32_t LEAF_NODE_KEY_OFFSET;

#define LEAF_NODE_SPACE_FOR_CELLS (PAGE_SIZE - LEAF_NODE_HEADER_SIZE)

void mark_page_dirty(Pager* pager, uint32_t page_num);
NodeType get_node_type(void* node);
void set_node_type(void* node, NodeType type);
bool is_node_root(void* node);
void set_node_root(void* node, bool is_root);
uint32_t* node_parent(void* node);
uint32_t* internal_node_num_keys(void* node);
uint32_t* internal_node_right_child(void* node);
uint32_t* internal_node_cell(void* node, uint32_t cell_num);
uint32_t* internal_node_child(void* node, uint32_t child_num);
uint32_t* internal_node_key(void* node, uint32_t key_num);
uint32_t* leaf_node_num_cells(void* node);
uint32_t* leaf_node_next_leaf(void* node);
void table_init_catalog(Table* t, Pager* pager);
bool compute_row_layout(const SqlSchema* schema, RowLayout* L);
bool table_init_user(Table* t, Pager* pager, uint32_t root_page,
                     const SqlSchema* schema);
uint32_t* tbl_leaf_key(Table* t, void* node, uint32_t cell_num);
void* tbl_leaf_value(Table* t, void* node, uint32_t cell_num);
void* get_page(Pager* pager, uint32_t page_num);
uint32_t get_node_max_key(Table* table, void* node);
void print_constants(void);
void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level,
                uint32_t leaf_cell_size);
void catalog_write_value(uint32_t root_page_key, const char* table_name,
                         const char* schema_text, void* dest_value);
void catalog_read_value(const void* value, uint32_t* root_page_key,
                        char* table_name_out, char* schema_out,
                        size_t schema_cap);
bool master_find_table(Table* db, const char* table_name, SqlSchema* schema_out,
                       uint32_t* root_page_out);
void initialize_leaf_node(void* node);
void initialize_internal_node(void* node);
Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key);
Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key);
Cursor* table_find(Table* table, uint32_t key);
Cursor* table_start(Table* table);
void* cursor_value(Cursor* cursor);
void cursor_advance(Cursor* cursor);
void create_new_root(Table* table, uint32_t right_child_page_num);
void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key);
void internal_rebalance_after_delete(Table* table, uint32_t internal_page_num);
void leaf_node_delete(Cursor* cursor);
void leaf_node_insert(Cursor* cursor, uint32_t key, const void* value);
ExecuteResult insert_row_into_table(Table* table, uint32_t key,
                                    const void* row_value);

Pager* pager_open(const char* filename);
bool header_is_valid(const DbFileHeader* header);
uint32_t recover_next_root_page_from_master(Pager* pager,
                                            uint32_t master_root_page);
Table* db_open(const char* filename);
void pager_flush(Pager* pager, uint32_t page_num, uint32_t is_commit);
void* background_checkpoint_task(void* arg);
void pager_checkpoint(Pager* pager);
void pager_commit_transaction_sync(Pager* pager);
void db_close(Table* table);
uint32_t get_unused_page_num(Pager* pager);
void pager_free_page(Pager* pager, uint32_t page_num);

bool ensure_directory(const char* path);
bool build_path(char* out, size_t out_size, const char* left,
                const char* right);
bool is_valid_identifier(const char* name, size_t max_len);
bool parse_u32(const char* text, uint32_t* value);
size_t bounded_text_len(const char* text, size_t max_len);
void print_table_border(const size_t* widths, uint32_t count);
void print_table_cell_text(const char* text, size_t width, bool right_align);
bool split_schema_payload(const char* payload, SqlSchema* schema);
bool make_schema_payload(const SqlSchema* schema, char* out, size_t out_size);
bool pack_insert_row_values(const char values[MAX_SCHEMA_COLUMNS][256],
                            const SqlSchema* schema, const RowLayout* L,
                            uint8_t* out);
bool parse_u32_tokens_for_insert_values(
    const char values[MAX_SCHEMA_COLUMNS][256], const SqlSchema* schema,
    uint32_t key);
void print_user_row(const void* value, const SqlSchema* s, const RowLayout* L);
uint32_t find_table_max_key(Table* table, bool* has_rows);
bool table_has_key(Table* table, uint32_t key);
uint32_t allocate_next_root_page(Pager* pager);
bool create_table_root(Table* db, uint32_t root_page);
int split_tokens_sql(char* input, char* tokens[], int max_tokens);
bool parse_create_table(char* line, SqlStatement* out);
bool parse_insert_into(char* line, SqlStatement* out);
bool parse_create_database(char* line, SqlStatement* out);
bool parse_select_from(char* line, SqlStatement* out);
bool parse_delete_from(char* line, SqlStatement* out);
bool parse_update_table(char* line, SqlStatement* out);
bool parse_drop_table(char* line, SqlStatement* out);
bool parse_sql_statement(char* line, SqlStatement* out);

InputBuffer* new_input_buffer(void);
void print_prompt(void);
void read_input(InputBuffer* input_buffer);
void close_input_buffer(InputBuffer* input_buffer);
bool switch_database(Session* session, const char* db_name);
void list_databases(const char* root_path);
void show_tables(Table* db);
bool create_database(Session* session, const char* db_name);
SqlExecuteResult execute_create_database(Session* session,
                                         const SqlStatement* stmt);
SqlExecuteResult execute_create_table(Table* db, const SqlStatement* stmt);
SqlExecuteResult execute_insert(Table* db, const SqlStatement* stmt);
SqlExecuteResult execute_select(Table* db, const SqlStatement* stmt);
SqlExecuteResult execute_delete(Table* db, const SqlStatement* stmt);
SqlExecuteResult execute_update(Table* db, const SqlStatement* stmt);
SqlExecuteResult execute_drop_table(Table* db, const SqlStatement* stmt);
SqlExecuteResult execute_statement(Session* session, const SqlStatement* stmt);
void execute_sql(Session* session, const SqlStatement* stmt);
MetaCommandResult do_meta_command(Session* session, InputBuffer* input_buffer);

#endif
