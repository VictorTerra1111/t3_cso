#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIRECT_IO_ALIGNMENT 512L

static void usage(const char *prog)
{
    fprintf(stderr,
            "Uso: %s <arquivo_ou_dispositivo> <tam_bloco_bytes> <tam_disco_blocos> "
            "<num_operacoes_por_processo> <pct_escrita> <tam_req_min> <tam_req_max> "
            "<num_processos> [aleatorio|sequencial]\n",
            prog);
    fprintf(stderr,
            "Exemplo aleatorio:   %s /dev/vdb 4096 1024 1000 30 512 4096 8\n",
            prog);
    fprintf(stderr,
            "Exemplo sequencial:  %s /dev/vdb 4096 1024 1000 30 512 4096 4 sequencial\n",
            prog);
}

static long parse_long_arg(const char *text, const char *name)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "Parametro invalido para %s: %s\n", name, text);
        exit(1);
    }

    return value;
}

static int is_power_of_two(long value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

static long random_between(unsigned int *seed, long min, long max)
{
    long range = max - min + 1;
    return min + (long)(rand_r(seed) % (unsigned int)range);
}

static long random_aligned_size(unsigned int *seed, long min, long max, long alignment)
{
    long aligned_min;
    long aligned_max;
    long slots;

    if (alignment <= 1)
        return random_between(seed, min, max);

    aligned_min = ((min + alignment - 1) / alignment) * alignment;
    aligned_max = (max / alignment) * alignment;

    if (aligned_min > aligned_max)
        return random_between(seed, min, max);

    slots = ((aligned_max - aligned_min) / alignment) + 1;
    return aligned_min + ((long)(rand_r(seed) % (unsigned int)slots) * alignment);
}

static int open_target(const char *filename, int use_direct_io)
{
    int flags = O_RDWR | O_SYNC;
    int fd;

    if (use_direct_io)
        flags |= O_DIRECT;

    fd = open(filename, flags);
    if (fd >= 0)
        return fd;

    if (use_direct_io) {
        perror("open com O_DIRECT falhou; tentando sem O_DIRECT");
        fd = open(filename, O_RDWR | O_SYNC);
    }

    return fd;
}

static int do_io(int fd, char *buffer, size_t size, int is_write)
{
    ssize_t ret;

    if (is_write)
        ret = write(fd, buffer, size);
    else
        ret = read(fd, buffer, size);

    if (ret < 0)
        return -1;

    if ((size_t)ret != size) {
        errno = EIO;
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *filename;
    const char *mode;
    long block_size;
    long disk_blocks;
    long operations;
    long pct_write;
    long req_min;
    long req_max;
    long num_processes;
    int sequential;
    int use_direct_io;
    void *allocated_buffer = NULL;
    char *buffer;

    if (argc != 9 && argc != 10) {
        usage(argv[0]);
        return 1;
    }

    filename = argv[1];
    block_size = parse_long_arg(argv[2], "tam_bloco_bytes");
    disk_blocks = parse_long_arg(argv[3], "tam_disco_blocos");
    operations = parse_long_arg(argv[4], "num_operacoes_por_processo");
    pct_write = parse_long_arg(argv[5], "pct_escrita");
    req_min = parse_long_arg(argv[6], "tam_req_min");
    req_max = parse_long_arg(argv[7], "tam_req_max");
    num_processes = parse_long_arg(argv[8], "num_processos");
    mode = (argc == 10) ? argv[9] : "aleatorio";

    sequential = strcmp(mode, "sequencial") == 0 || strcmp(mode, "seq") == 0;
    if (!sequential && strcmp(mode, "aleatorio") != 0 && strcmp(mode, "random") != 0) {
        fprintf(stderr, "Modo invalido: %s. Use 'aleatorio' ou 'sequencial'.\n", mode);
        return 1;
    }

    if (!is_power_of_two(block_size)) {
        fprintf(stderr, "tam_bloco_bytes deve ser potencia de dois.\n");
        return 1;
    }

    if (disk_blocks <= 0 || operations <= 0 || num_processes <= 0) {
        fprintf(stderr, "tam_disco_blocos, num_operacoes_por_processo e num_processos devem ser maiores que zero.\n");
        return 1;
    }

    if (pct_write < 0 || pct_write > 100) {
        fprintf(stderr, "pct_escrita deve estar entre 0 e 100.\n");
        return 1;
    }

    if (req_min <= 0 || req_max <= 0 || req_min > req_max || req_max > block_size) {
        fprintf(stderr, "tam_req_min e tam_req_max devem satisfazer: 0 < min <= max <= tam_bloco_bytes.\n");
        return 1;
    }

    use_direct_io = (block_size % DIRECT_IO_ALIGNMENT == 0) &&
                    (req_min % DIRECT_IO_ALIGNMENT == 0) &&
                    (req_max % DIRECT_IO_ALIGNMENT == 0);

    if (posix_memalign(&allocated_buffer, (size_t)block_size, (size_t)block_size) != 0) {
        perror("posix_memalign");
        return 1;
    }

    buffer = allocated_buffer;
    memset(buffer, 0xAA, (size_t)block_size);

    printf("Iniciando teste C-LOOK\n");
    printf("Arquivo/dispositivo: %s\n", filename);
    printf("Bloco: %ld bytes | Disco: %ld blocos | Operacoes/processo: %ld\n",
           block_size, disk_blocks, operations);
    printf("Escritas: %ld%% | Requisicao: %ld a %ld bytes | Processos: %ld | Modo: %s\n",
           pct_write, req_min, req_max, num_processes, sequential ? "sequencial" : "aleatorio");
    printf("O_DIRECT: %s\n", use_direct_io ? "habilitado quando suportado" : "desabilitado");
    fflush(NULL);

    for (long p = 0; p < num_processes; p++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            free(allocated_buffer);
            return 1;
        }

        if (pid == 0) {
            unsigned int seed = (unsigned int)time(NULL) ^ ((unsigned int)getpid() << 16) ^ (unsigned int)p;
            int fd = open_target(filename, use_direct_io);

            if (fd < 0) {
                perror("open");
                free(allocated_buffer);
                exit(1);
            }

            for (long i = 0; i < operations; i++) {
                long block;
                long req_size;
                off_t offset;
                int is_write;

                req_size = random_aligned_size(&seed, req_min, req_max,
                                               use_direct_io ? DIRECT_IO_ALIGNMENT : 1);

                if (sequential)
                    block = (p * operations + i) % disk_blocks;
                else
                    block = (long)(rand_r(&seed) % (unsigned int)disk_blocks);

                offset = (off_t)block * (off_t)block_size;
                is_write = ((long)(rand_r(&seed) % 100U) < pct_write);

                memset(buffer, (int)(0xA0 + (p % 32)), (size_t)req_size);

                if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
                    perror("lseek");
                    close(fd);
                    free(allocated_buffer);
                    exit(1);
                }
                long req_size = min_req_size + (rand() % (max_req_size - min_req_size + 1));

                if (do_io(fd, buffer, (size_t)req_size, is_write) != 0) {
                    perror(is_write ? "write" : "read");
                    close(fd);
                    free(allocated_buffer);
                    exit(1);
                }
            }

            if (fsync(fd) != 0)
                perror("fsync");

            close(fd);
            free(allocated_buffer);
            exit(0);
        }
    }

    for (long p = 0; p < num_processes; p++) {
        int status;

        if (wait(&status) < 0) {
            perror("wait");
            free(allocated_buffer);
            return 1;
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Um processo filho terminou com erro.\n");
            free(allocated_buffer);
            return 1;
        }
    }

    free(allocated_buffer);
    printf("Teste concluido com sucesso para todos os processos.\n");
    return 0;
}
