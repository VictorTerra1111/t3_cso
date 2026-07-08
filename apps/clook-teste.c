#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <sys/wait.h>

int main(int argc, char *argv[])
{
    if (argc != 7) {
        fprintf(stderr, "Uso: %s <arquivo> <tam_bloco_bytes> <tam_disco_blocos> <num_operacoes> <pct_escrita> <num_processos>\n", argv[0]);
        fprintf(stderr, "Exemplo: %s /dev/sdb 4096 1024 1000 30 4\n", argv[0]);
        return 1;
    }

    char *filename = argv[1];
    long block_size = atol(argv[2]);
    long disk_blocks = atol(argv[3]);
    int operations = atoi(argv[4]);
    int pct_write = atoi(argv[5]);
    int num_processes = atoi(argv[6]);

    char *buffer = malloc(block_size);
    if (!buffer) {
        perror("malloc");
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

            int fd = open(filename, O_RDWR | O_DIRECT); /
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

                if ((rand() % 100) < pct_write) {
                    if (write(fd, buffer, block_size) < 0) perror("write");
                } else {
                    if (read(fd, buffer, block_size) < 0) perror("read");
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