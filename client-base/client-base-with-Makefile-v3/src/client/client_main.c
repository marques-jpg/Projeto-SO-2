#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h> // Necessário para sinalizar SIGPIPE e SIGINT

// Variáveis globais
Board board;
bool stop_execution = false;
int tempo;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Handler para garantir que o terminal é restaurado em caso de interrupção forçada
void handle_sigint(int sig) {
    (void)sig;
    terminal_cleanup();
    exit(0);
}

static void *receiver_thread(void *arg) {
    (void)arg;

    while (true) {
        // Recebe atualização do servidor (bloqueante)
        Board local_board = receive_board_update();

        // Se o jogo acabou ou houve erro (pipe fechado)
        if (!local_board.data || local_board.game_over == 1){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            
            // Se tiver dados (caso de game over com último frame), desenha uma última vez
            if (local_board.data) {
                draw_board_client(local_board);
                refresh_screen();
                free(local_board.data);
            }
            break;
        }

        // Atualizar tempo global para o input saber a velocidade
        pthread_mutex_lock(&mutex);
        tempo = local_board.tempo;
        pthread_mutex_unlock(&mutex);

        draw_board_client(local_board);
        refresh_screen();
        
        // Libertar memória alocada no receive_board_update
        if (local_board.data) free(local_board.data);
    }

    debug("Returning receiver thread...\n");
    return NULL;
}

int main(int argc, char *argv[]) {
    // 1. Ignorar SIGPIPE (Evita crash se servidor desligar e tentarmos escrever)
    signal(SIGPIPE, SIG_IGN);
    // 2. Capturar Ctrl+C para restaurar terminal
    signal(SIGINT, handle_sigint);

    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    // Preparar caminhos dos pipes
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH, "/tmp/%s_request", client_id);
    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH, "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    // Ligar ao servidor
    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        return 1;
    }

    // 3. INICIALIZAR TERMINAL ANTES DA THREAD
    // Isto evita que a thread tente desenhar antes do ncurses estar pronto
    terminal_init();
    set_timeout(500); // Timeout para o get_input não bloquear para sempre

    // Desenhar estado inicial (vazio)
    draw_board_client(board);
    refresh_screen();

    // Iniciar thread que desenha o jogo (AGORA é seguro)
    pthread_t receiver_thread_id;
    pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL);

    char command;
    int ch;

    // Loop principal de Input
    while (1) {
        pthread_mutex_lock(&mutex);
        if (stop_execution) {
            pthread_mutex_unlock(&mutex);
            break; // Sai do loop corretamente
        }
        pthread_mutex_unlock(&mutex);

        if (cmd_fp) {
            // Leitura de ficheiro
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                // Se acabou o ficheiro, não faz rewind eterno, espera pelo fim do jogo ou quit
                sleep_ms(100); 
                continue;
            }

            command = (char)ch;

            if (command == '\n' || command == '\r' || command == '\0')
                continue;

            command = toupper(command);
            
            // Pausa para sincronizar com a velocidade do jogo
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            if (wait_for > 0) sleep_ms(wait_for);
            
        } else {
            // Leitura interativa (Teclado)
            command = get_input();
            command = toupper(command);
        }

        if (command == '\0' || command == ERR) // ERR é retornado pelo ncurses no timeout
            continue;

        if (command == 'Q') {
            debug("Client pressed 'Q', quitting game\n");
            // Nota: Não fazemos break imediato para permitir limpeza correta
            // Mas para sair já, vamos avisar o loop e desconectar
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        debug("Command: %c\n", command);
        pacman_play(command);
    }

    // Limpeza
    pacman_disconnect();

    // Esperar que a thread de receção termine
    pthread_join(receiver_thread_id, NULL);

    if (cmd_fp) fclose(cmd_fp);

    pthread_mutex_destroy(&mutex);
    
    // IMPORTANTE: Restaurar terminal
    terminal_cleanup();
    close_debug_file();

    return 0;
}