#include "board.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <pthread.h>
#include <errno.h> // Importante para errno e EINTR

FILE * debugfile;

// --- FUNÇÕES HELPER DO BOARD  ---
// lock_two_positions: trava dois mutexes de posições do tabuleiro.
// Para evitar deadlock, tranca sempre na ordem do menor índice para o maior.
// Se os índices forem iguais, tranca apenas uma vez.
static void lock_two_positions(board_t* board, int idx1, int idx2) {
    if (idx1 == idx2) {
        pthread_mutex_lock(&board->board[idx1].mutex);
        return;
    }
    int first = (idx1 < idx2) ? idx1 : idx2;
    int second = (idx1 < idx2) ? idx2 : idx1;
    pthread_mutex_lock(&board->board[first].mutex);
    pthread_mutex_lock(&board->board[second].mutex);
}

// unlock_two_positions: desbloqueia as duas posições; se iguais, desbloqueia só uma.
static void unlock_two_positions(board_t* board, int idx1, int idx2) {
    pthread_mutex_unlock(&board->board[idx1].mutex);
    if (idx1 != idx2) {
        pthread_mutex_unlock(&board->board[idx2].mutex);
    }
}

/* Procura um pacman vivo na posição (new_x,new_y) e marca-o como morto.
 * Retorna DEAD_PACMAN se matou um pacman, caso contrário VALID_MOVE.
 * Note: chama kill_pacman que altera o conteúdo do tabuleiro (sem lock aqui).
 */
static int find_and_kill_pacman(board_t* board, int new_x, int new_y) {
    for (int p = 0; p < board->n_pacmans; p++) {
        pacman_t* pac = &board->pacmans[p];
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
            pac->alive = 0;
            kill_pacman(board, p);
            return DEAD_PACMAN;
        }
    }
    return VALID_MOVE;
}

// Índice linear no array board->board a partir de coordenadas (x,y)
static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

// Valida se coordenadas estão dentro das dimensões do tabuleiro
static inline int is_valid_position(board_t* board, int x, int y) {
    return (x >= 0 && x < board->width) && (y >= 0 && y < board->height);
}

/* Encontra a primeira posição livre no tabuleiro que não seja parede nem portal.
 * Usado como fallback quando um ficheiro de entidade não especifica POS.
 * Retorna a posição via ponteiros x,y.
 */
static void find_first_free_pos(board_t* board, int* x, int* y) {
    for (int row = 0; row < board->height; row++) {
        for (int col = 0; col < board->width; col++) {
            int index = get_board_index(board, col, row);
            char content = board->board[index].content;
            int is_portal = board->board[index].has_portal;

            if (content != 'W' && !is_portal) {
                *x = col;
                *y = row;
                return;
            }
        }
    }
}

/* Sleep em milissegundos.
 * Usa nanosleep para maior precisão. Não trata interrupções aqui.
 */
void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

// ... (Funções de move_pacman, move_ghost, etc. MANTÉM IGUAIS ATÉ read_file_content) ...
// (Estou a omitir as funções de movimento para poupar espaço, elas não mudaram. 
//  Certifica-te que as manténs no ficheiro!)

int move_pacman(board_t* board, int pacman_index, command_t* command) {
    if (pacman_index < 0) return DEAD_PACMAN;
    pacman_t* pac = &board->pacmans[pacman_index];
    if (!pac->alive) return DEAD_PACMAN;

    int current_x = pac->pos_x;
    int current_y = pac->pos_y;
    int new_x = current_x;
    int new_y = current_y;

    // Sistema de "passo/waiting": se está a aguardar, decrementa e não se move.
    if (pac->waiting > 0) {
        pac->waiting -= 1;
        return VALID_MOVE;        
    }
    pac->waiting = pac->passo;

    char direction = command->command;

    // 'R' significa "random": escolhe uma direcção aleatória entre W,S,A,D.
    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    switch (direction) {
        case 'W': new_y--; break;
        case 'S': new_y++; break;
        case 'A': new_x--; break;
        case 'D': new_x++; break;
        case 'T':
            // T = espera por N turns (comando de turn)
            if (command->turns_left == 1) {
                pac->current_move += 1;
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE;
    }

    pac->current_move += 1;

    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int old_index = get_board_index(board, current_x, current_y);
    int new_index = get_board_index(board, new_x, new_y);

    // Tranca ambas as posições para evitar condições de corrida com fantasmas
    lock_two_positions(board, old_index, new_index);

    // Verifica se fora alterado por outro thread enquanto se preparava
    if (!pac->alive || pac->pos_x != current_x || pac->pos_y != current_y) {
        unlock_two_positions(board, old_index, new_index);
        return DEAD_PACMAN;
    }

    char target_content = board->board[new_index].content;
    int ret_val = VALID_MOVE;

    // Se tiver portal, move para lá e sinaliza REACHED_PORTAL
    if (board->board[new_index].has_portal) {
        board->board[old_index].content = ' ';
        board->board[new_index].content = 'P';
        ret_val = REACHED_PORTAL;
    }
    else if (target_content == 'W') {
        ret_val = INVALID_MOVE;
    }
    else if (target_content == 'M') {
        // Encontrou fantasma -> morte
        kill_pacman(board, pacman_index);
        ret_val = DEAD_PACMAN;
    }
    else {
        // Comida: incrementa pontos e retira ponto do tabuleiro
        if (board->board[new_index].has_dot) {
            pac->points++;
            board->board[new_index].has_dot = 0;
        }

        board->board[old_index].content = ' ';
        pac->pos_x = new_x;
        pac->pos_y = new_y;
        board->board[new_index].content = 'P';
    }

    unlock_two_positions(board, old_index, new_index);

    return ret_val;
}

/* Calcula destino de um ghost em modo "charged": o fantasma percorre a
 * linha/coluna até encontrar parede/monstro, ou até encontrar um pacman.
 * *dest_x,*dest_y retornam a posição final.
 * Retorna 1 se encontrou um pacman no percurso (posição de morte), senão 0.
 * Note que se já estiver na extremidade retorna 0 e não se move.
 */
static int get_charged_dest(board_t* board, int x, int y, char direction, int* dest_x, int* dest_y) {
    *dest_x = x;
    *dest_y = y;
    
    switch (direction) {
        case 'W':
            if (y == 0) return 0;
            *dest_y = 0; 
            for (int i = y - 1; i >= 0; i--) {
                char c = board->board[get_board_index(board, x, i)].content;
                if (c == 'W' || c == 'M') { *dest_y = i + 1; return 0; }
                if (c == 'P') { *dest_y = i; return 1; }
            }
            break;
        case 'S':
            if (y == board->height - 1) return 0;
            *dest_y = board->height - 1;
            for (int i = y + 1; i < board->height; i++) {
                char c = board->board[get_board_index(board, x, i)].content;
                if (c == 'W' || c == 'M') { *dest_y = i - 1; return 0; }
                if (c == 'P') { *dest_y = i; return 1; }
            }
            break;
        case 'A':
            if (x == 0) return 0;
            *dest_x = 0;
            for (int j = x - 1; j >= 0; j--) {
                char c = board->board[get_board_index(board, j, y)].content;
                if (c == 'W' || c == 'M') { *dest_x = j + 1; return 0; }
                if (c == 'P') { *dest_x = j; return 1; }
            }
            break;
        case 'D':
            if (x == board->width - 1) return 0;
            *dest_x = board->width - 1;
            for (int j = x + 1; j < board->width; j++) {
                char c = board->board[get_board_index(board, j, y)].content;
                if (c == 'W' || c == 'M') { *dest_x = j - 1; return 0; }
                if (c == 'P') { *dest_x = j; return 1; }
            }
            break;
    }
    return 0;
}   

int move_ghost_charged(board_t* board, int ghost_index, char direction) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int cur_x = ghost->pos_x;
    int cur_y = ghost->pos_y;
    int new_x, new_y;

    // charged é apenas um sinal; ao executar a carga, marcamos como não-charged
    ghost->charged = 0;

    get_charged_dest(board, cur_x, cur_y, direction, &new_x, &new_y);

    if (cur_x == new_x && cur_y == new_y) {
        debug("DEFAULT CHARGED MOVE BLOCKED - direction = %c\n", direction);
        return INVALID_MOVE;
    }

    int old_index = get_board_index(board, cur_x, cur_y);
    int new_index = get_board_index(board, new_x, new_y);

    // Tranca origem e destino antes de mutar o tabuleiro
    lock_two_positions(board, old_index, new_index);

    if (ghost->pos_x != cur_x || ghost->pos_y != cur_y) {
         unlock_two_positions(board, old_index, new_index);
         return INVALID_MOVE;
    }

    char target_content = board->board[new_index].content;
    int result = VALID_MOVE;

    if (target_content == 'P') {
        // mata pacman na posição final (se houver)
        result = find_and_kill_pacman(board, new_x, new_y);
    } 
    else if (target_content == 'W' || target_content == 'M') {
        // Se destino inválido (parede/outro monstro) aborta
        unlock_two_positions(board, old_index, new_index);
        return INVALID_MOVE;
    }

    // Move fantasma: limpa origem e escreve 'M' no destino
    board->board[old_index].content = ' '; 
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    board->board[new_index].content = 'M';

    unlock_two_positions(board, old_index, new_index);
    return result;
}

int move_ghost(board_t* board, int ghost_index, command_t* command) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int current_x = ghost->pos_x;
    int current_y = ghost->pos_y;
    int new_x = current_x;
    int new_y = current_y;

    if (ghost->waiting > 0) {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;
    
    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    switch (direction) {
        case 'W': new_y--; break;
        case 'S': new_y++; break;
        case 'A': new_x--; break;
        case 'D': new_x++; break;
        case 'C':
            // C = próximos comandos tornam-se "charged": não se move agora
            ghost->current_move += 1;
            ghost->charged = 1;
            return VALID_MOVE;
        case 'T':
            // Turns como no pacman
            if (command->turns_left == 1) {
                ghost->current_move += 1; 
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE;
    }

    ghost->current_move++;
    
    if (ghost->charged)
        return move_ghost_charged(board, ghost_index, direction);

    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int old_index = get_board_index(board, current_x, current_y);
    int new_index = get_board_index(board, new_x, new_y);

    // Tranca origem e destino
    lock_two_positions(board, old_index, new_index);

    if (ghost->pos_x != current_x || ghost->pos_y != current_y) {
        unlock_two_positions(board, old_index, new_index);
        return INVALID_MOVE;
    }

    char target_content = board->board[new_index].content;

    if (target_content == 'W' || target_content == 'M') {
        unlock_two_positions(board, old_index, new_index);
        return INVALID_MOVE;
    }

    int result = VALID_MOVE;
    if (target_content == 'P') {
        // mata pacman se o encontrar
        result = find_and_kill_pacman(board, new_x, new_y);
    }

    board->board[old_index].content = ' '; 
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    board->board[new_index].content = 'M';

    unlock_two_positions(board, old_index, new_index);
    return result;
}

/* kill_pacman: marca pacman como morto e limpa o tabuleiro.
 * Notar: não faz locking; quem chama deve garantir coerência (ex: locks exteriores).
 */
void kill_pacman(board_t* board, int pacman_index) {
    debug("Killing %d pacman\n\n", pacman_index);
    pacman_t* pac = &board->pacmans[pacman_index];
    int index = pac->pos_y * board->width + pac->pos_x;

    board->board[index].content = ' ';
    pac->alive = 0;
}

/* load_pacman: inicializa um pacman simples (usado em fallback).
 * Define pos = -1 para indicar que a posição ainda não foi especificada.
 */
int load_pacman(board_t* board, int points) {
    board->pacmans[0].pos_x = -1;
    board->pacmans[0].pos_y = -1;
    board->pacmans[0].alive = 1;
    board->pacmans[0].points = points;
    return 0;
}

// ... (load_ghost, load_level mantêm-se iguais) ...
int load_ghost(board_t* board) {
    board->board[3 * board->width + 1].content = 'M'; 
    board->ghosts[0].pos_x = 1;
    board->ghosts[0].pos_y = 3;
    board->ghosts[0].passo = 0;
    board->ghosts[0].waiting = 0;
    board->ghosts[0].current_move = 0;
    board->ghosts[0].n_moves = 16;
    for (int i = 0; i < 8; i++) {
        board->ghosts[0].moves[i].command = 'D';
        board->ghosts[0].moves[i].turns = 1; 
    }
    for (int i = 8; i < 16; i++) {
        board->ghosts[0].moves[i].command = 'A';
        board->ghosts[0].moves[i].turns = 1; 
    }

    board->board[2 * board->width + 4].content = 'M';
    board->ghosts[1].pos_x = 4;
    board->ghosts[1].pos_y = 2;
    board->ghosts[1].passo = 1;
    board->ghosts[1].waiting = 1;
    board->ghosts[1].current_move = 0;
    board->ghosts[1].n_moves = 1;
    board->ghosts[1].moves[0].command = 'R';
    board->ghosts[1].moves[0].turns = 1; 
    
    return 0;
}

int load_level(board_t *board, int points) {
    board->height = 5;
    board->width = 10;
    board->tempo = 10;

    board->n_ghosts = 2;
    board->n_pacmans = 1;

    // Aloca o tabuleiro e inicializa mutexes por posição
    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    
    for(int i = 0; i < board->width * board->height; i++) {
        pthread_mutex_init(&board->board[i].mutex, NULL);
    }

    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));

    sprintf(board->level_name, "Static Level");

    for (int i = 0; i < board->height; i++) {
        for (int j = 0; j < board->width; j++) {
            if (i == 0 || j == 0 || j == (board->width - 1)) {
                board->board[i * board->width + j].content = 'W';
            }
            else if (i == 4 && j == 8) {
                board->board[i * board->width + j].content = ' ';
                board->board[i * board->width + j].has_portal = 1;
            }
            else {
                board->board[i * board->width + j].content = ' ';
                board->board[i * board->width + j].has_dot = 1;
            }
        }
    }

    load_ghost(board);
    load_pacman(board, points);

    return 0;
}

// --- LEITURA DE FICHEIROS ROBUSTA ---
/* read_file_content: lê todo o ficheiro para um buffer alocado.
 * Usa open/read (não fgets) e trata EINTR no read. A implementação actual
 * assume um tamanho máximo inicial (4096) e não expande o buffer; se os ficheiros
 * forem maiores, a leitura será truncada. Retorna buffer terminado por '\0'.
 */
char* read_file_content(const char* filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return NULL; 
    
    int capacity = 4096;
    char *buffer = calloc(capacity, sizeof(char)); 
    if (!buffer) { close(fd); return NULL; }

    int total_lido = 0;
    int n;

    while (total_lido < capacity - 1) {
        n = read(fd, buffer + total_lido, capacity - 1 - total_lido);
        if (n == -1) {
            if (errno == EINTR) continue;
            free(buffer); close(fd); return NULL;
        }
        if (n == 0) break;
        total_lido += n;
    }
    buffer[total_lido] = '\0';
    close(fd); 
    return buffer;
}

/* is_valid_pos: verifica se uma posição é utilizável para colocar uma entidade.
 * Garante que não colocamos sobre um fantasma existente ('M') ou pacman ('P').
 */
int is_valid_pos(board_t *board, int x, int y) {
    if (!board || !board->board) return 0;
    if (!is_valid_position(board, x, y)) return 0;
    int idx = y * board->width + x;
    char pos = board->board[idx].content;
    if (pos == 'M' || pos == 'P') return 0;
    return 1;
}

/* parse_move_line: interpreta uma linha de movimento (ex: "W" ou "T3") e
 * adiciona ao array de comandos. Retorna 1 se adicionou com sucesso.
 */
int parse_move_line(char *linha, command_t *moves_array, int *n_moves) {
    if (*n_moves >= MAX_MOVES) return 0;

    char cmd = '\0';
    int turns = 1;

    if (linha[0] == 'T') {
        cmd = 'T';
        if (sscanf(linha + 1, "%d", &turns) != 1) {
            turns = 1;
        }
    } 
    else {
        cmd = linha[0];
    }

    if (cmd != '\0') {
        moves_array[*n_moves].command = cmd;
        moves_array[*n_moves].turns = turns;
        moves_array[*n_moves].turns_left = turns;
        (*n_moves)++;
        return 1;
    }
    return 0;
}

/*
 * load_entity_file: lê ficheiro de entidade (pacman/ghost), interpreta linhas
 * PASSO, POS e movimentos. Se POS não estiver presente, deixa pos_x/pos_y = -1
 * e mais tarde é usado o fallback find_first_free_pos.
 */
int load_entity_file(board_t *board, const char* filename, int index, int is_pacman, int points) {
    char *buffer = read_file_content(filename);

    if (!buffer) {
        if (is_pacman) {
            load_pacman(board, points);
            board->pacmans[index].n_moves = 0; 
        }
        return 0;
    }

    int *e_passo, *e_pos_x, *e_pos_y, *e_n_moves, *e_waiting;
    command_t *e_moves;

    if (is_pacman) {
        pacman_t *p = &board->pacmans[index];
        p->alive = 1; 
        p->points = points;
        p->current_move = 0;
        e_passo = &p->passo;
        e_waiting = &p->waiting;
        e_pos_x = &p->pos_x;
        e_pos_y = &p->pos_y;
        e_n_moves = &p->n_moves;
        e_moves = p->moves;
    } else {
        ghost_t *g = &board->ghosts[index];
        g->charged = 0;
        g->current_move = 0;
        e_passo = &g->passo;
        e_waiting = &g->waiting;
        e_pos_x = &g->pos_x;
        e_pos_y = &g->pos_y;
        e_n_moves = &g->n_moves;
        e_moves = g->moves;
    }
    
    *e_n_moves = 0;
    *e_waiting = 0;
    *e_passo = 0;
    *e_pos_x = -1; // Flag para detetar falta de POS
    *e_pos_y = -1;

    char *saveptr; 
    char *linha = strtok_r(buffer, "\n", &saveptr);

    while(linha != NULL){
        if(linha[0] != '#'){
            if(strncmp(linha, "PASSO", 5) == 0){
                int passo_val;
                if(sscanf(linha, "PASSO %d", &passo_val) == 1){
                    *e_passo = passo_val;
                    *e_waiting = passo_val;
                }
            }
            else if (strncmp(linha, "POS", 3) == 0){
                int l, c;
                if(sscanf(linha, "POS %d %d", &l, &c) == 2){
                    *e_pos_y = l;
                    *e_pos_x = c;
                    if(is_valid_pos(board, c, l)){
                        int idx = l * board->width + c;
                        if (is_pacman) {
                            board->board[idx].content = 'P';
                            board->board[idx].has_dot = 0;
                        } else {
                            board->board[idx].content = 'M';
                        }
                    }
                }
            }
            else {
                parse_move_line(linha, e_moves, e_n_moves);
            }
        }
        linha = strtok_r(NULL, "\n", &saveptr);
    }
    
    free(buffer);

    // Fallback se POS não foi definido: procura a primeira posição livre
    if (*e_pos_x == -1 || *e_pos_y == -1) {
        find_first_free_pos(board, e_pos_x, e_pos_y);
        if (is_valid_position(board, *e_pos_x, *e_pos_y)) {
            int idx = *e_pos_y * board->width + *e_pos_x;
            if (is_pacman) {
                board->board[idx].content = 'P';
                board->board[idx].has_dot = 0;
            } else {
                board->board[idx].content = 'M';
            }
        }
    }

    return 0;
}

/* process_entities: interpreta linha "PAC a.txt b.txt ..." ou "MON ..." e
 * aloca arrays de pacmans/ghosts e carrega cada ficheiro de entidade.
 * Usa sscanf para contar nomes e depois percorre novamente para carregar.
 */
void process_entities(board_t *board, char *linha, int tipo, int points) {
    char temp_name[50];
    int offset = 0;
    int count = 0;
    char *cursor = linha + 3; 

    while (sscanf(cursor, "%s%n", temp_name, &offset) == 1) {
        count++;
        cursor += offset;
    }
    if (tipo == 0) {
        board->n_pacmans = count;
        board->pacmans = calloc(count, sizeof(pacman_t));
    } else {
        board->n_ghosts = count;
        board->ghosts = calloc(count, sizeof(ghost_t));
    }

    cursor = linha + 3;
    int i = 0;
    while (sscanf(cursor, "%s%n", temp_name, &offset) == 1) {
        if (tipo == 0) load_entity_file(board, temp_name, i, 1, points); 
        else load_entity_file(board, temp_name, i, 0, 0);
        cursor += offset;
        i++;
    }
}

/* load_level_filename: parser principal do ficheiro de nível.
 * Lê DIM/TEMPO, linhas do mapa, e PAC/MON linhas com nomes de ficheiros.
 * Para as linhas de mapa, X = parede, '@' = portal, o resto pode ter pontos.
 */
int load_level_filename(board_t *board, const char *filename, int points) {
    char *buffer = read_file_content(filename);
    if (!buffer) return 1;

    char *saveptr;
    char *linha = strtok_r(buffer, "\n", &saveptr);
    int current_row = 0; 

    while (linha != NULL) {
        if (linha[0] != '#') {
            if (strncmp(linha, "DIM", 3) == 0) {
                int h, w;
                if (sscanf(linha, "DIM %d %d", &h, &w) == 2) {
                    board->height = h;
                    board->width = w;
                    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
                    
                    for(int i = 0; i < board->width * board->height; i++) {
                        pthread_mutex_init(&board->board[i].mutex, NULL);
                    }
                }
            } 
            else if (strncmp(linha, "TEMPO", 5) == 0) {
                int t;
                if (sscanf(linha, "TEMPO %d", &t) == 1) board->tempo = t; 
            }
            else if(strncmp(linha, "PAC", 3) == 0) process_entities(board, linha, 0, points);
            else if (strncmp(linha, "MON", 3) == 0) process_entities(board, linha, 1, 0);
            else {
                if (board->board != NULL && current_row < board->height) {
                    for (int x = 0; x < board->width && linha[x] != '\0'; x++) {
                        int index = (current_row * board->width) + x;
                        char char_lido = linha[x];
                        char conteudo_atual = board->board[index].content;

                        if (char_lido == 'X') {
                            board->board[index].content = 'W'; 
                        } 
                        else if (char_lido == '@') {
                            if (conteudo_atual != 'P' && conteudo_atual != 'M')
                                board->board[index].content = ' ';
                            board->board[index].has_portal = 1;
                        } 
                        else {
                            if (conteudo_atual != 'P' && conteudo_atual != 'M') {
                                board->board[index].content = ' '; 
                            }
                            board->board[index].has_dot = 1;
                        }
                    }
                    current_row++; 
                }
            }
        }
        linha = strtok_r(NULL, "\n", &saveptr);
    }
    
    // Se não havia linha PAC, cria um pacman por omissão
    if (board->pacmans == NULL) {
        board->n_pacmans = 1;
        board->pacmans = calloc(1, sizeof(pacman_t));
        load_pacman(board, points); 
    }

    pacman_t *pac = board->pacmans;
    if (pac->pos_x == -1 && pac->pos_y == -1) { 
        // Se posição não especificada, encontra primeira livre e coloca pacman
        find_first_free_pos(board, &pac->pos_x, &pac->pos_y);
        int idx = pac->pos_y * board->width + pac->pos_x;
        if (is_valid_position(board, pac->pos_x, pac->pos_y)) {
            board->board[idx].content = 'P';
            board->board[idx].has_dot = 0; 
        }
    }

    free(buffer);
    return 0;
}

// ... (unload_level, debug, print_board MANTÉM IGUAIS) ...
/* unload_level: destrói mutexes, libera memória alocada. 
 * Deve ser chamado ao terminar com o nível.
 */
void unload_level(board_t * board) {
    if (board->board) {
        for(int i = 0; i < board->width * board->height; i++) {
            pthread_mutex_destroy(&board->board[i].mutex);
        }
        free(board->board);
    }
    free(board->pacmans);
    free(board->ghosts);
}

/* Debug file helpers: abrir/fechar ficheiro de logs. Assumem que debugfile é válido. */
void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

/* debug: escreve format para debugfile (variadic). Flush imediata para ver logs em tempo real. */
void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);
    fflush(debugfile);
}

/* print_board: formata estado do tabuleiro num buffer e escreve para debug.
 * Mostra info do nível, ficheiros de monstros e o mapa em linhas.
 */
void print_board(board_t *board) {
    if (!board || !board->board) {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    char buffer[8192];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== [%d] LEVEL INFO ===\n"
                       "Dimensions: %d x %d\n"
                       "Tempo: %d\n"
                       "Pacman file: %s\n",
                       getpid(), board->height, board->width, board->tempo, board->pacman_file);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "Monster files (%d):\n", board->n_ghosts);

    for (int i = 0; i < board->n_ghosts; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  - %s\n", board->ghosts_files[i]);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== BOARD ===\n");

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            if (offset < sizeof(buffer) - 2) {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2) {
            buffer[offset++] = '\n';
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "==================\n");
    buffer[offset] = '\0';
    debug("%s", buffer);
}