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

struct Session {
    int id;
    int req_pipe;      
    int notif_pipe;    
    char req_pipe_path[40];
    char notif_pipe_path[40];
};

static struct Session session = { .id = -1 };

// Leitura segura que garante N bytes
static int read_exact(int fd, void *buffer, size_t count) {
    size_t total_read = 0;
    char *buf_ptr = (char *)buffer;

    while (total_read < count) {
        ssize_t r = read(fd, buf_ptr + total_read, count - total_read);
        if (r == -1) {
            if (errno == EINTR) continue; 
            return -1; 
        }
        if (r == 0) return -1; // EOF prematuro
        total_read += r;
    }
    return 0; 
}

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
    if (mkfifo(req_pipe_path, 0666) == -1 && errno != EEXIST) return 1;
    if (mkfifo(notif_pipe_path, 0666) == -1 && errno != EEXIST) return 1;

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

    session.notif_pipe = open(notif_pipe_path, O_RDONLY);
    if (session.notif_pipe == -1) return 1;

    session.req_pipe = open(req_pipe_path, O_WRONLY);
    if (session.req_pipe == -1) {
        close(session.notif_pipe);
        return 1;
    }

    char res_op, res_code;
    if (read_exact(session.notif_pipe, &res_op, 1) != 0 ||
        read_exact(session.notif_pipe, &res_code, 1) != 0) {
        close(session.notif_pipe);
        close(session.req_pipe);
        return 1;
    }

    if (res_op != OP_CODE_CONNECT || res_code != 0) {
        close(session.notif_pipe);
        close(session.req_pipe);
        return 1; 
    }

    strncpy(session.req_pipe_path, req_pipe_path, 40);
    strncpy(session.notif_pipe_path, notif_pipe_path, 40);
    session.id = 1; 

    return 0;
}

int pacman_play(char command) {
    if (session.id == -1) return -1;
    char op = OP_CODE_PLAY;
    if (write(session.req_pipe, &op, 1) == -1) return -1;
    if (write(session.req_pipe, &command, 1) == -1) return -1;
    return 0;
}

Board receive_board_update(void) {
    Board board = {0};
    char op;

    int n = read(session.notif_pipe, &op, 1);
    if (n <= 0) {
        board.data = NULL;
        return board; 
    }

    if (op != OP_CODE_BOARD) return board;

    if (read_exact(session.notif_pipe, &board.width, sizeof(int)) != 0 ||
        read_exact(session.notif_pipe, &board.height, sizeof(int)) != 0 ||
        read_exact(session.notif_pipe, &board.tempo, sizeof(int)) != 0 ||
        read_exact(session.notif_pipe, &board.victory, sizeof(int)) != 0 ||
        read_exact(session.notif_pipe, &board.game_over, sizeof(int)) != 0 ||
        read_exact(session.notif_pipe, &board.accumulated_points, sizeof(int)) != 0) {
        board.data = NULL;
        return board;
    }

    int size = board.width * board.height;
    if (size > 0) {
        board.data = malloc(size);
        if (read_exact(session.notif_pipe, board.data, size) != 0) {
            free(board.data);
            board.data = NULL;
        }
    } else {
        board.data = NULL;
    }

    return board;
}

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