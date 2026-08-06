#include <cwist/app.h>
#include <cwist/sys/job/scheduler.h>
#include <stdio.h>

static void my_background_job(void *arg) {
    (void)arg;
    printf("[JOB] Background scheduler task executed.\n");
}

int main(void) {
    cwist_scheduler_init();
    cwist_scheduler_schedule_once(my_background_job, NULL, 1);

    cwist_app *app = cwist_app_create();
    cwist_app_listen(app, 8089);

    cwist_app_destroy(app);
    cwist_scheduler_shutdown();
    return 0;
}
