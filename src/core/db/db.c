#include <cwist/core/db/sql.h>
#include <cwist/core/utils/json_heal.h>
#include <cwist/core/utils/zod.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/mem/alloc.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/**
 * @file db.c
 * @brief SQLite convenience wrappers plus lightweight heuristics for query execution and JSON healing.
 */

typedef struct {
    uint32_t join_count;
    uint32_t predicate_count;
    uint64_t symbolic_weight;
} cwist_db_plan_hint;

/* Bhaskara II style integer-root refinement keeps everything in integer space. */
/**
 * @brief Compute an integer square-root style refinement for heuristic scaling.
 * @param value Input value to reduce.
 * @return Integer root approximation used for timeout tuning.
 */
static uint64_t cwist_db_integer_root(uint64_t value) {
    if (value == 0) return 0;
    uint64_t x = value;
    uint64_t y = (x + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + value / x) >> 1;
    }
    return x;
}

/**
 * @brief Check whether a character is an ASCII alphabetic letter.
 * @param c Character to classify.
 * @return true when @p c is an ASCII letter.
 */
static bool cwist_db_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/**
 * @brief Match an uppercase SQL keyword at a specific position with identifier boundaries.
 * @param sql SQL string to inspect.
 * @param pos Candidate keyword start position.
 * @param kw Uppercase keyword to match.
 * @return true when the keyword occurs at @p pos with token boundaries.
 */
static bool cwist_db_match_keyword(const char *sql, size_t pos, const char *kw) {
    size_t i = 0;
    while (kw[i]) {
        char current = sql[pos + i];
        if (!current) return false;
        if ((char)toupper((unsigned char)current) != kw[i]) {
            return false;
        }
        ++i;
    }
    if (pos > 0) {
        char prev = sql[pos - 1];
        if (cwist_db_is_alpha(prev)) return false;
    }
    char next = sql[pos + i];
    if (cwist_db_is_alpha(next)) return false;
    return true;
}

/**
 * @brief Derive simple execution heuristics from raw SQL text.
 * @param sql SQL text to analyze.
 * @return Hint bundle containing join count, predicate count, and symbolic weight.
 */
static cwist_db_plan_hint cwist_db_analyze_sql(const char *sql) {
    cwist_db_plan_hint hint = {0, 0, 0};
    if (!sql) return hint;
    size_t len = strlen(sql);
    uint64_t accum = 0xcbf29ce484222325ULL;
    uint32_t depth = 0;
    for (size_t i = 0; i < len; ++i) {
        char upper = (char)toupper((unsigned char)sql[i]);
        accum ^= (uint64_t)upper;
        accum *= 0x100000001b3ULL;
        if (upper == '(') depth++;
        else if (upper == ')' && depth > 0) depth--;

        if (upper == 'J' && cwist_db_match_keyword(sql, i, "JOIN")) {
            hint.join_count++;
        } else if (upper == 'W' && cwist_db_match_keyword(sql, i, "WHERE")) {
            hint.predicate_count++;
        } else if (upper == 'A' && cwist_db_match_keyword(sql, i, "AND")) {
            hint.predicate_count++;
        } else if (upper == 'O' && cwist_db_match_keyword(sql, i, "OR")) {
            hint.predicate_count++;
        }
    }
    hint.symbolic_weight = accum ^ ((uint64_t)depth << 48);
    return hint;
}

/**
 * @brief Apply coarse SQLite pragmas and busy timeouts derived from the SQL hint.
 * @param conn SQLite connection to tune.
 * @param hint Analyzed SQL complexity hint.
 */
static void cwist_db_apply_hint(sqlite3 *conn, const cwist_db_plan_hint *hint) {
    if (!conn || !hint) return;
    uint64_t complexity = hint->symbolic_weight +
                          ((uint64_t)hint->join_count * 131ULL) +
                          ((uint64_t)hint->predicate_count * 53ULL) + 1ULL;
    uint64_t root = cwist_db_integer_root(complexity);
    int busy_ms = 50 + (int)(root % 350);
    sqlite3_busy_timeout(conn, busy_ms);
    if (hint->join_count >= 2) {
        sqlite3_exec(conn, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
    }
    if (hint->predicate_count >= 3) {
        sqlite3_exec(conn, "PRAGMA cache_spill = 0;", NULL, NULL, NULL);
    }
}

// Make SQLite Error type as cwist_error_t
/**
 * @brief Convert an SQLite result code and message into CWIST's JSON error form.
 * @param rc SQLite result code.
 * @param msg SQLite-provided human-readable message.
 * @return JSON-backed CWIST error object.
 */
static cwist_error_t make_sqlite_error(int rc, char *msg) {
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(err.error.err_json, "sqlite_rc", rc);
    cJSON_AddStringToObject(err.error.err_json, "message", msg ? msg : "Unknown Error");
    return err;
}

// Open SQLite database file
// 0 on success, -1 on failure
/**
 * @brief Open a SQLite database and wrap the resulting connection in a CWIST handle.
 * @param db Output pointer receiving the allocated database wrapper.
 * @param path Filesystem path or SQLite URI to open.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_db_open(cwist_db **db, const char *path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    // if there's no db or path, return -1
    if (!db || !path) {
        err.error.err_i16 = -1;
        return err;
    }

    // if malloc fails, return -1
    *db = (cwist_db*)cwist_alloc(sizeof(cwist_db));
    if (!*db) {
        err.error.err_i16 = -1;
        return err;
    }

    int rc = sqlite3_open(path, &(*db)->conn);
    if (rc) {
        cwist_error_t sql_err = make_sqlite_error(rc, (char*)sqlite3_errmsg((*db)->conn));
        sqlite3_close((*db)->conn);
        cwist_free(*db);
        *db = NULL;
        return sql_err;
    }

    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Close a CWIST database wrapper and its underlying SQLite connection.
 * @param db Database wrapper to close.
 */
void cwist_db_close(cwist_db *db) {
    if (db) {
        if (db->conn) {
            sqlite3_close(db->conn);
        }
        cwist_free(db);
    }
}

// Execute given SQL command
// return errmsg on failure
/**
 * @brief Execute an arbitrary SQL statement string without returning rows.
 * @param db Open database wrapper.
 * @param sql SQL text to execute.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_db_exec(cwist_db *db, const char *sql) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!db || !db->conn || !sql) {
        err.error.err_i16 = -1;
        return err;
    }

    char *zErrMsg = 0;
    cwist_db_plan_hint hint = cwist_db_analyze_sql(sql);
    cwist_db_apply_hint(db->conn, &hint);
    int rc = sqlite3_exec(db->conn, sql, 0, 0, &zErrMsg);
    
    if (rc != SQLITE_OK) {
        cwist_error_t err = make_sqlite_error(rc, zErrMsg);
        sqlite3_free(zErrMsg);
        return err;
    }

    err.error.err_i16 = 0;
    return err;
}

// Callback for converting rows to JSON
typedef struct {
    cJSON *rows;
} query_context;

/**
 * @brief Convert one sqlite3_exec callback row into a cJSON object appended to the result array.
 * @param data Query context carrying the destination array.
 * @param argc Number of columns in the row.
 * @param argv Column values as strings or NULL.
 * @param azColName Column names for the current row.
 * @return 0 to continue sqlite3_exec iteration.
 */
static int query_callback(void *data, int argc, char **argv, char **azColName) {
    query_context *ctx = (query_context *)data;
    cJSON *row = cJSON_CreateObject();
    
    for (int i = 0; i < argc; i++) {
        // SQLite returns everything as char* by default in exec callback
        // For simplicity, we treat everything as string or try to guess types if needed.
        // But for a generic wrapper, string is safest unless we parse schema.
        if (argv[i]) {
            cJSON_AddStringToObject(row, azColName[i], argv[i]);
        } else {
            cJSON_AddNullToObject(row, azColName[i]);
        }
    }
    
    cJSON_AddItemToArray(ctx->rows, row);
    return 0;
}

// execute a query and store result at result pointer
/**
 * @brief Execute a SQL query and collect every row into a cJSON array.
 * @param db Open database wrapper.
 * @param sql SQL text to execute.
 * @param result Output pointer receiving the cJSON row array.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_db_query(cwist_db *db, const char *sql, cJSON **result) {
    if (!result) {
        cwist_error_t err = make_error(CWIST_ERR_INT16);
        err.error.err_i16 = -1;
        return err;
    }
    *result = NULL;
    if (!db || !db->conn || !sql) {
        cwist_error_t err = make_error(CWIST_ERR_INT16);
        err.error.err_i16 = -1;
        return err;
    }

    query_context ctx;
    ctx.rows = cJSON_CreateArray();
    
    char *zErrMsg = 0;
    cwist_db_plan_hint hint = cwist_db_analyze_sql(sql);
    cwist_db_apply_hint(db->conn, &hint);
    int rc = sqlite3_exec(db->conn, sql, query_callback, &ctx, &zErrMsg);
    
    if (rc != SQLITE_OK) {
        cJSON_Delete(ctx.rows);
        cwist_error_t err = make_sqlite_error(rc, zErrMsg);
        sqlite3_free(zErrMsg);
        return err;
    }

    *result = ctx.rows;
    
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = 0;
    return err;
}

/* ==========================================================================
 * cwist_db_insert_healed
 * ======================================================================== */

/*
 * Build an INSERT statement from a cJSON object.
 * Returns a heap-allocated (sqlite3_malloc) SQL string, or NULL on failure.
 * The caller must free it with sqlite3_free().
 *
 * String values are escaped with %Q; numbers, booleans and nulls are
 * rendered directly.  Nested objects/arrays are serialised to a JSON
 * string so they can be stored as TEXT.
 */
/**
 * @brief Build an INSERT statement from a cJSON object using SQLite-safe quoting.
 * @param table Destination table name.
 * @param obj JSON object whose fields become columns and values.
 * @return sqlite3_malloc-allocated SQL string, or NULL on failure.
 */
static char *build_insert_sql(const char *table, const cJSON *obj) {
    /* Collect column/value fragments. */
    int    count   = cJSON_GetArraySize((cJSON *)obj);
    if (count <= 0) return NULL;

    /* Worst-case sizes – we'll sqlite3_mprintf each fragment individually. */
    char *cols_buf = (char *)sqlite3_malloc(count * 64 + 16);
    char *vals_buf = (char *)sqlite3_malloc(count * 256 + 16);
    if (!cols_buf || !vals_buf) {
        sqlite3_free(cols_buf);
        sqlite3_free(vals_buf);
        return NULL;
    }
    cols_buf[0] = '\0';
    vals_buf[0] = '\0';

    const cJSON *child = obj->child;
    bool first = true;
    while (child) {
        /* Column name – double-quoted for safety */
        char col_frag[128];
        snprintf(col_frag, sizeof(col_frag),
                 first ? "\"%s\"" : ",\"%s\"", child->string);
        size_t cols_cap  = (size_t)(count * 64 + 16);
        size_t cols_used = strlen(cols_buf);
        if (cols_used < cols_cap - 1)
            strncat(cols_buf, col_frag, cols_cap - 1 - cols_used);

        /* Value */
        char *val_frag = NULL;
        if (cJSON_IsString(child)) {
            val_frag = sqlite3_mprintf(first ? "%Q" : ",%Q",
                                       child->valuestring);
        } else if (cJSON_IsNumber(child)) {
            val_frag = sqlite3_mprintf(first ? "%g" : ",%g",
                                       child->valuedouble);
        } else if (cJSON_IsTrue(child)) {
            val_frag = sqlite3_mprintf(first ? "1" : ",1");
        } else if (cJSON_IsFalse(child)) {
            val_frag = sqlite3_mprintf(first ? "0" : ",0");
        } else if (cJSON_IsNull(child)) {
            val_frag = sqlite3_mprintf(first ? "NULL" : ",NULL");
        } else {
            /* Object / Array → store as JSON text */
            char *serialised = cJSON_PrintUnformatted(child);
            if (serialised) {
                val_frag = sqlite3_mprintf(first ? "%Q" : ",%Q", serialised);
                free(serialised);
            }
        }

        if (!val_frag) {
            sqlite3_free(cols_buf);
            sqlite3_free(vals_buf);
            return NULL;
        }
        size_t vals_cap  = (size_t)(count * 256 + 16);
        size_t vals_used = strlen(vals_buf);
        if (vals_used < vals_cap - 1)
            strncat(vals_buf, val_frag, vals_cap - 1 - vals_used);
        sqlite3_free(val_frag);

        first = false;
        child = child->next;
    }

    char *sql = sqlite3_mprintf("INSERT INTO \"%s\" (%s) VALUES (%s);",
                                 table, cols_buf, vals_buf);
    sqlite3_free(cols_buf);
    sqlite3_free(vals_buf);
    return sql;
}

/**
 * @brief Heal, validate, and insert a JSON object into a table.
 * @param db Open database wrapper.
 * @param table Destination table name.
 * @param json_str Raw JSON input to heal and insert.
 * @param schema Optional schema used for alignment and validation.
 * @param heal_cfg Optional healing configuration overrides.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_db_insert_healed(cwist_db *db, const char *table,
                                      const char *json_str,
                                      const cwist_schema_t  *schema,
                                      const cwist_heal_config_t *heal_cfg) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;

    if (!db || !table || !json_str) return err;

    /* Build effective heal config (merge schema if not already set). */
    cwist_heal_config_t effective_cfg;
    if (heal_cfg) {
        effective_cfg = *heal_cfg;
    } else {
        memset(&effective_cfg, 0, sizeof(effective_cfg));
        effective_cfg.threshold = 0.8;
    }
    if (schema && !effective_cfg.schema) effective_cfg.schema = schema;

    /* Step 1: heal the JSON. */
    cwist_heal_result_t healed = cwist_json_heal(json_str, &effective_cfg);
    if (!healed.json) {
        fprintf(stderr, "[CWIST-DB] insert_healed: healing failed for table '%s'\n",
                table);
        cwist_heal_result_free(&healed);
        return err;
    }

    /* Step 2: strict Zod validation if a schema is provided. */
    if (schema) {
        cJSON *validated = NULL;
        cwist_zod_result_t zod = cwist_zod_parse(healed.json, schema, &validated);
        if (!zod.valid) {
            fprintf(stderr,
                    "[CWIST-DB] insert_healed: Zod validation failed for table '%s':\n",
                    table);
            cwist_zod_print_errors(&zod);
            cwist_heal_result_free(&healed);
            return err;
        }
        if (validated) cJSON_Delete(validated);
    }

    /* Step 3: parse the healed JSON and build INSERT SQL. */
    cJSON *obj = cJSON_Parse(healed.json);
    cwist_heal_result_free(&healed);
    if (!obj || !cJSON_IsObject(obj)) {
        if (obj) cJSON_Delete(obj);
        return err;
    }

    char *sql = build_insert_sql(table, obj);
    cJSON_Delete(obj);
    if (!sql) return err;

    /* Step 4: execute. */
    err = cwist_db_exec(db, sql);
    sqlite3_free(sql);
    return err;
}

/* ==========================================================================
 * cwist_db_query_strict
 * ======================================================================== */

/**
 * @brief Execute a query and require the result rows to satisfy a CWIST schema.
 * @param db Open database wrapper.
 * @param sql SQL text to execute.
 * @param result Output pointer receiving the cJSON row array on success.
 * @param schema Schema applied to every returned row.
 * @return Tagged CWIST error describing success or validation failure.
 */
cwist_error_t cwist_db_query_strict(cwist_db *db, const char *sql, cJSON **result,
                                     const cwist_schema_t *schema) {
    /* When no schema is given, behave identically to cwist_db_query(). */
    if (!schema) return cwist_db_query(db, sql, result);

    cJSON *all = NULL;
    cwist_error_t err = cwist_db_query(db, sql, &all);
    if (err.errtype == CWIST_ERR_JSON || !all) return err;

    cJSON *conforming = cJSON_CreateArray();
    if (!conforming) {
        cJSON_Delete(all);
        err.error.err_i16 = -1;
        return err;
    }

    cJSON *row = NULL;
    cJSON_ArrayForEach(row, all) {
        cwist_zod_result_t zod = cwist_zod_validate(row, schema);
        if (zod.valid) {
            cJSON *clone = cJSON_Duplicate(row, 1);
            if (clone) cJSON_AddItemToArray(conforming, clone);
        }
    }

    cJSON_Delete(all);
    *result = conforming;

    err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = 0;
    return err;
}
