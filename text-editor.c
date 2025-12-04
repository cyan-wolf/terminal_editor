#include <asm-generic/errno-base.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>

/*
 * Defines.
 */

// Maps ASCII letters to their control character counterpart.
// i.e. This maps 'a' (97) to 1 and 'z' (122) to 26.
#define CTRL_KEY(k) ((k) & 0x1f)

/*
 * Data.
 */

// Caches the original terminal attributes for later cleanup.
struct termios og_termios;

/*
 * Terminal handling.
 */

void clearTermScreen() {
    // Clears the screen by writing the escape sequence: '\x1b', '[', '2', 'J' to STDOUT.
    write(STDOUT_FILENO, "\x1b[2J", 4);
}

void resetTermCursor() {
    // Position the cursor at the top-left of the terminal.
    write(STDOUT_FILENO, "\x1b[H", 3);
}

// Crash the program with an error message.
void die(const char *message) {
    clearTermScreen();
    resetTermCursor();

    perror(message);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &og_termios) == -1) {
        die("tcsetattr");
    }
}

void enableTermRawMode() {
    // Get current terminal attribututes.
    if (tcgetattr(STDIN_FILENO, &og_termios) == -1) {
        die("tcsetattr");
    }
    
    // Set a callback for disabling raw mode (cleeanup).
    atexit(disableRawMode);

    // Copy the current terminal attributes for futher modification.
    struct termios raw = og_termios;

    // Disable the default CTRL-S and CTRL-Q handling.
    // Disabling `ICRNL` makes CTRL-M be read as 13 (correct) instead of 10 (incorrect). 
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);

    // Disable output processing (i.e. automatic carriage return insertion
    // when the program itself prints a line).
    raw.c_oflag &= ~(OPOST);

    // Enable (if it isn't already) the bit mask for setting the character 
    // size to 8 bits.
    raw.c_cflag |= CS8;

    // Disable the:
    // - echo
    // - canonical (line-by-line) mode.
    // - signal processing (i.e. from CTRL-C or CTRL-Z)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;     // minimum # of bytes before `read()` can return
    raw.c_cc[VTIME] = 1;    // delay (in 10ths of a second) to wait before `read()` returns

    // Set the modified terminal attributes.
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

// Waits for a key press before returning.
char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1) != 1)) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }
    return c;
}

/*
 * Input handling.
 */

// Waits for a key press and processes it. 
void editorProcessKeypress() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            clearTermScreen();
            resetTermCursor();
            exit(0);
            break;
    }
}

/*
 * Output handling.
 */

void editorDrawRows() {
    for (int y = 0; y < 24; y++) {
        write(STDOUT_FILENO, "~\r\n", 3);
    }
}

void editorRefreshScreen() {
    clearTermScreen();
    resetTermCursor();

    editorDrawRows();

    resetTermCursor();
}

/*
 * Program initialization.
 */
int main() {
    enableTermRawMode();

    
    while (true) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
