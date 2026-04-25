#ifndef TEST_DB_H
#define TEST_DB_H

#include <stdint.h>

/**
 * @brief Opaque handle for the database (Encapsulation).
 */
typedef struct test_db test_db_t;

/**
 * @brief Initializes the database and creates tables.
 * @param path Path to the database file.
 * @return Pointer to handle on success, NULL on failure.
 */
test_db_t* test_db_init(const char *path);

/**
 * @brief Saves a test result to the database.
 * @return 0 on success, -1 on failure.
 */
int test_db_save(test_db_t *db, uint32_t id, double duration, 
                 const char *peripheral, const char *status);

/**
 * @brief Prints all records (Print on demand).
 */
void test_db_print_report(test_db_t *db);

/**
 * @brief Clears all records from the results table.
 * @return 0 on success, -1 on failure.
 */
int test_db_clear(test_db_t *db);

/**
 * @brief Closes the database and frees memory.
 */
void test_db_destroy(test_db_t *db);

#endif /* TEST_DB_H */