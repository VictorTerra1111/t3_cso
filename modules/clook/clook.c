#include "elevator.h"

#include <linux/module.h>
#include <linux/init.h>

#include <linux/printk.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kobject.h>

#include <linux/spinlock.h>
#include <linux/hrtimer.h>

static int clook_init_sched(struct request_queue *q, struct elevator_type *e);
static void clook_exit_sched(struct elevator_queue *e);
static void clook_insert_requests(struct blk_mq_hw_ctx *hctx, struct list_head *list, blk_insert_t flags);
static struct request *clook_dispatch_request(struct blk_mq_hw_ctx *hctx);
static bool clook_has_work(struct blk_mq_hw_ctx *hctx);
static void clook_finish_request(struct request *rq);
static int __init clook_init(void);
static void __exit clook_exit(void);
static enum hrtimer_restart clook_timer_callback(struct hrtimer *timer);

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

struct clook_data {
    struct list_head queue;

    spinlock_t lock;

    struct hrtimer timer;

    sector_t head_position;

    unsigned int nr_requests;

    unsigned int max_requests;
    unsigned int timeout_ms;

    bool timeout_expired;
    bool queue_full;
    bool debug;

    unsigned long long fcfs_distance;
    unsigned long long clook_distance;

    sector_t last_fcfs_sector;
    sector_t last_clook_sector;

    bool first_fcfs;
    bool first_clook;

    unsigned long dispatched;
    unsigned long received;
    unsigned long circular_jumps;

    struct blk_mq_hw_ctx *hctx;
};

struct clook_request {
    sector_t sector;
    struct request *rq;
    struct list_head list;
};

static unsigned int queue_size = 50;
static unsigned int timeout_ms = 50;
static bool debug = false;

module_param(queue_size, uint, 0644);
MODULE_PARM_DESC(queue_size, "Numero de requisicoes acumuladas antes do despacho C-LOOK");

module_param(timeout_ms, uint, 0644);
MODULE_PARM_DESC(timeout_ms, "Tempo maximo de espera da fila, em milissegundos");

module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Habilita logs de depuracao no log do kernel");

static int clook_init_sched(struct request_queue *q, struct elevator_type *e)
{
    struct elevator_queue *eq;
    struct clook_data *cd;

    eq = elevator_alloc(q, e);
    if (!eq)
        return -ENOMEM;

    cd = kzalloc(sizeof(*cd), GFP_KERNEL);
    if (!cd) {
        kobject_put(&eq->kobj);
        return -ENOMEM;
    }

    INIT_LIST_HEAD(&cd->queue);
    spin_lock_init(&cd->lock);

    if (queue_size == 0) {
        pr_warn("clook: queue_size=0 invalido; usando 50\n");
        cd->max_requests = 50;
    } else {
        cd->max_requests = queue_size;
    }

    if (timeout_ms == 0) {
        pr_warn("clook: timeout_ms=0 invalido; usando 50 ms\n");
        cd->timeout_ms = 50;
    } else {
        cd->timeout_ms = timeout_ms;
    }

    cd->debug = debug;

    cd->head_position = 0;
    cd->nr_requests = 0;

    cd->timeout_expired = false;
    cd->queue_full = false;

    cd->first_fcfs = true;
    cd->first_clook = true;

    cd->fcfs_distance = 0;
    cd->clook_distance = 0;

    cd->received = 0;
    cd->dispatched = 0;
    cd->circular_jumps = 0;

    cd->hctx = NULL;

    hrtimer_init(&cd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    cd->timer.function = clook_timer_callback;

    eq->elevator_data = cd;

    blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);

    q->nr_requests = 128;
    q->elevator = eq;

    pr_info("clook: scheduler initialized queue_size=%u timeout_ms=%u debug=%d\n",
            cd->max_requests, cd->timeout_ms, cd->debug);

    return 0;
}

static void clook_exit_sched(struct elevator_queue *e)
{
    struct clook_data *cd;
    struct clook_request *entry, *tmp;
    unsigned long long economy;

    cd = e->elevator_data;

    if (!cd)
        return;

    hrtimer_cancel(&cd->timer);

    list_for_each_entry_safe(entry, tmp, &cd->queue, list) {
        list_del_init(&entry->list);
        kfree(entry);
    }

    if (cd->fcfs_distance > cd->clook_distance)
        economy = cd->fcfs_distance - cd->clook_distance;
    else
        economy = 0;

    pr_info("clook: removendo escalonador: total despachado=%lu, setores FCFS=%llu, setores C-LOOK=%llu, economia=%llu\n",
            cd->dispatched, cd->fcfs_distance, cd->clook_distance, economy);
    pr_info("clook: requisicoes recebidas=%lu, saltos circulares=%lu\n",
            cd->received, cd->circular_jumps);

    kfree(cd);
}

static void clook_insert_requests(struct blk_mq_hw_ctx *hctx, struct list_head *list, blk_insert_t flags)
{
    struct clook_data *cd;
    struct request *rq, *next;
    struct clook_request *entry;
    unsigned long irqflags;
    bool run_queue = false;
    bool restart_timer = false;

    cd = hctx->queue->elevator->elevator_data;
    (void)flags;

    spin_lock_irqsave(&cd->lock, irqflags);

    cd->hctx = hctx;

    list_for_each_entry_safe(rq, next, list, queuelist) {

        list_del_init(&rq->queuelist);

        entry = kmalloc(sizeof(*entry), GFP_ATOMIC);

        if (!entry) {
            list_add_tail(&rq->queuelist, list);
            continue;
        }

        entry->rq = rq;
        entry->sector = blk_rq_pos(rq);

        INIT_LIST_HEAD(&entry->list);
        list_add_tail(&entry->list, &cd->queue);

        cd->nr_requests++;
        cd->received++;

        if (cd->first_fcfs) {
            cd->last_fcfs_sector = entry->sector;
            cd->first_fcfs = false;
        } else {
            if (entry->sector > cd->last_fcfs_sector)
                cd->fcfs_distance += entry->sector - cd->last_fcfs_sector;
            else
                cd->fcfs_distance += cd->last_fcfs_sector - entry->sector;

            cd->last_fcfs_sector = entry->sector;
        }

        if (cd->debug) pr_info("clook: FCFS: %llu\n", (unsigned long long)entry->sector);
    }

    if (cd->nr_requests >= cd->max_requests) {
        cd->queue_full = true;
        run_queue = true;

        if (cd->debug)
            pr_info("clook: fila cheia (%u/%u), solicitando despacho\n",
                    cd->nr_requests, cd->max_requests);
    }

    if (cd->nr_requests > 0) {
        cd->timeout_expired = false;
        restart_timer = true;
    }

    spin_unlock_irqrestore(&cd->lock, irqflags);

    if (restart_timer)
        hrtimer_start(&cd->timer, ms_to_ktime(cd->timeout_ms), HRTIMER_MODE_REL);

    if (run_queue)
        blk_mq_run_hw_queue(hctx, true);
}

static struct request *clook_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
    struct clook_data *cd;
    struct clook_request *entry;
    struct clook_request *best = NULL;
    struct request *rq;
    sector_t pos;
    unsigned long flags;

    cd = hctx->queue->elevator->elevator_data;

    spin_lock_irqsave(&cd->lock, flags);

    if (list_empty(&cd->queue)) {
        spin_unlock_irqrestore(&cd->lock, flags);
        return NULL;
    }

    list_for_each_entry(entry, &cd->queue, list) {

        pos = entry->sector;

        if (pos >= cd->head_position) {

            if (!best || pos < best->sector)
                best = entry;
        }
    }

    if (!best) {

        list_for_each_entry(entry, &cd->queue, list) {

            if (!best || entry->sector < best->sector)
                best = entry;
        }

        cd->circular_jumps++;

        if (cd->debug)
            pr_info("clook: salto circular %llu -> %llu; direcao=crescente\n",
                    (unsigned long long)cd->head_position,
                    (unsigned long long)best->sector);
    }

    rq = best->rq;
    pos = best->sector;

    if (cd->first_clook) {
        cd->last_clook_sector = pos;
        cd->first_clook = false;
    } else {

        if (pos > cd->last_clook_sector)
            cd->clook_distance += pos - cd->last_clook_sector;
        else
            cd->clook_distance += cd->last_clook_sector - pos;

        cd->last_clook_sector = pos;
    }

    cd->head_position = pos;
    cd->nr_requests--;
    cd->dispatched++;

    list_del_init(&best->list);
    kfree(best);

    spin_unlock_irqrestore(&cd->lock, flags);

    if (cd->debug)
        pr_info("clook: CLOOK: setor=%llu direcao=crescente\n",
                (unsigned long long)pos);

    return rq;
}

static bool clook_has_work(struct blk_mq_hw_ctx *hctx)
{
    struct clook_data *cd;
    unsigned long flags;
    bool work;

    cd = hctx->queue->elevator->elevator_data;

    spin_lock_irqsave(&cd->lock, flags);

    work = (cd->queue_full || cd->timeout_expired) && (cd->nr_requests > 0);

    if (!work) {
        cd->queue_full = false;
        cd->timeout_expired = false;
    }

    spin_unlock_irqrestore(&cd->lock, flags);

    return work;
}

static void clook_finish_request(struct request *rq)
{
    (void)rq;
    return;
}

static enum hrtimer_restart clook_timer_callback(struct hrtimer *timer)
{
    struct clook_data *cd;
    struct blk_mq_hw_ctx *hctx;
    unsigned long flags;

    cd = container_of(timer, struct clook_data, timer);

    spin_lock_irqsave(&cd->lock, flags);

    cd->timeout_expired = true;
    hctx = cd->hctx;

    spin_unlock_irqrestore(&cd->lock, flags);

    if (hctx)
        blk_mq_run_hw_queue(hctx, true);

    if (cd->debug)
        pr_info("clook: timeout expirado\n");

    return HRTIMER_NORESTART;
}

static int __init clook_init(void)
{
    pr_info("C-LOOK: registrando escalonador\n");

    return elv_register(&clook);
}

static void __exit clook_exit(void)
{
    pr_info("C-LOOK: removendo escalonador\n");
    elv_unregister(&clook);
}

module_init(clook_init);
module_exit(clook_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bruno, Joao Victor, Lucas e  Cleysso");
MODULE_DESCRIPTION("C-LOOK (Circular LOOK) I/O scheduler");
