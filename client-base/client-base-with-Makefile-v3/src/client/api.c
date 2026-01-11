#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>

// Estrutura para guardar os dados da sessão atual
struct Session {
    int id;
    int req_pipe;      // File descriptor para enviar pedidos (teclas)
    int notif_pipe;    // File descriptor para receber o tabuleiro
    char req_pipe_path[40];
    char notif_pipe_path[40];
};

static struct Session session = { .id = -1 };

// 1. Ligar ao Servidor
int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
    // A. Criar os pipes do cliente (mkfifo)
    if (mkfifo(req_pipe_path, 0666) == -1 && errno != EEXIST) return 1;
    if (mkfifo(notif_pipe_path, 0666) == -1 && errno != EEXIST) return 1;

    // B. Enviar pedido de conexão ao servidor
    int server_fd = open(server_pipe_path, O_WRONLY);
    if (server_fd == -1) return 1; 

    char op = OP_CODE_CONNECT;
    char req_buf[40] = {0};   
    char notif_buf[40] = {0};

    strncpy(req_buf, req_pipe_path, 40);
    strncpy(notif_buf, notif_pipe_path, 40);

    write(server_fd, &op, 1);
    write(server_fd, req_buf, 40);
    write(server_fd, notif_buf, 40);
    close(server_fd);

    // C. Abrir AMBOS os pipes (Isto evita o Deadlock!)
    // 1. Abrir Notificações (Leitura) - Bloqueia até servidor abrir escrita
    session.notif_pipe = open(notif_pipe_path, O_RDONLY);
    if (session.notif_pipe == -1) return 1;

    // 2. Abrir Pedidos (Escrita) - Desbloqueia o servidor que está à espera disto
    session.req_pipe = open(req_pipe_path, O_WRONLY);
    if (session.req_pipe == -1) {
        close(session.notif_pipe);
        return 1;
    }

    // D. AGORA sim, lemos a resposta do servidor
    char res_op, res_code;
    read(session.notif_pipe, &res_op, 1);
    read(session.notif_pipe, &res_code, 1);

    if (res_op != OP_CODE_CONNECT || res_code != 0) {
        close(session.notif_pipe);
        close(session.req_pipe);
        return 1; // Falha na conexão ou recusado
    }

    // Guardar caminhos para limpar no fim
    strncpy(session.req_pipe_path, req_pipe_path, 40);
    strncpy(session.notif_pipe_path, notif_pipe_path, 40);
    session.id = 1; 

    return 0;
}

// 2. Enviar Tecla (Jogada)
void pacman_play(char command) {
    if (session.id == -1) return;
    char op = OP_CODE_PLAY;
    // Ignorar sinais se o servidor fechar o pipe (evita crash com SIGPIPE)
    // Se write falhar, não faz mal, o jogo provavelmente acabou
    write(session.req_pipe, &op, 1);
    write(session.req_pipe, &command, 1);
}

// 3. Receber Atualização do Tabuleiro
Board receive_board_update(void) {
    Board board = {0};
    char op;

    // Tenta ler o código da operação
    int n = read(session.notif_pipe, &op, 1);
    
    if (n <= 0) {
        // Pipe fechado ou erro -> Jogo acabou
        board.data = NULL;
        return board; 
    }

    if (op != OP_CODE_BOARD) return board;

    // Lê os metadados (inteiros)
    read(session.notif_pipe, &board.width, sizeof(int));
    read(session.notif_pipe, &board.height, sizeof(int));
    read(session.notif_pipe, &board.tempo, sizeof(int));
    read(session.notif_pipe, &board.victory, sizeof(int));
    read(session.notif_pipe, &board.game_over, sizeof(int));
    read(session.notif_pipe, &board.accumulated_points, sizeof(int));

    // Lê a matriz do mapa
    int size = board.width * board.height;
    if (size > 0) {
        board.data = malloc(size);
        // Garantir que lemos tudo (loop simples de leitura)
        int total_read = 0;
        while (total_read < size) {
            int r = read(session.notif_pipe, board.data + total_read, size - total_read);
            if (r <= 0) break;
            total_read += r;
        }
    } else {
        board.data = NULL;
    }

    return board;
}

// 4. Desligar
int pacman_disconnect() {
    if (session.id == -1) return 0;
    
    char op = OP_CODE_DISCONNECT;
    write(session.req_pipe, &op, 1);

    close(session.req_pipe);
    close(session.notif_pipe);
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);
    session.id = -1;
    return 0;
}