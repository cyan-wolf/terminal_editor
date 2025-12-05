// #include <ctype.h>
#include <asm-generic/ioctls.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/ioctl.h>

/*
 * Defines.
 */

// Maps ASCII letters to their control character counterpart.
// i.e. This maps 'a' (97) to 1 and 'z' (122) to 26.
#define CTRL_KEY(k) ((k) & 0x1f)

/*
 * Data.
 */

struct EditorConfig {
    int termRows;
    int termCols;

    // Caches the original terminal attributes for later cleanup.
    struct termios og_termios;
};

struct EditorConfig editor;

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
    if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &editor.og_termios) == -1) {
        die("tcsetattr");
    }
}

void enableTermRawMode() {
    // Get current terminal attribututes.
    if (tcgetattr(STDIN_FILENO, &editor.og_termios) == -1) {
        die("tcsetattr");
    }
    
    // Set a callback for disabling raw mode (cleeanup).
    atexit(disableRawMode);

    // Copy the current terminal attributes for futher modification.
    struct termios raw = editor.og_termios;

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

int getCursorPosition(int *rows, int *cols) {
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }
    
    char buf[32];
    unsigned int i = 0;

    // Read the cursor position report into the buffer.
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        i++;
    }
    // Null terminate the buffer so that it can be interpreted as a string.
    buf[i] = '\0';

    // Make sure that the read report is in the correct format.
    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    // Parse the rows and columns from the report.
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }
    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // Fallback logic to get the window size anyways:

        // Try to move the cursor to the bottom right corner of the screen.
        // This is done by moving it by large values (999). This escape code is 
        // guaranteed to not move the cursor offscreen.
        if (write(STDOUT_FILENO, "\x1b[999\x1b[999B", 12) != 12) {
            return -1;
        }
        // Since the cursor at this point is at the bottom right corner, 
        // the cursor's position corresponds to the window size.
        return getCursorPosition(rows, cols);
    }
    else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
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
    for (int y = 0; y < editor.termRows; y++) {
        write(STDOUT_FILENO, "~", 1);

        // Avoid adding an extra new line at the end of the final row.
        if (y < editor.termRows - 1) {
            write(STDOUT_FILENO, "\r\n", 2);
        }
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

void initEditor() {
    if (getWindowSize(&editor.termRows, &editor.termCols) == -1) {
        die("getWindowSize");
    }
}

int main() {
    enableTermRawMode();
    initEditor();

    while (true) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
