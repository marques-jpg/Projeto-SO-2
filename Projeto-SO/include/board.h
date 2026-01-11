#ifndef BOARD_H
#define BOARD_H

#define MAX_MOVES 20
#define MAX_LEVELS 20
#define MAX_FILENAME 256
#define MAX_GHOSTS 25

#include <pthread.h>

typedef enum {
    REACHED_PORTAL = 1, // Pacman reached the portal
    VALID_MOVE = 0,     // Move was successful
    INVALID_MOVE = -1,  // Move hit a wall or was out of bounds
    DEAD_PACMAN = -2,   // Pacman died
} move_t;

typedef struct {
    char command;   // Character representing the direction/action
    int turns;      // Total turns this command lasts
    int turns_left; // Turns remaining for this command
} command_t;

typedef struct {
    int pos_x, pos_y;            // current position
    int alive;                   // if is alive
    int points;                  // how many points have been collected
    int passo;                   // number of plays to wait before starting
    command_t moves[MAX_MOVES];  // array of moves loaded from file
    int current_move;            // Index of the current move in the moves array
    int n_moves;                 // number of predefined moves, 0 if controlled by user, >0 if readed from level file
    int waiting;                 // Turns left to wait before moving again
} pacman_t;

typedef struct {
    int pos_x, pos_y;           // current position
    int passo;                  // number of plays to wait between each move
    command_t moves[MAX_MOVES]; // array of moves loaded from file
    int n_moves;                // number of predefined moves from level file
    int current_move;           // Index of the current move in the moves array
    int waiting;                // Turns left to wait before moving again
    int charged;                // Flag indicating if the ghost is in 'charge' mode
} ghost_t;

typedef struct {
    char content;          // stuff like 'P' for pacman 'M' for monster/ghost and 'W' for wall
    int has_dot;           // whether there is a dot in this position or not
    int has_portal;        // whether there is a portal in this position or not
    pthread_mutex_t mutex; // mutex to lock this specific cell
} board_pos_t;

typedef struct {
    int width, height;                  // dimensions of the board
    board_pos_t* board;                 // actual board, a row-major matrix
    int n_pacmans;                      // number of pacmans in the board
    pacman_t* pacmans;                  // array containing every pacman in the board to iterate through when processing (Just 1)
    int n_ghosts;                       // number of ghosts in the board
    ghost_t* ghosts;                    // array containing every ghost in the board to iterate through when processing
    char level_name[256];               // name for the level file to keep track of which will be the next
    char pacman_file[256];              // file with pacman movements
    char ghosts_files[MAX_GHOSTS][256]; // files with monster movements
    int tempo;                          // Duration of each play
    int save_active;                    // Flag indicating if a save game is active/requested
} board_t;

// Shared state structure to synchronize threads
typedef struct {
    board_t *board;             // Pointer to the game board data
    pthread_mutex_t mutex;      // Mutex for synchronizing access to the state
    pthread_cond_t input_cond;  // Condition variable for input events
    int running;                // Flag indicating if the game loop is running
    int outcome;                // Result of the game (continue, next level, quit)
    char pending_input;         // Input character waiting to be processed
    int save_request;           // Flag indicating a request to save the game
} game_state_t;

// Arguments passed to each ghost thread
typedef struct {
    game_state_t *state; // Pointer to the shared game state
    int ghost_index;     // Index of this ghost in the board's ghost array
} ghost_thread_args_t;


/*Makes the current thread sleep for 'int milliseconds' miliseconds*/
void sleep_ms(int milliseconds);

/*Processes a command for Pacman or Ghost(Monster)
*_index - corresponding index in board's pacman_t/ghost_t array
command - command to be processed*/
int move_pacman(board_t* board, int pacman_index, command_t* command);
int move_ghost(board_t* board, int ghost_index, command_t* command);

/*Process the death of a Pacman*/
void kill_pacman(board_t* board, int pacman_index);

/*Adds a pacman to the board*/
int load_pacman(board_t* board, int points);

/*Adds a ghost(monster) to the board*/
int load_ghost(board_t* board);

/*Loads a level into board (Old static version)*/
int load_level(board_t* board, int accumulated_points);

/*Loads a level from a file (New dynamic version)*/
int load_level_filename(board_t *board, const char *filename, int accumulated_points);

/*Unloads levels loaded by load_level*/
void unload_level(board_t * board);

/*Reads the full content of a file into a buffer*/
char* read_file_content(const char* filename);

// DEBUG FILE

/*Opens the debug file*/
void open_debug_file(char *filename);

/*Closes the debug file*/
void close_debug_file();

/*Writes to the open debug file*/
void debug(const char * format, ...);

/*Writes the board and its contents to the open debug file*/
void print_board(board_t* board);

/*Checks if coordinates are within bounds and valid for placing an entity*/
int is_valid_pos(board_t *board, int x, int y);

/*Parses a move string from a file into a command struct*/
int parse_move_line(char *linha, command_t *moves_array, int *n_moves);

/*Loads entity data (pacman/ghost) from a specific file*/
int load_entity_file(board_t *board, const char* filename, int index, int is_pacman, int points);

/*Processes the entity list string from the level file*/
void processar_entidades(board_t *board, char *linha, int tipo, int points);

#endif