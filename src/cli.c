#include "sqlyt.h"

RunOptions g_run_options = {0};

InputBuffer* new_input_buffer() {
  InputBuffer* input_buffer = malloc(sizeof(InputBuffer));
  input_buffer->buffer = NULL;
  input_buffer->buffer_length = 0;
  input_buffer->input_length = 0;

  return input_buffer;
}

void print_prompt() { printf("db > "); }

void read_input(InputBuffer* input_buffer) {
  if (isatty(STDIN_FILENO) && init_readline_if_needed()) {
    char* line;
    line = g_readline_api.readline_fn(g_run_options.quiet ? "" : "db > ");
    if (line == NULL) {
      free(input_buffer->buffer);
      input_buffer->buffer = strdup(".exit");
      if (input_buffer->buffer == NULL) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
      }
      input_buffer->input_length = 5;
      input_buffer->buffer_length = 6;
      return;
    }
    if (!g_run_options.quiet && line[0] != '\0') {
      g_readline_api.add_history_fn(line);
    }
    free(input_buffer->buffer);
    input_buffer->buffer = line;
    input_buffer->input_length = (ssize_t)strlen(line);
    input_buffer->buffer_length = (size_t)input_buffer->input_length + 1;
    return;
  }

  if (isatty(STDIN_FILENO)) {
    if (!g_run_options.quiet) {
      print_prompt();
      fflush(stdout);
    }
  }

  ssize_t bytes_read =
      getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);

  if (bytes_read <= 0) {
    printf("Error reading input\n");
    exit(EXIT_FAILURE);
  }

  // Ignore trailing newline
  input_buffer->input_length = bytes_read - 1;
  input_buffer->buffer[bytes_read - 1] = 0;
}

void close_input_buffer(InputBuffer* input_buffer) {
  free(input_buffer->buffer);
  free(input_buffer);
}

bool switch_database(Session* session, const char* db_name) {
  char db_dir[1024];
  char db_file[1024];

  if (!is_valid_identifier(db_name, MAX_TABLE_NAME_LENGTH)) {
    return false;
  }
  if (!build_path(db_dir, sizeof(db_dir), session->root_path, db_name)) {
    return false;
  }
  struct stat st;
  if (stat(db_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return false;
  }
  if (!build_path(db_file, sizeof(db_file), db_dir, DATABASE_FILE_NAME)) {
    return false;
  }

  if (session->database != NULL) {
    db_close(session->database);
    session->database = NULL;
  }
  session->database = db_open(db_file);
  if (session->database == NULL) {
    return false;
  }

  strcpy(session->active_database, db_name);
  session->has_active_database = true;
  return true;
}

void list_databases(const char* root_path) {
  DIR* dir = opendir(root_path);
  struct dirent* entry;
  if (dir == NULL) {
    printf("Unable to open root path.\n");
    return;
  }

  while ((entry = readdir(dir)) != NULL) {
    char path[1024];
    struct stat st;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (!build_path(path, sizeof(path), root_path, entry->d_name)) {
      continue;
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      printf("%s\n", entry->d_name);
    }
  }
  closedir(dir);
}

void show_tables(Table* db) {
  Cursor* cursor;
  Table master;
  char name_buf[MAX_TABLE_NAME_LENGTH + 1];
  char schema_buf[SCHEMA_TEXT_MAX + 1];

  table_init_catalog(&master, db->pager);
  master.root_page_num = db->pager->master_root_page;

  cursor = table_start(&master);
  while (!cursor->end_of_table) {
    void* node = get_page(db->pager, cursor->page_num);
    uint32_t rp = *tbl_leaf_key(&master, node, cursor->cell_num);
    uint32_t dummy;
    catalog_read_value(tbl_leaf_value(&master, node, cursor->cell_num), &dummy,
                       name_buf, schema_buf, sizeof(schema_buf));
    printf("%s (root_page=%u)\n", name_buf, rp);
    cursor_advance(cursor);
  }
  free(cursor);
}

bool create_database(Session* session, const char* db_name) {
  char db_dir[1024];
  if (!is_valid_identifier(db_name, MAX_TABLE_NAME_LENGTH)) {
    return false;
  }
  if (!build_path(db_dir, sizeof(db_dir), session->root_path, db_name)) {
    return false;
  }
  return ensure_directory(db_dir);
}

SqlExecuteResult execute_create_database(Session* session,
                                         const SqlStatement* stmt) {
  if (!create_database(session, stmt->table_name)) {
    return SQL_EXEC_CREATE_DB_FAILED;
  }
  return SQL_EXEC_OK;
}

SqlExecuteResult execute_create_table(Table* db, const SqlStatement* stmt) {
  SqlSchema existing;
  uint32_t root_page;
  char schema_payload[SCHEMA_TEXT_MAX + 1];
  uint8_t catalog_value[CATALOG_VALUE_SIZE];
  ExecuteResult btree_result;
  RowLayout layout_check;

  if (strcmp(stmt->table_name, "sqlyt_master") == 0) {
    return SQL_EXEC_RESERVED_TABLE_NAME;
  }
  if (master_find_table(db, stmt->table_name, &existing, NULL)) {
    return SQL_EXEC_TABLE_EXISTS;
  }

  if (!compute_row_layout(&stmt->schema, &layout_check)) {
    return SQL_EXEC_ROW_LAYOUT_TOO_LARGE;
  }

  if (strlen(stmt->table_name) >= CATALOG_TABLE_NAME_SIZE) {
    return SQL_EXEC_TABLE_NAME_TOO_LONG;
  }
  if (!make_schema_payload(&stmt->schema, schema_payload,
                           sizeof(schema_payload))) {
    return SQL_EXEC_SCHEMA_METADATA_TOO_LONG;
  }

  root_page = allocate_next_root_page(db->pager);
  create_table_root(db, root_page);

  catalog_write_value(root_page, stmt->table_name, schema_payload,
                      catalog_value);

  btree_result = insert_row_into_table(db, root_page, catalog_value);
  if (btree_result == EXECUTE_DUPLICATE_KEY) {
    return SQL_EXEC_DUPLICATE_KEY;
  }
  return SQL_EXEC_OK;
}

SqlExecuteResult execute_insert(Table* db, const SqlStatement* stmt) {
  SqlSchema schema;
  uint32_t root_page;
  ExecuteResult btree_result;
  Table user_table;
  bool has_rows;
  uint32_t max_key;
  uint32_t planned_keys[MAX_INSERT_ROWS];
  char row_values[MAX_SCHEMA_COLUMNS][256];
  uint8_t row_buf[LEAF_NODE_SPACE_FOR_CELLS];

  if (!master_find_table(db, stmt->table_name, &schema, &root_page)) {
    return SQL_EXEC_TABLE_NOT_FOUND;
  }

  if (schema.column_count == 0 || schema.column_type[0] != COLUMN_TYPE_INT) {
    return SQL_EXEC_SCHEMA_INVALID_PK;
  }

  if (!table_init_user(&user_table, db->pager, root_page, &schema)) {
    return SQL_EXEC_ROW_LAYOUT_TOO_LARGE;
  }

  max_key = find_table_max_key(&user_table, &has_rows);

  /* Validate all rows and key conflicts first to keep statement behavior atomic. */
  for (uint32_t row = 0; row < stmt->row_count; row++) {
    uint32_t key;
    if (stmt->row_value_count[row] != schema.column_count) {
      printf("Error: Expected %u values.\n", schema.column_count);
      return SQL_EXEC_WRONG_VALUE_COUNT;
    }

    memcpy(row_values, stmt->values[row], sizeof(row_values));
    if (stmt->row_auto_id[row]) {
      key = has_rows ? max_key + 1 : 1;
      snprintf(row_values[0], sizeof(row_values[0]), "%u", key);
      max_key = key;
      has_rows = true;
    } else {
      key = stmt->row_ids[row];
      if (!has_rows || key > max_key) {
        max_key = key;
        has_rows = true;
      }
    }

    if (!parse_u32_tokens_for_insert_values(row_values, &schema, key)) {
      return SQL_EXEC_INSERT_VALIDATION_FAILED;
    }
    if (!pack_insert_row_values(row_values, &schema, &user_table.row_layout,
                                row_buf)) {
      return SQL_EXEC_ROW_PACK_FAILED;
    }

    planned_keys[row] = key;
    for (uint32_t j = 0; j < row; j++) {
      if (planned_keys[j] == key) {
        return SQL_EXEC_DUPLICATE_KEY;
      }
    }
    if (table_has_key(&user_table, key)) {
      return SQL_EXEC_DUPLICATE_KEY;
    }
  }

  max_key = find_table_max_key(&user_table, &has_rows);
  for (uint32_t row = 0; row < stmt->row_count; row++) {
    uint32_t key;

    memcpy(row_values, stmt->values[row], sizeof(row_values));
    if (stmt->row_auto_id[row]) {
      key = has_rows ? max_key + 1 : 1;
      snprintf(row_values[0], sizeof(row_values[0]), "%u", key);
      max_key = key;
      has_rows = true;
    } else {
      key = stmt->row_ids[row];
      if (!has_rows || key > max_key) {
        max_key = key;
        has_rows = true;
      }
    }

    if (!pack_insert_row_values(row_values, &schema, &user_table.row_layout,
                                row_buf)) {
      return SQL_EXEC_ROW_PACK_FAILED;
    }

    btree_result = insert_row_into_table(&user_table, key, row_buf);
    if (btree_result == EXECUTE_DUPLICATE_KEY) {
      return SQL_EXEC_DUPLICATE_KEY;
    }
  }
  return SQL_EXEC_OK;
}

SqlExecuteResult execute_select(Table* db, const SqlStatement* stmt) {
  SqlSchema schema;
  uint32_t root_page;
  Table target;
  Cursor* cursor;
  size_t widths[MAX_SCHEMA_COLUMNS];
  uint32_t row_count = 0;

  if (!master_find_table(db, stmt->table_name, &schema, &root_page)) {
    return SQL_EXEC_TABLE_NOT_FOUND;
  }

  if (!table_init_user(&target, db->pager, root_page, &schema)) {
    return SQL_EXEC_SELECT_LAYOUT_INVALID;
  }

  if (g_run_options.quiet) {
    cursor = table_start(&target);
    while (!cursor->end_of_table) {
      row_count++;
      cursor_advance(cursor);
    }
    free(cursor);
    (void)row_count;
    return SQL_EXEC_OK;
  }

  for (uint32_t i = 0; i < schema.column_count; i++) {
    widths[i] = strlen(schema.column_names[i]);
  }

  cursor = table_start(&target);
  while (!cursor->end_of_table) {
    const uint8_t* d = cursor_value(cursor);
    for (uint32_t i = 0; i < schema.column_count; i++) {
      if (schema.column_type[i] == COLUMN_TYPE_INT) {
        uint32_t v;
        char num_buf[32];
        size_t n;
        memcpy(&v, d + target.row_layout.col_offset[i], sizeof(uint32_t));
        snprintf(num_buf, sizeof(num_buf), "%u", v);
        n = strlen(num_buf);
        if (n > widths[i]) {
          widths[i] = n;
        }
      } else {
        size_t n = bounded_text_len(
            (const char*)(d + target.row_layout.col_offset[i]),
            target.row_layout.col_size[i]);
        if (n > widths[i]) {
          widths[i] = n;
        }
      }
    }
    cursor_advance(cursor);
  }
  free(cursor);

  print_table_border(widths, schema.column_count);
  printf("|");
  for (uint32_t i = 0; i < schema.column_count; i++) {
    print_table_cell_text(schema.column_names[i], widths[i], false);
    printf("|");
  }
  printf("\n");
  print_table_border(widths, schema.column_count);

  cursor = table_start(&target);
  while (!cursor->end_of_table) {
    const uint8_t* d = cursor_value(cursor);
    row_count++;
    printf("|");
    for (uint32_t i = 0; i < schema.column_count; i++) {
      if (schema.column_type[i] == COLUMN_TYPE_INT) {
        uint32_t v;
        char num_buf[32];
        memcpy(&v, d + target.row_layout.col_offset[i], sizeof(uint32_t));
        snprintf(num_buf, sizeof(num_buf), "%u", v);
        print_table_cell_text(num_buf, widths[i], true);
      } else {
        size_t n = bounded_text_len(
            (const char*)(d + target.row_layout.col_offset[i]),
            target.row_layout.col_size[i]);
        char text_buf[256];
        if (n >= sizeof(text_buf)) {
          n = sizeof(text_buf) - 1;
        }
        memcpy(text_buf, d + target.row_layout.col_offset[i], n);
        text_buf[n] = '\0';
        print_table_cell_text(text_buf, widths[i], false);
      }
      printf("|");
    }
    printf("\n");
    cursor_advance(cursor);
  }
  free(cursor);
  print_table_border(widths, schema.column_count);
  printf("(%u rows)\n", row_count);
  return SQL_EXEC_OK;
}

SqlExecuteResult execute_delete(Table* db, const SqlStatement* stmt) {
  SqlSchema schema;
  uint32_t root_page;
  Table target;

  if (!master_find_table(db, stmt->table_name, &schema, &root_page)) {
    return SQL_EXEC_TABLE_NOT_FOUND;
  }

  if (!table_init_user(&target, db->pager, root_page, &schema)) {
    return SQL_EXEC_SELECT_LAYOUT_INVALID;
  }

  Cursor* cursor = table_find(&target, stmt->id);
  void* page = get_page(target.pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(page);

  if (cursor->cell_num < num_cells) {
    uint32_t key_at_index = *tbl_leaf_key(&target, page, cursor->cell_num);
    if (key_at_index == stmt->id) {
       leaf_node_delete(cursor);
       free(cursor);
       return SQL_EXEC_OK;
    }
  }

  free(cursor);
  return SQL_EXEC_DELETE_RECORD_NOT_FOUND;
}

SqlExecuteResult execute_update(Table* db, const SqlStatement* stmt) {
  SqlSchema schema;
  uint32_t root_page;
  Table target;
  Cursor* cursor;
  uint32_t num_cells;
  uint32_t target_col = UINT32_MAX;
  void* page;
  uint8_t row_buf[LEAF_NODE_SPACE_FOR_CELLS];

  if (!master_find_table(db, stmt->table_name, &schema, &root_page)) {
    return SQL_EXEC_TABLE_NOT_FOUND;
  }
  if (!table_init_user(&target, db->pager, root_page, &schema)) {
    return SQL_EXEC_SELECT_LAYOUT_INVALID;
  }

  for (uint32_t i = 0; i < schema.column_count; i++) {
    if (strcasecmp(schema.column_names[i], stmt->update_column) == 0) {
      target_col = i;
      break;
    }
  }
  if (target_col == UINT32_MAX) {
    return SQL_EXEC_UPDATE_COLUMN_NOT_FOUND;
  }
  if (target_col == 0) {
    return SQL_EXEC_UPDATE_PK_NOT_ALLOWED;
  }

  cursor = table_find(&target, stmt->id);
  page = get_page(target.pager, cursor->page_num);
  num_cells = *leaf_node_num_cells(page);
  if (cursor->cell_num >= num_cells ||
      *tbl_leaf_key(&target, page, cursor->cell_num) != stmt->id) {
    free(cursor);
    return SQL_EXEC_UPDATE_RECORD_NOT_FOUND;
  }

  memcpy(row_buf, tbl_leaf_value(&target, page, cursor->cell_num),
         target.row_layout.value_size);

  if (schema.column_type[target_col] == COLUMN_TYPE_INT) {
    uint32_t v;
    if (!parse_u32(stmt->update_value, &v)) {
      printf("Type error: expected int for column %s.\n",
             schema.column_names[target_col]);
      free(cursor);
      return SQL_EXEC_INSERT_VALIDATION_FAILED;
    }
    memcpy(row_buf + target.row_layout.col_offset[target_col], &v,
           sizeof(uint32_t));
  } else {
    size_t len = strlen(stmt->update_value);
    if (len > schema.column_max[target_col]) {
      printf("String is too long.\n");
      free(cursor);
      return SQL_EXEC_INSERT_VALIDATION_FAILED;
    }
    memset(row_buf + target.row_layout.col_offset[target_col], 0,
           target.row_layout.col_size[target_col]);
    memcpy(row_buf + target.row_layout.col_offset[target_col],
           stmt->update_value, len);
  }

  memcpy(tbl_leaf_value(&target, page, cursor->cell_num), row_buf,
         target.row_layout.value_size);
  mark_page_dirty(target.pager, cursor->page_num);
  free(cursor);
  return SQL_EXEC_OK;
}

SqlExecuteResult execute_drop_table(Table* db, const SqlStatement* stmt) {
  Table master;
  Cursor* cursor;
  char name_buf[MAX_TABLE_NAME_LENGTH + 1];
  char schema_buf[SCHEMA_TEXT_MAX + 1];
  uint32_t dummy_root;

  if (strcasecmp(stmt->table_name, "sqlyt_master") == 0) {
    return SQL_EXEC_RESERVED_DROP_TABLE;
  }

  table_init_catalog(&master, db->pager);
  master.root_page_num = db->pager->master_root_page;
  cursor = table_start(&master);
  while (!cursor->end_of_table) {
    void* node = get_page(db->pager, cursor->page_num);
    catalog_read_value(tbl_leaf_value(&master, node, cursor->cell_num),
                       &dummy_root, name_buf, schema_buf, sizeof(schema_buf));
    if (strcasecmp(name_buf, stmt->table_name) == 0) {
      leaf_node_delete(cursor);
      free(cursor);
      return SQL_EXEC_OK;
    }
    cursor_advance(cursor);
  }

  free(cursor);
  return SQL_EXEC_TABLE_NOT_FOUND;
}

SqlExecuteResult execute_statement(Session* session, const SqlStatement* stmt) {
  Table* db = session->database;

  switch (stmt->type) {
    case SQL_STMT_CREATE_DATABASE:
      return execute_create_database(session, stmt);
    case SQL_STMT_CREATE_TABLE:
      return execute_create_table(db, stmt);
    case SQL_STMT_INSERT:
      return execute_insert(db, stmt);
    case SQL_STMT_SELECT:
      return execute_select(db, stmt);
    case SQL_STMT_DELETE:
      return execute_delete(db, stmt);
    case SQL_STMT_UPDATE:
      return execute_update(db, stmt);
    case SQL_STMT_DROP_TABLE:
      return execute_drop_table(db, stmt);
  }
  return SQL_EXEC_OK;
}

static void print_sql_execute_result(SqlExecuteResult r) {
  if (g_run_options.quiet) {
    /* In quiet mode, only print user-visible errors that are emitted elsewhere. */
    if (r == SQL_EXEC_WRONG_VALUE_COUNT ||
        r == SQL_EXEC_INSERT_VALIDATION_FAILED) {
      return;
    }
    if (r == SQL_EXEC_OK) {
      return;
    }
  }
  switch (r) {
    case SQL_EXEC_OK:
      printf("Executed.\n");
      break;
    case SQL_EXEC_CREATE_DB_FAILED:
      printf("Error: Could not create database.\n");
      break;
    case SQL_EXEC_RESERVED_TABLE_NAME:
      printf("Error: Reserved table name.\n");
      break;
    case SQL_EXEC_TABLE_EXISTS:
      printf("Error: Table already exists.\n");
      break;
    case SQL_EXEC_ROW_LAYOUT_TOO_LARGE:
      printf("Error: Row layout too large for one page.\n");
      break;
    case SQL_EXEC_TABLE_NAME_TOO_LONG:
      printf("Error: Table name too long for catalog.\n");
      break;
    case SQL_EXEC_SCHEMA_METADATA_TOO_LONG:
      printf("Error: Schema metadata too long.\n");
      break;
    case SQL_EXEC_DUPLICATE_KEY:
      printf("Error: Duplicate key.\n");
      break;
    case SQL_EXEC_TABLE_NOT_FOUND:
      printf("Error: Table not found.\n");
      break;
    case SQL_EXEC_WRONG_VALUE_COUNT:
      break;
    case SQL_EXEC_SCHEMA_INVALID_PK:
      printf("Error: First column must be int primary key.\n");
      break;
    case SQL_EXEC_INSERT_VALIDATION_FAILED:
      break;
    case SQL_EXEC_ROW_PACK_FAILED:
      printf("Error: Row pack failed.\n");
      break;
    case SQL_EXEC_SELECT_LAYOUT_INVALID:
      printf("Error: Invalid table layout.\n");
      break;
    case SQL_EXEC_DELETE_RECORD_NOT_FOUND:
      printf("Error: Record not found to delete.\n");
      break;
    case SQL_EXEC_UPDATE_COLUMN_NOT_FOUND:
      printf("Error: Column not found for update.\n");
      break;
    case SQL_EXEC_UPDATE_PK_NOT_ALLOWED:
      printf("Error: Updating primary key is not supported.\n");
      break;
    case SQL_EXEC_UPDATE_RECORD_NOT_FOUND:
      printf("Error: Record not found to update.\n");
      break;
    case SQL_EXEC_RESERVED_DROP_TABLE:
      printf("Error: Cannot drop reserved table.\n");
      break;
  }
}

void execute_sql(Session* session, const SqlStatement* stmt) {
  struct timespec t0, t1;
  if (g_run_options.timer_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
  }

  SqlExecuteResult r = execute_statement(session, stmt);
  print_sql_execute_result(r);
  
  if (r == SQL_EXEC_OK &&
      (stmt->type == SQL_STMT_INSERT || stmt->type == SQL_STMT_CREATE_TABLE ||
       stmt->type == SQL_STMT_CREATE_DATABASE || stmt->type == SQL_STMT_DELETE ||
       stmt->type == SQL_STMT_UPDATE || stmt->type == SQL_STMT_DROP_TABLE)) {
    if (!g_run_options.in_transaction) {
      if (session->database && session->database->pager) {
        pager_commit_transaction_sync(session->database->pager);
      }
    }
  }

  if (g_run_options.timer_enabled) {
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    if (!g_run_options.quiet) {
      printf("Time: %.3f ms\n", ms);
    }
  }
}

MetaCommandResult do_meta_command(Session* session, InputBuffer* input_buffer) {
  if (strcmp(input_buffer->buffer, ".exit") == 0) {
    return META_COMMAND_EXIT;
  }

  if (strncmp(input_buffer->buffer, ".quiet", 6) == 0 &&
      (input_buffer->buffer[6] == ' ' || input_buffer->buffer[6] == '\t' ||
       input_buffer->buffer[6] == '\0')) {
    const char* arg = input_buffer->buffer + 6;
    while (*arg == ' ' || *arg == '\t') arg++;
    if (strcasecmp(arg, "on") == 0) {
      g_run_options.quiet = true;
    } else if (strcasecmp(arg, "off") == 0 || *arg == '\0') {
      g_run_options.quiet = false;
    } else if (!g_run_options.quiet) {
      printf("Usage: .quiet on|off\n");
    }
    return META_COMMAND_SUCCESS;
  }

  if (strncmp(input_buffer->buffer, ".timer", 6) == 0 &&
      (input_buffer->buffer[6] == ' ' || input_buffer->buffer[6] == '\t' ||
       input_buffer->buffer[6] == '\0')) {
    const char* arg = input_buffer->buffer + 6;
    while (*arg == ' ' || *arg == '\t') arg++;
    if (strcasecmp(arg, "on") == 0) {
      g_run_options.timer_enabled = true;
    } else if (strcasecmp(arg, "off") == 0 || *arg == '\0') {
      g_run_options.timer_enabled = false;
    } else if (!g_run_options.quiet) {
      printf("Usage: .timer on|off\n");
    }
    return META_COMMAND_SUCCESS;
  }

  if (strcmp(input_buffer->buffer, ".begin") == 0) {
    g_run_options.in_transaction = true;
    if (!g_run_options.quiet) {
      printf("Transaction started.\n");
    }
    return META_COMMAND_SUCCESS;
  }

  if (strcmp(input_buffer->buffer, ".commit") == 0) {
    if (session->database && session->database->pager) {
      pager_commit_transaction_sync(session->database->pager);
    }
    g_run_options.in_transaction = false;
    if (!g_run_options.quiet) {
      printf("Transaction committed.\n");
    }
    return META_COMMAND_SUCCESS;
  }

  if (strncmp(input_buffer->buffer, ".usedatabase", 12) == 0 &&
      input_buffer->input_length >= 12 &&
      (input_buffer->buffer[12] == ' ' || input_buffer->buffer[12] == '\t' ||
       input_buffer->buffer[12] == '\0')) {
    const char* db_name = input_buffer->buffer + 12;
    while (*db_name == ' ' || *db_name == '\t') {
      db_name++;
    }
    if (*db_name == '\0') {
      if (!g_run_options.quiet) printf("Unable to use database.\n");
      return META_COMMAND_SUCCESS;
    }
    if (!switch_database(session, db_name)) {
      if (!g_run_options.quiet) printf("Unable to use database.\n");
    } else {
      if (!g_run_options.quiet) printf("Using database %s\n", db_name);
    }
    return META_COMMAND_SUCCESS;
  }

  if (strcmp(input_buffer->buffer, ".showdatabases") == 0) {
    if (g_run_options.quiet) return META_COMMAND_SUCCESS;
    list_databases(session->root_path);
    return META_COMMAND_SUCCESS;
  }

  if (strcmp(input_buffer->buffer, ".showtables") == 0) {
    if (!session->has_active_database || session->database == NULL) {
      if (!g_run_options.quiet) {
        printf("No active database. Use .usedatabase <name>.\n");
      }
      return META_COMMAND_SUCCESS;
    }
    if (g_run_options.quiet) return META_COMMAND_SUCCESS;
    show_tables(session->database);
    return META_COMMAND_SUCCESS;
  }

  if (strncmp(input_buffer->buffer, ".btree ", 7) == 0) {
    const char* table_name = input_buffer->buffer + 7;
    SqlSchema schema;
    uint32_t root_page;
    if (!session->has_active_database || session->database == NULL) {
      if (!g_run_options.quiet) {
        printf("No active database. Use .usedatabase <name>.\n");
      }
      return META_COMMAND_SUCCESS;
    }
    if (!master_find_table(session->database, table_name, &schema, &root_page)) {
      if (!g_run_options.quiet) printf("Error: Table not found.\n");
      return META_COMMAND_SUCCESS;
    }
    Table layout_table;
    if (!table_init_user(&layout_table, session->database->pager, root_page,
                         &schema)) {
      if (!g_run_options.quiet) printf("Error: Invalid table layout.\n");
      return META_COMMAND_SUCCESS;
    }
    if (!g_run_options.quiet) {
      printf("Tree:\n");
      print_tree(session->database->pager, root_page, 0, layout_table.cell_size);
    }
    return META_COMMAND_SUCCESS;
  }

  if (strcmp(input_buffer->buffer, ".constants") == 0) {
    if (!g_run_options.quiet) {
      printf("Constants:\n");
      print_constants();
    }
    return META_COMMAND_SUCCESS;
  }

  return META_COMMAND_UNRECOGNIZED_COMMAND;
}

