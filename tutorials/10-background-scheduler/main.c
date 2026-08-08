#include <cwist/app.h>
#include <cwist/sys/job/scheduler.h>
#include <stdio.h>

static void my_background_job(void *arg) {
    (void)arg;
    printf("[JOB] Background scheduler task executed.\n");
}

int main(void) {
    cwist_scheduler_t *sched = cwist_scheduler_create(2, 64);
    if (sched) {
        cwist_scheduler_schedule(sched, my_background_job, NULL, 1000);
    }

    cwist_app *app = cwist_app_create();
    cwist_app_listen(app, 8089);

    cwist_app_destroy(app);
    if (sched) {
        cwist_scheduler_destroy(sched);
    }
    return 0;
}
