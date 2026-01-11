#include "board.h"
#include "display.h"
#include "protocol.h" 
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <errno.h>
#include <semaphore.h>
#include <signal.h>

/* Possíveis outcomes do jogo (utilizados por threads para sinalizar estado) */
#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2

#define BUFFER_SIZE 10

// --- ESTRUTURAS EX 1.2 (Produtor-Consumidor) ---
// Pedido de ligação: caminhos para pipes do cliente
typedef struct {
    char req_pipe_path[40];
    char notif_pipe_path[40];
} connection_request_t;

// Buffer circular de pedidos (produtor = thread principal, consumidores = workers)
connection_request_t request_buffer[BUFFER_SIZE];
int buf_in = 0;
int buf_out = 0;

sem_t sem_empty;            // conta entradas livres no buffer
sem_t sem_full;             // conta entradas ocupadas no buffer
pthread_mutex_t buf_mutex;  // protege índices e escrita/leitura do buffer

char global_levels_dir[256]; // directoria que contém os ficheiros .lvl

// --- ESTRUTURAS EX 2 (Sinal SIGUSR1 - Top 5) ---
volatile sig_atomic_t sigusr1_received = 0; // flag ligada pelo handler de sinal

// Estrutura para mapear slots activos para estados de jogo e ID real do cliente
typedef struct {
    game_state_t *state;
    int real_client_id;
} active_game_slot_t;

active_game_slot_t *active_games; 
int max_games_limit = 0;
pthread_mutex_t active_games_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int id;
    int score;
} client_score_t;

// --- FUNÇÕES AUXILIARES EX 2 ---

/* Comparator para ordenar descendentemente por score */
int compare_scores(const void *a, const void *b) {
    client_score_t *s1 = (client_score_t *)a;
    client_score_t *s2 = (client_score_t *)b;
    return s2->score - s1->score; 
}

/* Handler simples que marca que SIGUSR1 foi recebido.
   Note que handlers devem ser simples e usar apenas operações async-signal-safe. */
void handle_sigusr1(int sig) {
    (void)sig;
    sigusr1_received = 1;
}

/* Extrai o primeiro inteiro numérico encontrado no nome do pipe.
   Usado para deduzir um ID de cliente a partir do nome do FIFO. */
int extract_id_from_pipe(const char *pipe_path) {
    int id = -1;
    for (int i = 0; pipe_path[i] != '\0'; i++) {
        if (pipe_path[i] >= '0' && pipe_path[i] <= '9') {
            id = atoi(&pipe_path[i]);
            break; 
        }
    }
    return id;
}

/* Leitura robusta que garante leitura de 'count' bytes (ou falha).
   Evita lixo de memória quando se espera uma string fixa. */
int read_exact_server(int fd, void *buffer, size_t count) {
    size_t total_read = 0;
    char *buf_ptr = (char *)buffer;
    while (total_read < count) {
        ssize_t n = read(fd, buf_ptr + total_read, count - total_read);
        if (n == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; 
        total_read += n;
    }
    return 0;
}

/* Gera ficheiro "top_scores.txt" com os 5 melhores clientes.
   Percorre a lista global de jogos activos (protegida por mutex). */
void generate_top5_file() {
    FILE *f = fopen("top_scores.txt", "w");
    if (!f) return;

    client_score_t *scores = malloc(sizeof(client_score_t) * max_games_limit);
    if (!scores) { fclose(f); return; }
    
    int count = 0;

    pthread_mutex_lock(&active_games_mutex);
    for (int i = 0; i < max_games_limit; i++) {
        if (active_games[i].state != NULL && active_games[i].state->board != NULL) {
            scores[count].id = active_games[i].real_client_id; // ID real
            if (active_games[i].state->board->n_pacmans > 0)
                scores[count].score = active_games[i].state->board->pacmans[0].points;
            else
                scores[count].score = 0;
            count++;
        }
    }
    pthread_mutex_unlock(&active_games_mutex);

    qsort(scores, count, sizeof(client_score_t), compare_scores);

    fprintf(f, "=== TOP 5 CLIENTS ===\n");
    int limit = (count < 5) ? count : 5;
    for (int i = 0; i < limit; i++) {
        fprintf(f, "Client ID: %d | Score: %d\n", scores[i].id, scores[i].score);
    }
    
    printf("[SERVER] SIGUSR1 recebido. Ficheiro 'top_scores.txt' gerado.\n");
    free(scores);
    fclose(f);
}

// --- ESTRUTURAS DE JOGO ---
// Contexto enviado à thread de render para saber fds e estado do jogo
typedef struct {
    game_state_t *state;
    int req_fd;
    int notif_fd;
} render_context_t;

/* Serializa o estado do tabuleiro para enviar ao cliente.
   Formato: 1 byte op + 6 ints (width,height,tempo,victory,game_over,points) + grid chars.
   Retorna buffer alocado e define out_size. */
char* capture_snapshot(board_t *board, int outcome, int *out_size) {
    char op = OP_CODE_BOARD;
    int victory = (outcome == NEXT_LEVEL);
    int game_over = (outcome == QUIT_GAME);
    int points = (board->n_pacmans > 0) ? board->pacmans[0].points : 0;
    
    int header_size = 1 + (6 * sizeof(int));
    int grid_size = board->width * board->height;
    int total_size = header_size + grid_size;

    char *buffer = malloc(total_size);
    if (!buffer) return NULL;

    int offset = 0;
    
    memcpy(buffer + offset, &op, 1); offset += 1;
    memcpy(buffer + offset, &board->width, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &board->height, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &board->tempo, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &victory, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &game_over, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &points, sizeof(int)); offset += sizeof(int);

    for (int i = 0; i < grid_size; i++) {
        int x = i % board->width;
        int y = i / board->width;
        char c = board->board[i].content;

        /* Corrige visualização de fantasmas e pontos/portais */
        if (c == 'M') {
            for (int g = 0; g < board->n_ghosts; g++) {
                if (board->ghosts[g].pos_x == x && board->ghosts[g].pos_y == y) {
                    if (board->ghosts[g].charged) c = 'G';
                    break;
                }
            }
        }
        else if (c == ' ') {
            if (board->board[i].has_portal) c = '@';
            else if (board->board[i].has_dot) c = '.';
        }
        buffer[offset++] = c;
    }

    *out_size = total_size;
    return buffer;
}

/* Define outcome do estado se ainda não tiver sido alterado, e acorda esperas. */
static void set_outcome(game_state_t *state, int outcome) {
    if (state->outcome == CONTINUE_PLAY) {
        state->outcome = outcome;
    }
    state->running = 0;
    pthread_cond_broadcast(&state->input_cond);
}

/* Thread responsável por enviar snapshots para cliente e ler comandos não-bloqueantes. */
static void *render_thread(void *arg) {
    render_context_t *ctx = (render_context_t *)arg;
    game_state_t *state = ctx->state;
    int req_fd = ctx->req_fd;
    int notif_fd = ctx->notif_fd;

    /* Tornar pipe de requests não-bloqueante para poder verificar sem bloquear */
    if (req_fd != -1) {
        int flags = fcntl(req_fd, F_GETFL, 0);
        fcntl(req_fd, F_SETFL, flags | O_NONBLOCK);
    }

    while (1) {
        pthread_mutex_lock(&state->mutex);
        
        int running = state->running;
        int outcome = state->outcome;
        
        int snap_size = 0;
        char *snapshot = capture_snapshot(state->board, outcome, &snap_size);
        int sleep_time = state->board->tempo;

        pthread_mutex_unlock(&state->mutex);

        /* Envia snapshot ao cliente (se existir) */
        if (snapshot && notif_fd != -1) {
            write(notif_fd, snapshot, snap_size);
            free(snapshot);
        }

        if (!running) break;

        /* Lê comandos do cliente: play ou disconnect */
        if (req_fd != -1) {
            char op_code;
            char command;
            int n = read(req_fd, &op_code, 1);
            
            if (n > 0) {
                if (op_code == OP_CODE_PLAY) {
                    if (read(req_fd, &command, 1) > 0) {
                        pthread_mutex_lock(&state->mutex);
                        state->pending_input = command;
                        pthread_cond_broadcast(&state->input_cond);
                        pthread_mutex_unlock(&state->mutex);
                    }
                } else if (op_code == OP_CODE_DISCONNECT) {
                    pthread_mutex_lock(&state->mutex);
                    set_outcome(state, QUIT_GAME);
                    pthread_mutex_unlock(&state->mutex);
                }
            }
        }

        if (sleep_time != 0) {
            sleep_ms(sleep_time);
        }
    }
    return NULL;
}

/* Constrói comando manual com 1 turno (quando cliente envia input) */
static command_t build_manual_command(char input) {
    command_t cmd;
    cmd.command = input;
    cmd.turns = 1;
    cmd.turns_left = 1;
    return cmd;
}

/* Thread que executa o Pacman: espera por input ou usa movimentos predefinidos. */
static void *pacman_thread(void *arg) {
    game_state_t *state = (game_state_t *)arg;
    board_t *board = state->board;
    command_t manual_cmd;

    while (1) {
        pthread_mutex_lock(&state->mutex);
        if (!state->running) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }

        pacman_t *pacman = &board->pacmans[0];
        command_t *cmd_ptr;

        /* Se não houver movimentos automáticos, espera por input do cliente */
        if (pacman->n_moves == 0) {
            while (state->pending_input == '\0' && state->running) {
                pthread_cond_wait(&state->input_cond, &state->mutex);
            }
            if (!state->running) {
                pthread_mutex_unlock(&state->mutex);
                break;
            }
            manual_cmd = build_manual_command(state->pending_input);
            state->pending_input = '\0';
            cmd_ptr = &manual_cmd;
        } else {
            int cmd_index = pacman->current_move % pacman->n_moves;
            cmd_ptr = &pacman->moves[cmd_index];
        }
        pthread_mutex_unlock(&state->mutex);

        if (cmd_ptr->command == 'Q') {
            pthread_mutex_lock(&state->mutex);
            set_outcome(state, QUIT_GAME);
            pthread_mutex_unlock(&state->mutex);
            continue;
        }

        pthread_mutex_lock(&state->mutex);
        int is_running = state->running;
        pthread_mutex_unlock(&state->mutex);
        if (!is_running) break;
        
        int result = move_pacman(board, 0, cmd_ptr);
        
        /* Se alcançar portal ou morrer, atualiza outcome e termina */
        if (result == REACHED_PORTAL || result == DEAD_PACMAN) {
            pthread_mutex_lock(&state->mutex);
            if (result == REACHED_PORTAL) set_outcome(state, NEXT_LEVEL);
            else if (result == DEAD_PACMAN) set_outcome(state, QUIT_GAME);
            pthread_mutex_unlock(&state->mutex);
        }

        if (board->tempo != 0) sleep_ms(board->tempo);
    }
    return NULL;
}

/* Thread para cada fantasma; executa os movimentos do fantasma e verifica colisões. */
static void *ghost_thread(void *arg) {
    ghost_thread_args_t *ghost_args = (ghost_thread_args_t *)arg;
    game_state_t *state = ghost_args->state;
    int ghost_index = ghost_args->ghost_index;
    board_t *board = state->board;

    while (1) {
        pthread_mutex_lock(&state->mutex);
        if (!state->running) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }

        ghost_t *ghost = &board->ghosts[ghost_index];
        if (ghost->n_moves == 0) {
            pthread_mutex_unlock(&state->mutex);
            if (board->tempo != 0) sleep_ms(board->tempo);
            continue;
        }

        int cmd_index = ghost->current_move % ghost->n_moves;
        command_t *cmd_ptr = &ghost->moves[cmd_index];
        pthread_mutex_unlock(&state->mutex);

        pthread_mutex_lock(&state->mutex);
        int is_running = state->running;
        pthread_mutex_unlock(&state->mutex);
        if (!is_running) break;

        int result = move_ghost(board, ghost_index, cmd_ptr);
        
        if (result == DEAD_PACMAN) {
            pthread_mutex_lock(&state->mutex);
            set_outcome(state, QUIT_GAME);
            pthread_mutex_unlock(&state->mutex);
        }

        if (board->tempo != 0) sleep_ms(board->tempo);
    }
    return NULL;
}

/* Utilitário: verifica se ficheiro tem determinada extensão */
int has_extension(const char *filename, const char *ext) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return 0;
    return (strcmp(dot, ext) == 0);
}

/* Lista ficheiros .lvl na directoria especificada, devolve count.
   Se não conseguir abrir a directoria pedida, tenta ".". */
int find_levels(const char *dirpath, char lista[MAX_LEVELS][MAX_FILENAME]) {
    DIR *dirp = opendir(dirpath);
    if (dirp == NULL) {
        if (strcmp(dirpath, ".") != 0) return find_levels(".", lista);
        perror("Error opening directory");
        return 0;
    }
    struct dirent *dp;
    int count = 0;
    while ((dp = readdir(dirp)) != NULL) {
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0) continue;
        if (has_extension(dp->d_name, ".lvl") && count < MAX_LEVELS) {
            strncpy(lista[count], dp->d_name, MAX_FILENAME - 1);
            lista[count][MAX_FILENAME - 1] = '\0';
            count++;
        }
    }
    closedir(dirp);
    return count;
}

// --- FUNÇÃO DE JOGO (Atualizada para Ex 2) ---
/* Executa um jogo completo: cria threads (render, pacman, fantasmas),
   aguarda término e atualiza a lista global de jogos activos. */
int run_game(board_t *board, int req_fd, int notif_fd, int slot_id, int client_id) {
    game_state_t state = {
        .board = board, .running = 1, .outcome = CONTINUE_PLAY,
        .pending_input = '\0', .save_request = 0
    };
    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.input_cond, NULL);

    // Registar o jogo na lista global para possibilitar geração de TOP e inspeção
    pthread_mutex_lock(&active_games_mutex);
    active_games[slot_id].state = &state;
    active_games[slot_id].real_client_id = client_id;
    pthread_mutex_unlock(&active_games_mutex);

    pthread_t render_tid, pacman_tid;
    pthread_t ghost_tids[MAX_GHOSTS];
    ghost_thread_args_t ghost_args[MAX_GHOSTS];

    render_context_t render_ctx = { .state = &state, .req_fd = req_fd, .notif_fd = notif_fd };

    pthread_create(&render_tid, NULL, render_thread, &render_ctx);
    pthread_create(&pacman_tid, NULL, pacman_thread, &state);

    for (int g = 0; g < board->n_ghosts; g++) {
        ghost_args[g].state = &state;
        ghost_args[g].ghost_index = g;
        pthread_create(&ghost_tids[g], NULL, ghost_thread, &ghost_args[g]);
    }

    pthread_join(pacman_tid, NULL);
    for (int g = 0; g < board->n_ghosts; g++) pthread_join(ghost_tids[g], NULL);
    pthread_join(render_tid, NULL);

    // Remover jogo da lista global (já terminou)
    pthread_mutex_lock(&active_games_mutex);
    active_games[slot_id].state = NULL;
    active_games[slot_id].real_client_id = -1;
    pthread_mutex_unlock(&active_games_mutex);

    int outcome = state.outcome;
    pthread_mutex_destroy(&state.mutex);
    pthread_cond_destroy(&state.input_cond);
    return outcome;
}

/* Remove FIFO ao terminar (utilitário simples) */
void cleanup_server(const char* fifo_name) {
    unlink(fifo_name);
}

// --- WORKER THREAD ---
/* Cada worker consome pedidos do buffer circular e gere um cliente:
   abre pipes do cliente, envia confirmação e executa níveis sequencialmente. */
void *worker_thread(void *arg) {
    int my_id = *(int*)arg; 
    free(arg);

    /* Bloqueia SIGUSR1 nesta thread para forçar tratamento apenas na main */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    while (1) {
        sem_wait(&sem_full);
        pthread_mutex_lock(&buf_mutex);
        connection_request_t req = request_buffer[buf_out];
        buf_out = (buf_out + 1) % BUFFER_SIZE;
        pthread_mutex_unlock(&buf_mutex);
        sem_post(&sem_empty);

        int client_notif_fd = open(req.notif_pipe_path, O_WRONLY);
        int client_req_fd = open(req.req_pipe_path, O_RDONLY);

        if (client_notif_fd == -1 || client_req_fd == -1) {
            if (client_notif_fd != -1) close(client_notif_fd);
            if (client_req_fd != -1) close(client_req_fd);
            continue;
        }

        /* Envia resposta de connect (protocolar) */
        char res_op = OP_CODE_CONNECT, res_val = 0;
        write(client_notif_fd, &res_op, 1);
        write(client_notif_fd, &res_val, 1);

        char lista_niveis[MAX_LEVELS][MAX_FILENAME];
        int n_niveis = find_levels(".", lista_niveis);
        int points = 0;

        // Extrair ID do pipe (se não for possível, usa id do worker)
        int real_id = extract_id_from_pipe(req.req_pipe_path);
        if (real_id == -1) real_id = my_id;

        for (int i = 0; i < n_niveis; i++) {
            /* Se o fd do cliente deixou de ser válido, sai do loop */
            if (fcntl(client_req_fd, F_GETFD) == -1) break;
            board_t game_board = {0};
            if (load_level_filename(&game_board, lista_niveis[i], points) != 0) continue;
            strncpy(game_board.level_name, lista_niveis[i], 255);
            
            int outcome = run_game(&game_board, client_req_fd, client_notif_fd, my_id, real_id);
            
            points = (game_board.n_pacmans > 0) ? game_board.pacmans[0].points : points;
            unload_level(&game_board);

            if (outcome == QUIT_GAME) break;
            sleep_ms(1000); // pequena pausa entre níveis
        }
        close(client_notif_fd);
        close(client_req_fd);
    }
    return NULL;
}

// --- MAIN ---
int main(int argc, char** argv) {
    /* Ignora SIGPIPE para evitar encerramento se cliente fechar o FIFO
       enquanto escrevemos. */
    signal(SIGPIPE, SIG_IGN);

    if (argc != 4) {    
        fprintf(stderr, "Uso: %s <dir_niveis> <max_jogos> <fifo_registo>\n", argv[0]);
        return 1;
    }

    strncpy(global_levels_dir, argv[1], 255);
    int max_games = atoi(argv[2]);
    max_games_limit = max_games;
    char *server_fifo_name = argv[3];

    if (chdir(global_levels_dir) != 0) {
        perror("Erro ao mudar de diretoria");
        return 1;
    }

    /* Inicializa semáforos e mutex para o buffer de pedidos */
    sem_init(&sem_empty, 0, BUFFER_SIZE);
    sem_init(&sem_full, 0, 0);
    pthread_mutex_init(&buf_mutex, NULL);
    
    // Alocar array global de jogos activos (inicialmente vazios)
    active_games = calloc(max_games, sizeof(active_game_slot_t));

    /* Configurar handler de SIGUSR1 para gerar ficheiro TOP5 quando recebido */
    struct sigaction sa;
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; 
    sigaction(SIGUSR1, &sa, NULL);

    /* Criar threads workers que irão aceitar pedidos do buffer */
    pthread_t *workers = malloc(sizeof(pthread_t) * max_games);
    for (int i = 0; i < max_games; i++) {
        int *arg = malloc(sizeof(int));
        *arg = i;
        pthread_create(&workers[i], NULL, worker_thread, arg);
    }

    if (mkfifo(server_fifo_name, 0666) == -1 && errno != EEXIST) {
        perror("Erro mkfifo server");
        return 1;
    }

    // Mostra PID para o user saber qual matar (útil para testes com sinais)
    printf("[SERVIDOR] A aguardar clientes em %s (Max: %d) | PID: %d\n", server_fifo_name, max_games, getpid());
    srand((unsigned int)time(NULL));
    open_debug_file("server_debug.log");

    /* Loop principal: aceita pedidos de registo (connect) através do FIFO do servidor.
       Cada pedido contém: op (1 byte) + req_pipe (40 bytes) + notif_pipe (40 bytes). */
    while (1) {
        int server_fd = open(server_fifo_name, O_RDONLY);
        if (server_fd == -1) {
            if (errno == EINTR) {
                if (sigusr1_received) {
                    generate_top5_file();
                    sigusr1_received = 0;
                }
                continue;
            }
            continue; 
        }

        char op;
        char req_pipe[40] = {0}; 
        char notif_pipe[40] = {0};

        int n = read(server_fd, &op, 1);
        
        if (n == -1 && errno == EINTR) {
             if (sigusr1_received) {
                generate_top5_file();
                sigusr1_received = 0;
            }
            close(server_fd);
            continue;
        }

        if (n > 0) {
            /* Usa read_exact_server para evitar lixo de memória e ler exactamente 40 bytes */
            if (read_exact_server(server_fd, req_pipe, 40) == 0 &&
                read_exact_server(server_fd, notif_pipe, 40) == 0) {
                
                if (op == OP_CODE_CONNECT) {
                    /* Enfileira pedido no buffer circular (produtor) */
                    sem_wait(&sem_empty);
                    pthread_mutex_lock(&buf_mutex);
                    strncpy(request_buffer[buf_in].req_pipe_path, req_pipe, 40);
                    strncpy(request_buffer[buf_in].notif_pipe_path, notif_pipe, 40);
                    buf_in = (buf_in + 1) % BUFFER_SIZE;
                    pthread_mutex_unlock(&buf_mutex);
                    sem_post(&sem_full);
                }
            }
        }
        close(server_fd);
    }

    /* Nunca alcançado no ciclo actual, mas mantido para limpeza */
    close_debug_file();
    cleanup_server(server_fifo_name);
    free(workers);
    return 0;
}