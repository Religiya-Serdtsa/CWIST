/**
 * @file cwist_io.h
 * @brief Platform Agnostic Async Job Queue Interface.
 */

#ifndef __CWIST_IO_H__
#define __CWIST_IO_H__

#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Opaque handle for the IO Queue.
 * Backend implementation depends on OS (io_uring, kqueue, select).
 */
typedef struct cwist_io_queue cwist_io_queue;

/**
 * @brief Job function signature.
 * @param arg User-provided argument.
 */
typedef void (*cwist_job_func)(void *arg);

/**
 * @brief Initialize the job queue.
 * @param capacity Maximum concurrent jobs/events.
 * @return Pointer to queue handle, or NULL on failure.
 */
cwist_io_queue *cwist_io_queue_create(size_t capacity);

/**
 * @brief Submit a job to be executed asynchronously.
 * @param q Queue handle.
 * @param func Function to execute.
 * @param arg Argument to pass to function.
 * @return true on success, false on failure (queue full/error).
 */
bool cwist_io_queue_submit(cwist_io_queue *q, cwist_job_func func, void *arg);

/**
 * @brief Run the event loop.
 * Blocks indefinitely, processing jobs as they complete/trigger.
 * @param q Queue handle.
 */
void cwist_io_queue_run(cwist_io_queue *q);

/**
 * @brief Destroy the queue and free resources.
 * @param q Queue handle.
 */
void cwist_io_queue_destroy(cwist_io_queue *q);

#endif
