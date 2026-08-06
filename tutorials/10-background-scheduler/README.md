# Tutorial 10: Asynchronous Task Scheduler

Schedule asynchronous one-shot or recurring tasks using the background worker thread pool.

## Key Concepts
- Initialize worker pool with `cwist_scheduler_init()`.
- Queue deferred execution callbacks via `cwist_scheduler_schedule_once(fn, arg, delay_sec)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut10
```
