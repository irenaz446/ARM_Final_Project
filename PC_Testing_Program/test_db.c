#include "test_db.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <stdio.h>

struct test_db {
    sqlite3 *handle;
};

test_db_t* test_db_init(const char *path) {
    test_db_t *db = calloc(1, sizeof(test_db_t));
    if (db == NULL) {
        return NULL;
    }

    if (sqlite3_open(path, &db->handle) != SQLITE_OK) {
        sqlite3_close(db->handle);
        free(db);
        return NULL;
    }

    const char *sql = "CREATE TABLE IF NOT EXISTS results ("
                      "id INT PRIMARY KEY, timestamp TEXT, "
                      "duration REAL, peripheral TEXT, status TEXT);";
    
    if (sqlite3_exec(db->handle, sql, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db->handle);
        free(db);
        return NULL;
    }
    return db;
}

int test_db_save(test_db_t *db, uint32_t id, double duration, 
                 const char *peripheral, const char *status) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR IGNORE INTO results VALUES (?, datetime('now'), ?, ?, ?);";
    
    if (db == NULL) {
        return -1;
    }

    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_double(stmt, 2, duration);
    sqlite3_bind_text(stmt, 3, peripheral, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, status, -1, SQLITE_STATIC);

    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);

    if (rc == 0 && sqlite3_changes(db->handle) == 0) {
        printf("Warning: test ID already existed — result not saved\n");
    }

    return rc;
}

void test_db_print_report(test_db_t *db) {
    sqlite3_stmt *stmt;

    if (db == NULL) {
        printf("Warning: Nothing to print — failed to get DB\n");
        return;
    }

    if (sqlite3_prepare_v2(db->handle, "SELECT * FROM results;", -1, &stmt, NULL) != SQLITE_OK) {
        return;
    }

    printf("\n%-10s | %-20s | %-8s | %-10s | %-8s\n", "ID", "Timestamp", "Dur(s)", "Periph", "Result");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%-10d | %-20s | %-8.3f | %-10s | %-8s\n",
               sqlite3_column_int(stmt, 0), sqlite3_column_text(stmt, 1),
               sqlite3_column_double(stmt, 2), sqlite3_column_text(stmt, 3),
               sqlite3_column_text(stmt, 4));
    }
    sqlite3_finalize(stmt);
}

int test_db_clear(test_db_t *db) {
    if (db == NULL || db->handle == NULL) {
        return -1;
    }

    const char *sql = "DELETE FROM results;";
    char *err_msg = NULL;

    if (sqlite3_exec(db->handle, sql, NULL, NULL, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}

void test_db_destroy(test_db_t *db) {
    if (db != NULL) {
        sqlite3_close(db->handle);
        free(db);
    }
}