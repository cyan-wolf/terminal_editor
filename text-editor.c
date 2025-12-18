// For 'enabling' POSIX extensions like `getline`.
#define _GNU_SOURCE

#include <asm-generic/ioctls.h>
#include <stddef.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>

/*
 * Defines.
 */

#define TERMINAL_EDITOR_VERSION "0.0.1"

#define TERMINAL_EDITOR_TAB_SIZE 8

#define TERMINAL_EDITOR_STATUS_MSG_TIMEOUT 5

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

    int renderSize;
    char *render;
};

struct EditorConfig {
    int cursorX;
    int cursorY;
    int renderCursorX;

    int rowOffset;
    int colOffset;

    int termRows;
    int termCols;

    int rowAmt;
    struct TextRow *rows;

    char *filename;

    char statusMsg[80];
    time_t statusMsgTime;

    // Caches the original terminal attributes for later cleanup.
    struct termios ogTermios;
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
    if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &editor.ogTermios) == -1) {
        die("tcsetattr");
    }
}

void enableTermRawMode() {
    // Get current terminal attribututes.
    if (tcgetattr(STDIN_FILENO, &editor.ogTermios) == -1) {
        die("tcsetattr");
    }
    
    // Set a callback for disabling raw mode (cleeanup).
    atexit(disableRawMode);

    // Copy the current terminal attributes for futher modification.
    struct termios raw = editor.ogTermios;

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
    char ch;
    while ((nread = read(STDIN_FILENO, &ch, 1) != 1)) {
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
    if (ch == '\x1b') {
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
        return ch;
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

int editorCursorXRealToRender(struct TextRow *row, int cursorX) {
    int renderCursorX = 0;
    
    for (int i = 0; i < cursorX; i++) {
        if (row->chars[i] == '\t') {
            renderCursorX += (TERMINAL_EDITOR_TAB_SIZE - 1) - (renderCursorX % TERMINAL_EDITOR_TAB_SIZE);
        }
        renderCursorX++;
    }
    return renderCursorX;
}

void editorUpdateRow(struct TextRow *row) {
    // Count the number of tabs in the row.
    int tabAmt = 0;
    for (int i = 0; i < row->size; i++) {
        if (row->chars[i] == '\t') {
            tabAmt++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabAmt*(TERMINAL_EDITOR_TAB_SIZE - 1) + 1);

    int renderIdx = 0;

    for (int i = 0; i < row->size; i++) {
        // Simulate tab spacing by adding spaces until the row index is a 
        // multiple of the tab size.
        if (row->chars[i] == '\t') {
            row->render[renderIdx++] = ' ';
            while (renderIdx % TERMINAL_EDITOR_TAB_SIZE != 0) {
                row->render[renderIdx++] = ' ';
            }
        }
        else {
            row->render[renderIdx++] = row->chars[i];
        }
    }
    row->render[renderIdx] = '\0';
    row->renderSize = renderIdx;
}

void editorAppendRow(char *s, size_t len) {
    editor.rows = realloc(editor.rows, sizeof(struct TextRow) * (editor.rowAmt + 1));

    int at = editor.rowAmt;
    editor.rows[at].size = len;
    editor.rows[at].chars = malloc(len + 1);
    memcpy(editor.rows[at].chars, s, len);
    editor.rows[at].chars[len] = '\0';

    editor.rows[at].renderSize = 0;
    editor.rows[at].render = NULL;
    editorUpdateRow(&editor.rows[at]);

    editor.rowAmt++;
}

void editorInsertCharIntoRow(struct TextRow* row, int at, int ch) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }
    row->chars = realloc(row->chars, row->size + 2);

    // Move the portion of the row at/after `at` by 1 to make room for 
    // inserting `ch` at `row->chars[at]`. 
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);

    row->size++;
    row->chars[at] = ch;
    editorUpdateRow(row);
}

/*
 * Editor operations.
 */

void editorInsertChar(int ch) {
    if (editor.cursorY == editor.rowAmt) {
        editorAppendRow("", 0);
    }
    editorInsertCharIntoRow(&editor.rows[editor.cursorY], editor.cursorX, ch);
    editor.cursorX++;
}

/*
 * File IO.
 */

void editorOpen(char *filename) {
    free(editor.filename);
    editor.filename = strdup(filename);

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
    struct TextRow *currRow = (editor.cursorY >= editor.rowAmt)? NULL : &editor.rows[editor.cursorY];

    switch (key) {
        case ARROW_LEFT:
            if (editor.cursorX == 0) {
                // Move to the end of the previous line (if it exists).
                if (editor.cursorY > 0) {
                    editor.cursorY--;
                    editor.cursorX = editor.rows[editor.cursorY].size;
                }

                break;
            }
            editor.cursorX--;
            break;

        case ARROW_RIGHT:
            if (currRow && editor.cursorX < currRow->size) {
                editor.cursorX++;
            }
            else if (currRow && editor.cursorX == currRow->size) {
                editor.cursorY++;
                editor.cursorX = 0;
            }
            break;

        case ARROW_UP:
            if (editor.cursorY == 0) {
                break;
            }
            editor.cursorY--;
            break;

        case ARROW_DOWN:
            // Stop the cursor from pointing to an out-of-bounds row.
            if (editor.cursorY >= editor.rowAmt) {
                break;
            }
            editor.cursorY++;
            break;
    }

    // Snap the cursor's x position to the length of the current row if it 
    // goes past it.
    currRow = (editor.cursorY >= editor.rowAmt)? NULL : &editor.rows[editor.cursorY];
    int rowLen = (currRow != NULL)? currRow->size : 0;
    if (editor.cursorX > rowLen) {
        editor.cursorX = rowLen;
    } 
}

// Waits for a key press and processes it. 
void editorProcessKeypress() {
    int ch = editorReadKey();

    switch (ch) {
        case CTRL_KEY('q'):
            clearTermScreen();
            resetTermCursor();
            exit(0);
            break;


        case HOME_KEY:
            editor.cursorX = 0;
            break;

        case END_KEY:
            if (editor.cursorY < editor.rowAmt) {
                editor.cursorX = editor.rows[editor.cursorY].size;
            }
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        {
            if (ch == PAGE_UP) {
                editor.cursorY = editor.rowOffset;
            }
            else if (ch == PAGE_DOWN) {
                editor.cursorY = editor.rowOffset + editor.termRows - 1;

                if (editor.cursorY > editor.rowAmt) {
                    editor.cursorY = editor.rowAmt;
                }
            }
            for (int _i = 0; _i < editor.rowAmt; ++_i) {
                editorMoveCursor((ch == PAGE_UP)? ARROW_UP : ARROW_DOWN);
            }
            break;
        }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(ch);
            break;

        default:
            editorInsertChar(ch);
            break;
    }
}

/*
 * Output handling.
 */

void editorScroll() {
    editor.renderCursorX = 0;
    if (editor.cursorY < editor.rowAmt) {
        editor.renderCursorX = editorCursorXRealToRender(&editor.rows[editor.cursorY], editor.cursorX);
    }

    if (editor.cursorY < editor.rowOffset) {
        editor.rowOffset = editor.cursorY;
    }
    if (editor.cursorY >= editor.rowOffset + editor.termRows) {
        editor.rowOffset = editor.cursorY - editor.termRows + 1;
    }

    if (editor.renderCursorX < editor.colOffset) {
        editor.colOffset = editor.renderCursorX;
    }
    if (editor.renderCursorX >= editor.colOffset + editor.termCols) {
        editor.colOffset = editor.renderCursorX - editor.termCols + 1;
    }
}

void editorDrawRows(struct AppendBuf *aBuf) {
    for (int y = 0; y < editor.termRows; y++) {
        int fileRow = y + editor.rowOffset;

        // Draw a line without text.
        if (fileRow >= editor.rowAmt) {
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
            int len = editor.rows[fileRow].renderSize - editor.colOffset;
            if (len < 0) {
                len = 0;
            }
            if (len > editor.termCols) {
                len = editor.termCols;
            }
            bufAppend(aBuf, &editor.rows[fileRow].render[editor.colOffset], len);
        }

        clearTermLine(aBuf);

        bufAppend(aBuf, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct AppendBuf *aBuf) {
    // Invert terminal colors for this row.
    bufAppend(aBuf, "\x1b[7m", 4);

    // Reserve the left and right portions of the status bar.
    char statusLeft[80], statusRight[80];

    int statusLeftLen = snprintf(statusLeft, sizeof(statusLeft), "%.20s - %d lines",
        (editor.filename != NULL)? editor.filename : "[No Filename]", 
        editor.rowAmt);

    int statusRightLen = snprintf(statusRight, sizeof(statusRight), "%d/%d", 
        editor.cursorY + 1, editor.rowAmt);

    if (statusLeftLen > editor.termCols) {
        statusLeftLen = editor.termCols;
    }
    // Add the left portion of the status to the screen.
    bufAppend(aBuf, statusLeft, statusLeftLen);

    while (statusLeftLen < editor.termCols) {
        // Add the right poriton of the status to the screen once we determine that 
        // we have enough space for the right portion.
        if (editor.termCols - statusLeftLen == statusRightLen) {
            bufAppend(aBuf, statusRight, statusRightLen);
            break;
        }
        // Add the margin between the left and right portions of the status 
        // to the screen.
        else {
            bufAppend(aBuf, " ", 1);
            statusLeftLen++;
        }
    }
    // Reset terminal colors back to normal.
    bufAppend(aBuf, "\x1b[m", 3);
    bufAppend(aBuf, "\r\n", 2);
}

void editorDrawMessageBar(struct AppendBuf *aBuf) {
    bufAppend(aBuf, "\x1b[K", 3);
    int msgLen = strlen(editor.statusMsg);
    
    if (msgLen > editor.termCols) {
        msgLen = editor.termCols;
    }
    if (msgLen && time(NULL) - editor.statusMsgTime < TERMINAL_EDITOR_STATUS_MSG_TIMEOUT) {
        bufAppend(aBuf, editor.statusMsg, msgLen);
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct AppendBuf aBuf = NEW_APPEND_BUF;

    hideCursor(&aBuf);
    resetTermCursorBuf(&aBuf);

    editorDrawRows(&aBuf);
    editorDrawStatusBar(&aBuf);
    editorDrawMessageBar(&aBuf);

    // Move the cursor to be at the position saved in the editor state.
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (editor.cursorY - editor.rowOffset) + 1, (editor.renderCursorX - editor.colOffset) + 1);
    bufAppend(&aBuf, buf, strlen(buf));

    showCursor(&aBuf);

    write(STDOUT_FILENO, aBuf.buf, aBuf.len);
    freeAppendBuf(&aBuf);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(editor.statusMsg, sizeof(editor.statusMsg), fmt, ap);
    va_end(ap);
    editor.statusMsgTime = time(NULL);
}

/*
 * Program initialization.
 */

void initEditor() {
    // Initialize the cursor position to be at the top-left corner.
    editor.cursorX = 0;
    editor.cursorY = 0;
    editor.renderCursorX = 0;

    editor.rowOffset = 0;
    editor.colOffset = 0;

    editor.rowAmt = 0;
    editor.rows = NULL;
    
    editor.filename = NULL;

    editor.statusMsg[0] = '\0';
    editor.statusMsgTime = 0;

    if (getWindowSize(&editor.termRows, &editor.termCols) == -1) {
        die("getWindowSize");
    }
    editor.termRows -= 2; // make room for the status rows at the bottom
}

int main(int argc, char *argv[]) {
    enableTermRawMode();
    initEditor();

    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: press CTRL-Q to quit");

    while (true) {
        editorRefreshScreen();

        // Blocks until a keypress is read.
        editorProcessKeypress();
    }

    return 0;
}
