// SPDX-License-Identifier: GPL-2.0
/*
 * clook.c - Escalonador de E/S C-LOOK (Circular LOOK) para blk-mq
 *
 * Este modulo implementa um escalonador de disco baseado no algoritmo
 * C-LOOK para a camada blk-mq do kernel Linux.
 *
 * Funcionamento geral:
 *   1. As requisicoes que chegam sao acumuladas em uma fila interna
 *      (em ordem de chegada / FCFS).
 *   2. O despacho so ocorre quando uma de duas condicoes e satisfeita:
 *        - a fila atinge 'queue_size' requisicoes (fila cheia); ou
 *        - o temporizador de 'timeout_ms' milissegundos expira.
 *      Isso permite acumular um lote de requisicoes antes de ordena-las,
 *      maximizando o ganho do C-LOOK.
 *   3. No despacho, escolhe-se sempre a requisicao com o menor setor
 *      maior ou igual a posicao atual da cabeca (varredura crescente).
 *      Quando nao ha mais setores a frente, e feito um "salto circular"
 *      para o menor setor pendente, reiniciando a varredura.
 *
 * O modulo tambem contabiliza estatisticas comparativas: a distancia
 * total (em setores) que a cabeca percorreria atendendo em ordem FCFS
 * versus a distancia efetivamente percorrida com o C-LOOK, permitindo
 * medir a "economia" de movimento ao remover o escalonador.
 *
 * Parametros do modulo:
 *   queue_size - numero de requisicoes acumuladas antes do despacho
 *   timeout_ms - tempo maximo de espera da fila, em milissegundos
 *   debug      - habilita logs de depuracao no log do kernel
 *
 * Autores: Bruno, Joao Victor, Lucas e Cleysso
 */

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

/* Prototipos das operacoes do elevator e das rotinas do modulo */
static int clook_init_sched(struct request_queue *q, struct elevator_type *e);
static void clook_exit_sched(struct elevator_queue *e);
static void clook_insert_requests(struct blk_mq_hw_ctx *hctx, struct list_head *list, blk_insert_t flags);
static struct request *clook_dispatch_request(struct blk_mq_hw_ctx *hctx);
static bool clook_has_work(struct blk_mq_hw_ctx *hctx);
static void clook_finish_request(struct request *rq);
static int __init clook_init(void);
static void __exit clook_exit(void);
static enum hrtimer_restart clook_timer_callback(struct hrtimer *timer);

/*
 * Descritor do escalonador registrado na camada de blocos.
 * A tabela .ops liga os ganchos do blk-mq as funcoes deste modulo,
 * e "clook" e o nome exposto em /sys/block/<disco>/queue/scheduler.
 */
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

/**
 * struct clook_data - estado privado do escalonador (um por dispositivo)
 * @queue:            fila interna de requisicoes pendentes, em ordem de
 *                    chegada (a ordenacao C-LOOK e feita no despacho)
 * @lock:             spinlock que protege todo o estado desta estrutura
 * @timer:            hrtimer que forca o despacho apos @timeout_ms sem
 *                    a fila encher
 * @head_position:    setor da ultima requisicao despachada; simula a
 *                    posicao atual da cabeca do disco
 * @nr_requests:      quantidade de requisicoes atualmente na fila
 * @max_requests:     limite que caracteriza "fila cheia" (queue_size)
 * @timeout_ms:       tempo maximo de espera antes de forcar o despacho
 * @timeout_expired:  indica que o temporizador estourou (gatilho de
 *                    despacho por tempo)
 * @queue_full:       indica que a fila atingiu @max_requests (gatilho de
 *                    despacho por tamanho)
 * @debug:            habilita mensagens de depuracao via pr_info()
 * @fcfs_distance:    distancia acumulada (em setores) que seria percorrida
 *                    atendendo as requisicoes em ordem de chegada (FCFS)
 * @clook_distance:   distancia acumulada realmente percorrida na ordem
 *                    de despacho do C-LOOK
 * @last_fcfs_sector: ultimo setor considerado no calculo da distancia FCFS
 * @last_clook_sector:ultimo setor considerado no calculo da distancia C-LOOK
 * @first_fcfs:       true ate a primeira requisicao recebida (evita somar
 *                    distancia a partir de um setor inexistente)
 * @first_clook:      true ate o primeiro despacho (mesma finalidade)
 * @dispatched:       total de requisicoes despachadas
 * @received:         total de requisicoes recebidas
 * @circular_jumps:   quantidade de saltos circulares realizados
 * @hctx:             contexto de hardware mais recente, guardado para que
 *                    o callback do timer possa reativar a fila
 */
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

/**
 * struct clook_request - no da fila interna do escalonador
 * @sector: setor inicial da requisicao (copiado de blk_rq_pos(), usado
 *          como chave de ordenacao do C-LOOK)
 * @rq:     ponteiro para a requisicao original do bloco
 * @list:   encadeamento na fila @clook_data.queue
 */
struct clook_request {
    sector_t sector;
    struct request *rq;
    struct list_head list;
};

/* Valores padrao dos parametros do modulo (ajustaveis na insercao
 * via insmod/modprobe e em /sys/module/clook/parameters/). */
static unsigned int queue_size = 50;
static unsigned int timeout_ms = 50;
static bool debug = false;

module_param(queue_size, uint, 0644);
MODULE_PARM_DESC(queue_size, "Numero de requisicoes acumuladas antes do despacho C-LOOK");

module_param(timeout_ms, uint, 0644);
MODULE_PARM_DESC(timeout_ms, "Tempo maximo de espera da fila, em milissegundos");

module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Habilita logs de depuracao no log do kernel");

/**
 * clook_init_sched - inicializa o escalonador para uma fila de requisicoes
 * @q: fila de requisicoes do dispositivo de bloco
 * @e: tipo de elevator sendo instanciado (clook)
 *
 * Chamada quando o escalonador e ativado para um dispositivo (por exemplo,
 * ao escrever "clook" em /sys/block/<disco>/queue/scheduler). Aloca o
 * elevator_queue e a estrutura privada clook_data, valida os parametros
 * do modulo (aplicando os padroes em caso de valores invalidos), inicializa
 * a fila, o spinlock e o hrtimer, e marca a fila como escalonador de fila
 * unica (QUEUE_FLAG_SQ_SCHED), ja que todo o estado e centralizado em uma
 * unica estrutura protegida por lock.
 *
 * Retorna: 0 em caso de sucesso ou -ENOMEM se faltar memoria.
 */
static int clook_init_sched(struct request_queue *q, struct elevator_type *e)
{
    struct elevator_queue *eq;
    struct clook_data *cd;

    eq = elevator_alloc(q, e);
    if (!eq)
        return -ENOMEM;

    cd = kzalloc(sizeof(*cd), GFP_KERNEL);
    if (!cd) {
        /* Libera o elevator_queue alocado acima antes de falhar */
        kobject_put(&eq->kobj);
        return -ENOMEM;
    }

    INIT_LIST_HEAD(&cd->queue);
    spin_lock_init(&cd->lock);

    /* Validacao dos parametros: valores invalidos caem no padrao (50) */
    if (queue_size == 0) {
        pr_warn("clook: queue_size=0 invalido; usando 50\n");
        cd->max_requests = 50;
    } else {
        cd->max_requests = queue_size;
    }

    if (timeout_ms <= 0) {
        pr_warn("clook: timeout_ms= %d invalido; usando 50 ms\n", timeout_ms);
        cd->timeout_ms = 50;
    } else {
        cd->timeout_ms = timeout_ms;
    }

    cd->debug = debug;

    /* Estado inicial: cabeca no setor 0, fila vazia, sem gatilhos ativos */
    cd->head_position = 0;
    cd->nr_requests = 0;

    cd->timeout_expired = false;
    cd->queue_full = false;

    /* Flags de "primeira amostra" das estatisticas de distancia */
    cd->first_fcfs = true;
    cd->first_clook = true;

    cd->fcfs_distance = 0;
    cd->clook_distance = 0;

    cd->received = 0;
    cd->dispatched = 0;
    cd->circular_jumps = 0;

    cd->hctx = NULL;

    /* Timer relativo e monotonico usado como gatilho de despacho por tempo */
    hrtimer_init(&cd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    cd->timer.function = clook_timer_callback;

    eq->elevator_data = cd;

    /* Escalonador de fila unica: evita concorrencia entre multiplos hctx */
    blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);

    /* Garante profundidade suficiente na fila para acumular o lote */
    q->nr_requests = 128;
    q->elevator = eq;

    pr_info("clook: scheduler initialized queue_size=%u timeout_ms=%u debug=%d\n",
            cd->max_requests, cd->timeout_ms, cd->debug);

    return 0;
}

/**
 * clook_exit_sched - finaliza o escalonador e libera seus recursos
 * @e: elevator_queue associado ao escalonador
 *
 * Chamada quando o escalonador e desativado do dispositivo. Cancela o
 * temporizador, libera eventuais entradas ainda pendentes na fila interna,
 * imprime o resumo das estatisticas (total despachado, distancias FCFS e
 * C-LOOK, economia de setores, requisicoes recebidas e saltos circulares)
 * e libera a estrutura privada.
 */
static void clook_exit_sched(struct elevator_queue *e)
{
    struct clook_data *cd;
    struct clook_request *entry, *tmp;
    unsigned long long economy;

    cd = e->elevator_data;

    if (!cd)
        return;

    /* Garante que o callback do timer nao rodara apos a liberacao */
    hrtimer_cancel(&cd->timer);

    /* Esvazia a fila interna, liberando os nos restantes */
    list_for_each_entry_safe(entry, tmp, &cd->queue, list) {
        list_del_init(&entry->list);
        kfree(entry);
    }

    /* Economia = quanto o C-LOOK reduziu de movimento em relacao ao FCFS */
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

/**
 * clook_insert_requests - insere novas requisicoes na fila do escalonador
 * @hctx:  contexto de hardware do blk-mq
 * @list:  lista de requisicoes a inserir
 * @flags: flags de insercao (nao utilizadas neste escalonador)
 *
 * Chamada pela camada de blocos para entregar requisicoes ao escalonador.
 * Cada requisicao e removida da lista de entrada, embrulhada em um
 * clook_request (com o setor inicial copiado) e adicionada ao final da
 * fila interna, preservando a ordem de chegada.
 *
 * Durante a insercao tambem e atualizada a distancia FCFS: a diferenca
 * absoluta entre o setor da nova requisicao e o setor da anterior, como
 * se elas fossem atendidas exatamente na ordem em que chegaram.
 *
 * Se a fila atingir max_requests, marca queue_full e agenda a execucao
 * da fila de hardware para iniciar o despacho. Enquanto houver requisicoes
 * pendentes, o temporizador de timeout e (re)armado, garantindo que o
 * lote nunca espere mais que timeout_ms para ser despachado.
 *
 * Se a alocacao de um no falhar (GFP_ATOMIC, pois estamos sob spinlock),
 * a requisicao e devolvida a lista de entrada em vez de ser perdida.
 */
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

    /* Guarda o hctx para o callback do timer poder reativar a fila */
    cd->hctx = hctx;

    list_for_each_entry_safe(rq, next, list, queuelist) {

        list_del_init(&rq->queuelist);

        /* GFP_ATOMIC: nao podemos dormir segurando o spinlock */
        entry = kmalloc(sizeof(*entry), GFP_ATOMIC);

        if (!entry) {
            /* Sem memoria: devolve a requisicao a lista de origem */
            list_add_tail(&rq->queuelist, list);
            continue;
        }

        entry->rq = rq;
        entry->sector = blk_rq_pos(rq);

        /* Insercao no final: a fila interna mantem a ordem de chegada */
        INIT_LIST_HEAD(&entry->list);
        list_add_tail(&entry->list, &cd->queue);

        cd->nr_requests++;
        cd->received++;

        /*
         * Estatistica FCFS: acumula |setor_atual - setor_anterior|,
         * simulando o deslocamento da cabeca se a ordem de chegada
         * fosse respeitada. A primeira requisicao apenas define o
         * ponto de partida.
         */
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

    /* Gatilho por tamanho: fila cheia libera o despacho do lote */
    if (cd->nr_requests >= cd->max_requests) {
        cd->queue_full = true;
        run_queue = true;

        if (cd->debug)
            pr_info("clook: fila cheia (%u/%u), solicitando despacho\n",
                    cd->nr_requests, cd->max_requests);
    }

    /* Ha requisicoes pendentes: (re)arma o gatilho por tempo */
    if (cd->nr_requests > 0) {
        cd->timeout_expired = false;
        restart_timer = true;
    }

    spin_unlock_irqrestore(&cd->lock, irqflags);

    /* Fora do lock: armar timer e agendar a fila podem ser mais custosos */
    if (restart_timer)
        hrtimer_start(&cd->timer, ms_to_ktime(cd->timeout_ms), HRTIMER_MODE_REL);

    if (run_queue)
        blk_mq_run_hw_queue(hctx, true);
}

/**
 * clook_dispatch_request - seleciona a proxima requisicao na ordem C-LOOK
 * @hctx: contexto de hardware do blk-mq
 *
 * Percorre a fila interna procurando a requisicao com o menor setor que
 * seja maior ou igual a posicao atual da cabeca (head_position), ou seja,
 * a proxima parada na varredura em direcao crescente.
 *
 * Se nenhuma requisicao estiver a frente da cabeca, realiza o salto
 * circular caracteristico do C-LOOK: escolhe a requisicao de menor setor
 * de toda a fila, reiniciando a varredura a partir do inicio do disco
 * (o contador circular_jumps registra cada salto).
 *
 * Antes de devolver a requisicao, atualiza a distancia C-LOOK (diferenca
 * absoluta em relacao ao ultimo setor despachado), avanca head_position
 * para o setor escolhido, atualiza os contadores e remove/libera o no
 * da fila interna.
 *
 * Retorna: a requisicao a ser executada, ou NULL se a fila estiver vazia.
 */
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

    /*
     * 1a passada: menor setor >= head_position
     * (proxima requisicao na direcao crescente da varredura)
     */
    list_for_each_entry(entry, &cd->queue, list) {

        pos = entry->sector;

        if (pos >= cd->head_position) {

            if (!best || pos < best->sector)
                best = entry;
        }
    }

    /*
     * 2a passada (salto circular): nada a frente da cabeca, entao
     * volta ao menor setor pendente e recomeca a varredura crescente.
     */
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

    /*
     * Estatistica C-LOOK: acumula |setor_atual - setor_anterior| na
     * ordem real de despacho, para comparacao com a distancia FCFS.
     */
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

    /* A cabeca "move-se" para o setor da requisicao despachada */
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

/**
 * clook_has_work - informa se o escalonador tem requisicoes prontas
 * @hctx: contexto de hardware do blk-mq
 *
 * Implementa a logica de despacho em lote: so ha "trabalho" quando existem
 * requisicoes pendentes E um dos gatilhos foi acionado (fila cheia ou
 * timeout expirado). Fora dessas condicoes, retorna false para que a
 * camada de blocos deixe as requisicoes acumularem na fila interna.
 *
 * Quando nao ha trabalho, os gatilhos sao rearmados (limpos) para o
 * proximo ciclo de acumulacao.
 *
 * Retorna: true se ha requisicoes prontas para despacho, false caso
 * contrario.
 */
static bool clook_has_work(struct blk_mq_hw_ctx *hctx)
{
    struct clook_data *cd;
    unsigned long flags;
    bool work;

    cd = hctx->queue->elevator->elevator_data;

    spin_lock_irqsave(&cd->lock, flags);

    work = (cd->queue_full || cd->timeout_expired) && (cd->nr_requests > 0);

    /* Sem trabalho pendente: limpa os gatilhos para o proximo lote */
    if (!work) {
        cd->queue_full = false;
        cd->timeout_expired = false;
    }

    spin_unlock_irqrestore(&cd->lock, flags);

    return work;
}

/**
 * clook_finish_request - callback de conclusao de requisicao
 * @rq: requisicao concluida
 *
 * O C-LOOK nao mantem estado por requisicao apos o despacho (o no da fila
 * ja foi liberado em clook_dispatch_request), portanto nada a fazer aqui.
 * O gancho existe apenas para completar a interface do elevator.
 */
static void clook_finish_request(struct request *rq)
{
    (void)rq;
    return;
}

/**
 * clook_timer_callback - gatilho de despacho por tempo
 * @timer: hrtimer embutido em clook_data
 *
 * Executada em contexto de interrupcao quando o lote espera ha mais de
 * timeout_ms sem a fila encher. Marca timeout_expired (fazendo has_work
 * passar a retornar true) e agenda a execucao da fila de hardware para
 * que o despacho comece mesmo com a fila parcialmente cheia.
 *
 * Retorna: HRTIMER_NORESTART - o timer nao se rearma sozinho; ele e
 * rearmado em clook_insert_requests enquanto houver requisicoes pendentes.
 */
static enum hrtimer_restart clook_timer_callback(struct hrtimer *timer)
{
    struct clook_data *cd;
    struct blk_mq_hw_ctx *hctx;
    unsigned long flags;

    /* Recupera a estrutura-mae a partir do ponteiro do timer */
    cd = container_of(timer, struct clook_data, timer);

    spin_lock_irqsave(&cd->lock, flags);

    cd->timeout_expired = true;
    hctx = cd->hctx;

    spin_unlock_irqrestore(&cd->lock, flags);

    /* Acorda a fila de hardware para iniciar o despacho do lote */
    if (hctx)
        blk_mq_run_hw_queue(hctx, true);

    if (cd->debug)
        pr_info("clook: timeout expirado\n");

    return HRTIMER_NORESTART;
}

/**
 * clook_init - ponto de entrada do modulo
 *
 * Registra o escalonador "clook" na camada de blocos, tornando-o
 * disponivel para selecao nos dispositivos.
 *
 * Retorna: 0 em caso de sucesso ou codigo de erro de elv_register().
 */
static int __init clook_init(void)
{
    pr_info("C-LOOK: registrando escalonador\n");

    return elv_register(&clook);
}

/**
 * clook_exit - ponto de saida do modulo
 *
 * Remove o registro do escalonador na camada de blocos.
 */
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
