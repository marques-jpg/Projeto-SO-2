#include "display.h"
#include "board.h"
#include <stdlib.h>
#include <ctype.h>

/* Inicializa o modo ncurses e configurações do terminal.
    Retorna 0 em sucesso. */
int terminal_init() {
     // Entra no modo ncurses (prepara a interface de texto)
     initscr();

     // Desativa buffering de linha para receber caracteres imediatamente
     cbreak();

     // Não mostra os caracteres digitados na tela
     noecho();

     // Permite capturar teclas especiais (setas, F-keys, etc.)
     keypad(stdscr, TRUE);

     // Faz getch() não-bloqueante: retorna ERR se não houver input.
     // Útil para atualizar o ecrã continuamente sem bloquear a lógica do jogo.
     nodelay(stdscr, TRUE);

     // Esconde o cursor para estética do jogo
     curs_set(0);

     // Configura cores se o terminal suportar
     if (has_colors()) {
          start_color();

          // Pares de cores: (id, foreground, background)
          init_pair(1, COLOR_YELLOW, COLOR_BLACK);  // Pacman
          init_pair(2, COLOR_RED, COLOR_BLACK);     // Ghosts
          init_pair(3, COLOR_BLUE, COLOR_BLACK);    // Walls
          init_pair(4, COLOR_WHITE, COLOR_BLACK);   // Points/dots
          init_pair(5, COLOR_GREEN, COLOR_BLACK);   // UI elements (títulos/score)
          init_pair(6, COLOR_MAGENTA, COLOR_BLACK); // Portais/elementos especiais
          init_pair(7, COLOR_CYAN, COLOR_BLACK);    // Extra (disponível)
     }

     // Limpa e desenha do zero
     clear();

     return 0;
}

/* Desenha todo o tabuleiro + UI.
    - board: estrutura com estado do jogo (mapa, pacmans, ghosts, etc.)
    - mode: indica se mostra menu, game over, vitória, etc.
    Observação: posições de desenho consideram start_row para reservar linhas de UI. */
void draw_board(board_t* board, int mode) {
     // Limpa o ecrã antes de redesenhar (evita "fantasmas" de frames anteriores)
     clear();

     // Cabeçalho / título
     attron(COLOR_PAIR(5));
     mvprintw(0, 0, "=== PACMAN GAME ===");
     switch(mode) {
     case DRAW_GAME_OVER:
          mvprintw(1, 0, " GAME OVER ");
          break;

     case DRAW_WIN:
          mvprintw(1, 0, " VICTORY ");
          break;

     case DRAW_MENU:
          // Mostra o nome do nível e instruções básicas
          mvprintw(1, 0, "Level: %s | Use W/A/S/D to move | Q to quit | G to quicksave ", board->level_name);
          break;
     }

     // Linhas reservadas para UI (título/score) antes do tabuleiro propriamente dito
     int start_row = 3;

     // Percorre cada célula do tabuleiro e desenha o carácter apropriado
     for (int y = 0; y < board->height; y++) {
          for (int x = 0; x < board->width; x++) {
                int index = y * board->width + x;
                char ch = board->board[index].content;

                // Verifica se alguma ghost está nesta posição e se está "charged"
                int ghost_charged = 0;
                for (int g = 0; g < board->n_ghosts; g++) {
                     ghost_t* ghost = &board->ghosts[g];
                     if (ghost->pos_x == x && ghost->pos_y == y) {
                          if (ghost->charged)
                                ghost_charged = 1;
                          break;
                     }
                }

                // Move o cursor para a célula correta (considerando start_row)
                move(start_row + y, x);

                // Desenha o símbolo com cores/atributos consoante o tipo de célula
                switch (ch) {
                     case 'W': // Wall (parede)
                          attron(COLOR_PAIR(3));
                          addch('#');
                          attroff(COLOR_PAIR(3));
                          break;

                     case 'P': // Pacman
                          // Usa cor do pacman e negrito para destaque
                          attron(COLOR_PAIR(1) | A_BOLD);
                          addch('C');
                          attroff(COLOR_PAIR(1) | A_BOLD);
                          break;

                     case 'M': // Ghost/Monstro
                          // Se ghost estiver "charged" pode usar um atributo diferente.
                          // A combinação de atributos tem de ser ativada/desativada cuidadosamente.
                          attron((COLOR_PAIR(2) | A_BOLD) | ((ghost_charged) ? (A_DIM) : (0)));
                          addch('M');
                          attroff((COLOR_PAIR(2) | A_BOLD) | ((ghost_charged) ? (A_DIM) : (0)));
                          break;

                     case ' ': // Espaço vazio (pode conter portal ou ponto)
                          if (board->board[index].has_portal) {
                                // Portal representado por '@' com cor própria
                                attron(COLOR_PAIR(6));
                                addch('@');
                                attroff(COLOR_PAIR(6));
                          }
                          else if (board->board[index].has_dot) {
                                // Pontos/dots que o pacman coleciona
                                attron(COLOR_PAIR(4));
                                addch('.');
                                attroff(COLOR_PAIR(4));
                          }
                          else
                                addch(' '); // Espaço em branco padrão
                          break;

                     default:
                          // Caso genérico: desenha o carácter contido na célula
                          addch(ch);
                          break;
                }
          }
     }

     // Desenha o score/status abaixo do tabuleiro.
     // Assumimos pacman principal em pacmans[0].
     attron(COLOR_PAIR(5));
     mvprintw(start_row + board->height + 1, 0, "Points: %d",
                 board->pacmans[0].points);
     attroff(COLOR_PAIR(5));
}

/* Função auxiliar para desenhar um carácter numa posição específica com cor.
    - c: carácter a desenhar
    - colour_i: índice do par de cores (init_pair)
    - pos_x/pos_y: coordenadas no ecrã */
void draw(char c, int colour_i, int pos_x, int pos_y) {
     move(pos_y, pos_x);
     attron(COLOR_PAIR(colour_i) | A_BOLD);
     addch(c);
     attroff(COLOR_PAIR(colour_i) | A_BOLD);
}

/* Refresca o ecrã físico com o buffer virtual (ncurses double-buffer). */
void refresh_screen() {
     refresh();
}

/* Lê uma tecla do utilizador de forma não-bloqueante.
    Retorna '\0' se não houver input ou se tecla não reconhecida.
    Converte para maiúsculas para simplificar o tratamento. */
char get_input() {
     int ch = getch();

     // getch() devolve ERR quando não há input (devido a nodelay)
     if (ch == ERR) {
          return '\0'; // Sem input
     }

     // Normaliza para maiúscula para tratar 'w' e 'W' como igual
     ch = toupper((char)ch);

     // Aceita apenas as teclas relevantes para o jogo; outras são ignoradas
     switch ((char)ch) {
          case 'W':
          case 'S':
          case 'A':
          case 'D':
          case 'Q':
          case 'G':
                return (char)ch;
          
          default:
                return '\0';
     }
}

/* Restaura as configurações do terminal e sai do modo ncurses.
    Deve ser chamada ao terminar o jogo para devolver o terminal ao estado normal. */
void terminal_cleanup() {
     endwin();
}