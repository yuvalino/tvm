#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

/* ---------- Terminal raw mode ---------- */

struct termios orig_termios;

void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // show cursor
}

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    printf("\033[?25l"); // hide cursor
}

/* ---------- Game state ---------- */

#define WIDTH 40

volatile sig_atomic_t ctrl_c_pressed = 0;

int marker_pos;
int marker_dir;

int target_start;
int target_size;

int round_num = 1;
int streak = 0;

useconds_t speed;

/* ---------- Signal handler ---------- */

void handle_sigint(int signo) {
    (void)signo;
    ctrl_c_pressed = 1;
}

/* ---------- Utility ---------- */

void sleep_us(useconds_t us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

void reset_difficulty(void) {
    speed = 120000;
    target_size = 7;
    streak = 0;
}

void next_round(void) {
    target_start = rand() % (WIDTH - target_size - 2) + 1;
    marker_pos = 1;
    marker_dir = 1;
    ctrl_c_pressed = 0;
}

/* ---------- Rendering ---------- */

void draw(void) {
    printf("\033[H"); // move cursor home without clearing
    putchar('|');
    for (int i = 1; i < WIDTH - 1; i++) {
        if (i >= target_start && i < target_start + target_size)
            putchar('=');
        else if (i == marker_pos)
            putchar('O');
        else
            putchar(' ');
    }
    putchar('|');
    putchar('\n');

    printf(
        "Round: %d  Speed: %dus         \n"
        "Ctrl+C to stop | Q to quit\n",
        round_num, speed
    );
}

/* ---------- Input ---------- */

int check_quit(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 'q' || c == 'Q')
            return 1;
    }
    return 0;
}

/* ---------- Main ---------- */

int main(void) {
    srand(time(NULL));
    enable_raw_mode();

    /* clear the screen once at the start */
    printf("\033[H\033[J");

    /* Install SIGINT handler */
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    reset_difficulty();
    next_round();

    draw();

    for (;;) {
        if (check_quit())
            break;

        marker_pos += marker_dir;
        if (marker_pos <= 1 || marker_pos >= WIDTH - 2)
            marker_dir = -marker_dir;

        if (ctrl_c_pressed) {
            ctrl_c_pressed = 0;
            round_num++;

            if (marker_pos >= target_start &&
                marker_pos < target_start + target_size) {
                /* HIT */
                streak++;
                if (speed > 25000) speed -= 8000;
                if (target_size > 2) target_size--;

                next_round();
            } else {
                /* MISS = GAME OVER, leave board visible */
                printf("MISS! You reached round %d\n", round_num);
                return 0;
            }
        }

        draw();
        sleep_us(speed);
    }

    printf("\nQuit after %d rounds\n", round_num);
    return 0;
}
