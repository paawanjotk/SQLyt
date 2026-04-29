#include "sqlyt.h"

static void normalize_input_line(InputBuffer* input_buffer) {
  char* s;
  size_t len;
  size_t start = 0;
  size_t end;

  if (input_buffer == NULL || input_buffer->buffer == NULL) {
    return;
  }

  s = input_buffer->buffer;
  len = strlen(s);

  while (start < len && (s[start] == ' ' || s[start] == '\t' ||
                         s[start] == '\n' || s[start] == '\r')) {
    start++;
  }

  end = len;
  while (end > start &&
         (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' ||
          s[end - 1] == '\r')) {
    end--;
  }

  if (start > 0 && end >= start) {
    memmove(s, s + start, end - start);
  }
  s[end - start] = '\0';
  input_buffer->input_length = (ssize_t)(end - start);
  input_buffer->buffer_length = (size_t)input_buffer->input_length + 1;
}

static int process_line(Session* session, SqlStatement* stmt, InputBuffer* input_buffer) {
  if (input_buffer->buffer == NULL) {
    return 0;
  }

  normalize_input_line(input_buffer);
  if (input_buffer->buffer[0] == '\0') {
    return 0;
  }

  if (input_buffer->buffer[0] == '.') {
    MetaCommandResult meta = do_meta_command(session, input_buffer);
    if (meta == META_COMMAND_EXIT) {
      return 1;
    }
    if (meta == META_COMMAND_UNRECOGNIZED_COMMAND) {
      if (!g_run_options.quiet) {
        printf("Unrecognized command '%s'\n", input_buffer->buffer);
      }
    }
    return 0;
  }

  memset(stmt, 0, sizeof(*stmt));
  if (!parse_sql_statement(input_buffer->buffer, stmt)) {
    if (!g_run_options.quiet) {
      printf("Syntax error. Could not parse SQL statement.\n");
    }
    return -1;
  }

  if (stmt->type != SQL_STMT_CREATE_DATABASE &&
      (!session->has_active_database || session->database == NULL)) {
    if (!g_run_options.quiet) {
      printf("No active database. Use .usedatabase <name>.\n");
    }
    return -1;
  }

  execute_sql(session, stmt);
  return 0;
}

int main(int argc, char* argv[]) {
  Session session;
  InputBuffer* input_buffer;
  SqlStatement* stmt;
  const char* root_path;
  const char* run_path = NULL;

  memset(&session, 0, sizeof(session));

  root_path = DEFAULT_ROOT_PATH;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--quiet") == 0) {
      g_run_options.quiet = true;
      continue;
    }
    if (strcmp(argv[i], "--timer") == 0) {
      g_run_options.timer_enabled = true;
      continue;
    }
    if (strcmp(argv[i], "--run") == 0 && i + 1 < argc) {
      run_path = argv[i + 1];
      i++;
      continue;
    }
    if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
      root_path = argv[i + 1];
      i++;
      continue;
    }
    if (argv[i][0] != '-' && root_path == DEFAULT_ROOT_PATH) {
      /* Back-compat: ./db /path/to/root */
      root_path = argv[i];
      continue;
    }
    if (!g_run_options.quiet) {
      printf("Unknown argument: %s\n", argv[i]);
      printf("Usage: ./db [root_path] [--root PATH] [--run file.sql] [--quiet] [--timer]\n");
    }
    return EXIT_FAILURE;
  }

  strncpy(session.root_path, root_path, sizeof(session.root_path) - 1);
  session.root_path[sizeof(session.root_path) - 1] = '\0';
  if (!ensure_directory(session.root_path)) {
    printf("Unable to create/open root directory.\n");
    exit(EXIT_FAILURE);
  }

  input_buffer = new_input_buffer();
  stmt = calloc(1, sizeof(SqlStatement));
  if (stmt == NULL) {
    printf("Unable to allocate statement buffer.\n");
    close_input_buffer(input_buffer);
    exit(EXIT_FAILURE);
  }

  if (run_path != NULL) {
    FILE* f = fopen(run_path, "r");
    if (f == NULL) {
      if (!g_run_options.quiet) {
        printf("Unable to open script file.\n");
      }
      free(stmt);
      close_input_buffer(input_buffer);
      return EXIT_FAILURE;
    }

    char* line = NULL;
    size_t cap = 0;
    while (true) {
      ssize_t n = getline(&line, &cap, f);
      if (n <= 0) {
        break;
      }
      /* Strip trailing newline */
      if (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[n - 1] = '\0';
      }

      free(input_buffer->buffer);
      input_buffer->buffer = strdup(line);
      if (input_buffer->buffer == NULL) {
        break;
      }
      input_buffer->input_length = (ssize_t)strlen(input_buffer->buffer);
      input_buffer->buffer_length = (size_t)input_buffer->input_length + 1;

      int rc = process_line(&session, stmt, input_buffer);
      if (rc == 1) {
        break; /* .exit */
      }
      if (rc < 0) {
        free(line);
        fclose(f);
        free(stmt);
        close_input_buffer(input_buffer);
        if (session.database != NULL) {
          db_close(session.database);
        }
        return EXIT_FAILURE;
      }
    }

    free(line);
    fclose(f);
    free(stmt);
    close_input_buffer(input_buffer);
    if (session.database != NULL) {
      db_close(session.database);
    }
    return EXIT_SUCCESS;
  }

  while (true) {
    read_input(input_buffer);
    int rc = process_line(&session, stmt, input_buffer);
    if (rc == 1) { /* .exit */
      free(stmt);
      close_input_buffer(input_buffer);
      if (session.database != NULL) {
        db_close(session.database);
      }
      exit(EXIT_SUCCESS);
    }
  }
}