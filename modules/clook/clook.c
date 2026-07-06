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

/*
Outras funções úteis:

    blk_mq_run_hw_queue(hctx, true): força o kernel a disparar uma chamada do has_work;
    blk_rq_pos(rq): retorna o número do setor de uma requisição struct request *rq;

Outras dicas:

    Use linux/hrtimer.h para implementar o temporizador. Já trabalhamos com essa API no laboratório 2.4;
    As callbacks insert_requests e dispatch_request podem ser chamadas concorrentemente, mas não podem ser postas para dormir por uma mutex. Para resolver, use uma spinlock quando for acessar a fila do C-LOOK;
    O hrtimer pode modificar o estado dos dados do C-LOOK, necessitando também o uso da spinlock. Porém, o hrtimer usa dentro de uma interrupção. Portanto, sempre use spin_lock_irqsave e spin_lock_irqrestore ao invés de spin_lock e spin_unlock. Verifique a documentação da spinlock no Moodle.

 */
