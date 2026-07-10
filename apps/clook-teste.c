#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <sys/wait.h>

int main(int argc, char *argv[])
{
    if (argc != 9) {
        fprintf(stderr, "Uso: %s <arquivo> <tam_bloco_bytes> <tam_disco_blocos> <num_operacoes> <pct_escrita> <tam_min_req> <tam_max_req> <num_processos>\n", argv[0]);
        fprintf(stderr, "Exemplo: %s /dev/sdb 4096 1024 1000 30 1024 4096 4\n", argv[0]);
        return 1;
    }

    char *filename = argv[1];
    long block_size = atol(argv[2]);
    long disk_blocks = atol(argv[3]);
    int operations = atoi(argv[4]);
    int pct_write = atoi(argv[5]);
    long min_req_size = atol(argv[6]); 
    long max_req_size = atol(argv[7]); 
    int num_processes = atoi(argv[8]);

    if (min_req_size > max_req_size || max_req_size > block_size) {
        fprintf(stderr, "Erro: tam_min_req deve ser <= tam_max_req e tam_max_req deve ser <= tam_bloco_bytes\n");
        return 1;
    }

    char *buffer = NULL;
    if (posix_memalign((void **)&buffer, 4096, block_size) != 0) {
        perror("posix_memalign");
        return 1;
    }
    memset(buffer, 0xAA, block_size);

    printf("Iniciando teste concorrente: %d processos gerando %d operacoes cada...\n", num_processes, operations);

    for (int p = 0; p < num_processes; p++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) { 
            srand(time(NULL) ^ (getpid() << 16));

            int fd = open(filename, O_RDWR | O_DIRECT);
            if (fd < 0) {
                fd = open(filename, O_RDWR); 
                if (fd < 0) {
                    perror("open filho");
                    exit(1);
                }
            }

            for (int i = 0; i < operations; i++) {
                off_t offset = (rand() % disk_blocks) * block_size;

                if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
                    perror("lseek");
                    break;
                }
                long req_size = min_req_size + (rand() % (max_req_size - min_req_size + 1));

                if ((rand() % 100) < pct_write) {
                    if (write(fd, buffer, req_size) < 0) perror("write");
                } else {
                    if (read(fd, buffer, req_size) < 0) perror("read");
                }
            }

            fsync(fd);
            close(fd);
            free(buffer);
            exit(0); 
        }
    }

    for (int p = 0; p < num_processes; p++) {
        wait(NULL);
    }

    free(buffer);
    printf("Teste concluido com sucesso para todos os processos.\n");
    return 0;
}