// For 'enabling' POSIX extensions like `getline`.
#define _GNU_SOURCE

#include <asm-generic/ioctls.h>
#include <stddef.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>

/*
 * Defines.
 */

#define TERMINAL_EDITOR_VERSION "0.0.1"

// Maps ASCII letters to their control character counterpart.
// i.e. This maps 'a' (97) to 1 and 'z' (122) to 26.
#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

/*
 * Data.
 */

struct TextRow {
    int size;
    char *chars;
};

struct EditorConfig {
    int cursorX;
    int cursorY;

    int termRows;
    int termCols;

    int rowAmt;
    struct TextRow *rows;

    // Caches the original terminal attributes for later cleanup.
    struct termios og_termios;
};

struct EditorConfig editor;

/*
 * "Append Buffer" type.
 */
struct AppendBuf {
    char *buf;
    size_t len;
};

#define NEW_APPEND_BUF {NULL, 0}

void bufAppend(struct AppendBuf *aBuf, const char *s, int len) {
    // Reallocate the underlying buffer for the string content.
    char *newBuf = realloc(aBuf->buf, aBuf->len + len);
    if (newBuf == NULL) {
        return;
    }
    // Copy the source string `s` onto the new portion of the re-allocated buffer.
    memcpy(&newBuf[aBuf->len], s, len);

    // Update the structure's fields.
    aBuf->buf = newBuf;
    aBuf->len += len;
}

void freeAppendBuf(struct AppendBuf *aBuf) {
    free(aBuf->buf);
}

/*
 * Terminal handling.
 */

void clearTermScreen() {
    // Clears the screen by writing the escape sequence: '\x1b', '[', '2', 'J' to STDOUT.
    write(STDOUT_FILENO, "\x1b[2J", 4);
}

// void clearTermScreenBuf(struct AppendBuf *aBuf) {
//     // Clears the screen by writing the escape sequence: '\x1b', '[', '2', 'J' to STDOUT.
//     bufAppend(aBuf, "\x1b[2J", 4);
// }

void clearTermLine(struct AppendBuf *aBuf) {
    bufAppend(aBuf, "\x1b[K", 3);
}

void resetTermCursor() {
    // Position the cursor at the top-left of the terminal.
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void resetTermCursorBuf(struct AppendBuf *aBuf) {
    // Position the cursor at the top-left of the terminal.
    bufAppend(aBuf, "\x1b[H", 3);
}

void showCursor(struct AppendBuf *aBuf) {
    bufAppend(aBuf, "\x1b[?25h", 6);
}

void hideCursor(struct AppendBuf *aBuf) {
    bufAppend(aBuf, "\x1b[?25l", 6);
}

// Crash the program with an explanatory string `s`.
void die(const char *s) {
    clearTermScreen();
    resetTermCursor();

    perror(s);
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
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1) != 1)) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    // Intercept arrow keys so that they are read as special characters.
    // The mapping is as follows:
    // - Arrow keys are read as an escape sequence that starts with '\x1b' and '['.
    //   which are then followed by A (up), B (down), C (right), or D (left).
    // 
    // The logic below reads past the first two bytes of the escape sequence and then
    // maps the A, B, C, or D as ARROW_UP, ARROW_DOWN, ARROW_RIGHT, or ARROW_LEFT, respectively.
    if (c == '\x1b') {
        char seq[3];

        if (read(STDOUT_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDOUT_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    
        return '\x1b';
    }
    else {
        return c;
    }
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
 * Row operations
 */
void editorAppendRow(char *s, size_t len) {
    editor.rows = realloc(editor.rows, sizeof(struct TextRow) * (editor.rowAmt + 1));

    int at = editor.rowAmt;
    editor.rows[at].size = len;
    editor.rows[at].chars = malloc(len + 1);
    memcpy(editor.rows[at].chars, s, len);
    editor.rows[at].chars[len] = '\0';
    editor.rowAmt++;
}

/*
 * File IO.
 */

void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }

    char *line = NULL;
    size_t lineCapacity = 0;
    ssize_t lineLen;

    while ((lineLen = getline(&line, &lineCapacity, fp)) != -1) {
        while (lineLen > 0 
            && (line[lineLen - 1] == '\n' || line[lineLen - 1] == '\r')
        ) {
            lineLen--;
        }
        editorAppendRow(line, lineLen);
    }
    free(line);
    fclose(fp);
}

/*
 * Input handling.
 */


void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (editor.cursorX == 0) {
                break;
            }
            editor.cursorX--;
            break;

        case ARROW_RIGHT:
            if (editor.cursorX == editor.termCols - 1) {
                break;
            }
            editor.cursorX++;
            break;

        case ARROW_UP:
            if (editor.cursorY == 0) {
                break;
            }
            editor.cursorY--;
            break;

        case ARROW_DOWN:
            if (editor.cursorY == editor.termRows - 1) {
                break;
            }
            editor.cursorY++;
            break;
    }
}

// Waits for a key press and processes it. 
void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            clearTermScreen();
            resetTermCursor();
            exit(0);
            break;


        case HOME_KEY:
            editor.cursorX = 0;
            break;

        case END_KEY:
            editor.cursorX = editor.termCols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        {
            // Move the cursor to the top or bottom of the screen.
            // We always move the `editor.termRows` up/down since the 
            // move cursor function already covers clamping so we don't worry 
            // about going out of bounds.
            for (int _i = 0; _i < editor.termRows; ++_i) {
                editorMoveCursor((c == PAGE_UP)? ARROW_UP : ARROW_DOWN);
            }
            break;
        }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*
 * Output handling.
 */

void editorDrawRows(struct AppendBuf *aBuf) {
    for (int y = 0; y < editor.termRows; y++) {
        // Draw a line without text.
        if (y >= editor.rowAmt) {
            if (editor.rowAmt == 0 && y == editor.termRows / 3) {
                char welcome[80];
                int welcomeLen = snprintf(welcome, sizeof(welcome), 
                "Terminal Editor - Version %s", TERMINAL_EDITOR_VERSION);

                if (welcomeLen > editor.termCols) {
                    welcomeLen = editor.termCols;
                }
                int padding = (editor.termCols - welcomeLen) / 2;
                if (padding > 0) {
                    bufAppend(aBuf, "~", 1);
                    padding--;
                }
                while (padding > 0) {
                    padding--;
                    bufAppend(aBuf, " ", 1);
                }

                bufAppend(aBuf, welcome, welcomeLen);
            }
            else {
                bufAppend(aBuf, "~", 1);
            }
        }
        // Draw a line with text.
        else {
            int len = editor.rows[y].size;
            if (len > editor.termCols) {
                len = editor.termCols;
            }
            bufAppend(aBuf, editor.rows[y].chars, len);
        }

        clearTermLine(aBuf);

        // Avoid adding an extra new line at the end of the final row.
        if (y < editor.termRows - 1) {
            bufAppend(aBuf, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct AppendBuf aBuf = NEW_APPEND_BUF;

    hideCursor(&aBuf);
    resetTermCursorBuf(&aBuf);

    editorDrawRows(&aBuf);

    // Move the cursor to be at the position saved in the editor state.
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", editor.cursorY + 1, editor.cursorX + 1);
    bufAppend(&aBuf, buf, strlen(buf));

    showCursor(&aBuf);

    write(STDOUT_FILENO, aBuf.buf, aBuf.len);
    freeAppendBuf(&aBuf);
}

/*
 * Program initialization.
 */

void initEditor() {
    // Initialize the cursor position to be at the top-left corner.
    editor.cursorX = 0;
    editor.cursorY = 0 ;

    editor.rowAmt = 0;
    editor.rows = NULL;

    if (getWindowSize(&editor.termRows, &editor.termCols) == -1) {
        die("getWindowSize");
    }
}

int main(int argc, char *argv[]) {
    enableTermRawMode();
    initEditor();

    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (true) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
