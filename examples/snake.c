#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>

#define WIDTH 40
#define HEIGHT 20
#define MAX_SNAKE 200

/* ---------- Terminal raw mode ---------- */

struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    printf("\033[?25l"); // hide cursor
}

/* ---------- Game data ---------- */

typedef struct {
    int x, y;
} Point;

Point snake[MAX_SNAKE];
int snake_len = 3;

Point food;
int dx = 1, dy = 0;
int game_over = 0;

/* ---------- Utility ---------- */

int kbhit() {
    char c;
    return read(STDIN_FILENO, &c, 1) == 1 ? c : 0;
}

void clear_screen() {
    printf("\033[H\033[J");
}

void spawn_food() {
    food.x = rand() % (WIDTH - 2) + 1;
    food.y = rand() % (HEIGHT - 2) + 1;
}

/* ---------- Input ---------- */

void handle_input() {
    char c = kbhit();
    if (!c) return;

    if (c == 'q') game_over = 1;

    if (c == '\033') { // arrow keys
        char seq[2];
        read(STDIN_FILENO, &seq[0], 1);
        read(STDIN_FILENO, &seq[1], 1);
        if (seq[1] == 'A' && dy == 0) { dx = 0; dy = -1; }
        if (seq[1] == 'B' && dy == 0) { dx = 0; dy = 1; }
        if (seq[1] == 'C' && dx == 0) { dx = 1; dy = 0; }
        if (seq[1] == 'D' && dx == 0) { dx = -1; dy = 0; }
    }

    if (c == 'w' && dy == 0) { dx = 0; dy = -1; }
    if (c == 's' && dy == 0) { dx = 0; dy = 1; }
    if (c == 'a' && dx == 0) { dx = -1; dy = 0; }
    if (c == 'd' && dx == 0) { dx = 1; dy = 0; }
}

/* ---------- Game logic ---------- */

void update() {
    Point new_head = { snake[0].x + dx, snake[0].y + dy };

    if (new_head.x <= 0 || new_head.x >= WIDTH - 1 ||
        new_head.y <= 0 || new_head.y >= HEIGHT - 1) {
        game_over = 1;
        return;
    }

    for (int i = 0; i < snake_len; i++) {
        if (snake[i].x == new_head.x && snake[i].y == new_head.y) {
            game_over = 1;
            return;
        }
    }

    memmove(&snake[1], &snake[0], sizeof(Point) * (snake_len - 1));
    snake[0] = new_head;

    if (new_head.x == food.x && new_head.y == food.y) {
        if (snake_len < MAX_SNAKE) snake_len++;
        spawn_food();
    }
}

/* ---------- Rendering ---------- */

void draw() {
    clear_screen();

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            if (x == 0 || x == WIDTH - 1 || y == 0 || y == HEIGHT - 1) {
                putchar('#');
            } else if (x == food.x && y == food.y) {
                putchar('*');
            } else {
                int body = 0;
                for (int i = 0; i < snake_len; i++) {
                    if (snake[i].x == x && snake[i].y == y) {
                        putchar(i == 0 ? 'O' : 'o');
                        body = 1;
                        break;
                    }
                }
                if (!body) putchar(' ');
            }
        }
        putchar('\n');
    }

    printf("Score: %d | WASD / Arrows | Q to quit\n", snake_len - 3);
}

/* ---------- Main ---------- */

int main() {
    srand(time(NULL));
    enable_raw_mode();

    snake[0] = (Point){ WIDTH / 2, HEIGHT / 2 };
    snake[1] = (Point){ WIDTH / 2 - 1, HEIGHT / 2 };
    snake[2] = (Point){ WIDTH / 2 - 2, HEIGHT / 2 };

    spawn_food();

    while (!game_over) {
        handle_input();
        update();
        draw();
        usleep(100000); // game speed
    }

    draw();
    printf("Game Over! Final score: %d\n", snake_len - 3);
    return 0;
}

