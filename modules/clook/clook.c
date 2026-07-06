#include "elevator.h"

static struct elevator_type clook = {
    .ops = {
        .init_sched = clook_init_sched,
        .exit_sched = clook_exit_sched,
        .insert_requests = clook_insert_requests,
        .dispatch_request = clook_dispatch_request,
        .has_work = clook_has_work,
        .finish_request = clook_finish_request
    },
    .elevator_name = "clook",
    .elevator_owner = THIS_MODULE,
};


