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
#include <fcntl.h>
#include <ctype.h>

/*
 * Defines.
 */

#define TERMINAL_EDITOR_VERSION "0.0.1"

#define TERMINAL_EDITOR_TAB_SIZE 8

#define TERMINAL_EDITOR_STATUS_MSG_TIMEOUT 5

#define TERMINAL_EDITOR_QUIT_TIMES 3

// Maps ASCII letters to their control character counterpart.
// i.e. This maps 'a' (97) to 1 and 'z' (122) to 26.
#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey {
    BACKSPACE = 127,
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

enum EditorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/*
 * Data.
 */

struct EditorSyntax {
    char *fileType;
    char **fileMatch;
    char **keywords;
    char *singleLineCommentStart;
    int flags;
};

struct TextRow {
    int size;
    char *chars;

    int renderSize;
    char *render;

    unsigned char *highlight;
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

    struct EditorSyntax *syntax;

    bool isDirty;

    // Caches the original terminal attributes for later cleanup.
    struct termios ogTermios;
};

struct EditorConfig editor;

/*
 * File types. 
 */

char *cLangExtensions[] = { ".c", ".h", ".cpp", NULL };

char *cLangKeywords[] = {
    // Proper keywords.
    "switch",
    "if",
    "while",
    "for",
    "break",
    "continue",
    "return",
    "else",
    "struct",
    "union",
    "typedef",
    "static",
    "enum",
    "class",
    "case",

    // Type names / modifiers.              
    "int|",
    "long|",
    "double|",
    "float|",
    "char|",
    "unsigned|",
    "signed|",
    "void|",

    NULL,
};

struct EditorSyntax highlightDb[] = {
    {
        "c",
        cLangExtensions,
        cLangKeywords,
        "//",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS,
    },
};

#define HIGHLIGHT_DB_ENTRIES (sizeof(highlightDb) / sizeof(highlightDb[0]))

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
 * Forward declarations.
 */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

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
 * Syntax highlighting. 
 */

bool isSeparator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(struct TextRow *row) {
    row->highlight = realloc(row->highlight, row->renderSize);
    memset(row->highlight, HL_NORMAL, row->renderSize);

    // If `editor.syntax` is not set then no file type was detected for the current file 
    // and no syntax highlighting will take place.
    if (editor.syntax == NULL) {
        return;
    }

    char **keywords = editor.syntax->keywords;

    char *singleLineCommentStart = editor.syntax->singleLineCommentStart;
    int singleLineCommentLen = (singleLineCommentStart)? strlen(singleLineCommentStart) : 0;

    bool prevWasSep = true;
    bool inString = false; 
    char stringDelim = '\0';
    
    int i = 0;
    while (i < row->renderSize) {
        char c = row->render[i];
        unsigned char prevHighlight = (i > 0)? row->highlight[i - 1] : HL_NORMAL;


        // Syntax highlighting for single line comments.
        if (singleLineCommentLen > 0 && !inString) {
            if (!strncmp(&row->render[i], singleLineCommentStart, singleLineCommentLen)) {
                memset(&row->highlight[i], HL_COMMENT, row->renderSize - i);
                break;
            }
        }

        // Syntax highlighting for strings.
        if (editor.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (inString) {
                row->highlight[i] = HL_STRING;

                // Highlight escaped single and double quotes.
                if (c == '\\' && i + 1 < row->renderSize) {
                    row->highlight[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                
                if (c == stringDelim) {
                    inString = false;
                    stringDelim = '\0';
                }
                i++;
                prevWasSep = true;
                continue;
            }
            else {
                if (c == '"' || c == '\'') {
                    inString = true;
                    stringDelim = c;
                    
                    row->highlight[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        // Syntax highlighting for numbers.
        if (editor.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prevWasSep || prevHighlight == HL_NUMBER)) || 
                (c == '.' && prevHighlight == HL_NUMBER)) {
                row->highlight[i] = HL_NUMBER;
                i++;
                prevWasSep = false;
                continue;
            }
        }

        // Syntax highlighting for keywords.
        if (prevWasSep) {
            int j;
            for (j = 0; keywords[j] != NULL; ++j) {
                int keywordLen = strlen(keywords[j]);
                bool isSecondaryKw = keywords[j][keywordLen - 1] == '|';

                // Secondary keywords have an extra marker '|' at the end, we 
                // decrement the keyword len to take into account the real length.
                if (isSecondaryKw) {
                    keywordLen--;
                }

                if (!strncmp(&row->render[i], keywords[j], keywordLen) && 
                    isSeparator(row->render[i + keywordLen])) {
                    memset(&row->highlight[i], isSecondaryKw ? HL_KEYWORD2 : HL_KEYWORD1, keywordLen);
                    i += keywordLen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prevWasSep = false;
                continue;
            }
        }

        prevWasSep = isSeparator(c);
        i++;
    }
}

int editorSyntaxToColor(int highlightType) {
    switch (highlightType) {
        case HL_COMMENT: return 36;
        case HL_KEYWORD1: return 33;
        case HL_KEYWORD2: return 32;
        case HL_STRING: return 35;
        case HL_NUMBER: return 31;
        case HL_MATCH: return 34;
        default: return 37;
    }
}

void editorSelectSyntaxHighlight() {
    editor.syntax = NULL;
    if (editor.filename == NULL) {
        return;
    }

    char *fileExt = strchr(editor.filename, '.');

    for (unsigned int i = 0; i < HIGHLIGHT_DB_ENTRIES; ++i) {
        struct EditorSyntax *syntax = &highlightDb[i];

        unsigned int j = 0;
        while (syntax->fileMatch[j]) {
            int isExt = (syntax->fileMatch[j][0] == '.');

            if ((isExt && fileExt && !strcmp(fileExt, syntax->fileMatch[j])) || 
                (!isExt && strstr(editor.filename, syntax->fileMatch[j]))) {
                editor.syntax = syntax;

                // Re-highlight all the file's rows after a syntax highlighting 
                // scheme is determined.
                for (int fileRow = 0; fileRow < editor.rowAmt; ++fileRow) {
                    editorUpdateSyntax(&editor.rows[fileRow]);
                }

                return;
            }
            j++;
        }
    }
}

/*
 * Row operations
 */

// Converts an index into the row's real backing character array 
// `row.chars` into an index into the row's rendereed character array 
// `row.render`.
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

// Does the same thing as `editorCursorXRealToRender` but in the other 
// direction where it turns a `row.render` index into a `row.chars` index.
int editorRenderCursorXToReal(struct TextRow *row, int renderCursorX) {
    int currRenderCursorX = 0;
    
    for (int cursorX = 0; cursorX < row->size; ++cursorX) {
        if (row->chars[cursorX] == '\t') {
            currRenderCursorX += (TERMINAL_EDITOR_TAB_SIZE - 1) - (currRenderCursorX % TERMINAL_EDITOR_TAB_SIZE);
        }
        currRenderCursorX++;

        if (currRenderCursorX > renderCursorX) {
            return cursorX;
        }
    }
    return row->size; // unreachable
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

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > editor.rowAmt) {
        return;
    }

    editor.rows = realloc(editor.rows, sizeof(struct TextRow) * (editor.rowAmt + 1));
    memmove(&editor.rows[at + 1], &editor.rows[at], sizeof(struct TextRow) * (editor.rowAmt - at));

    editor.rows[at].size = len;
    editor.rows[at].chars = malloc(len + 1);
    memcpy(editor.rows[at].chars, s, len);
    editor.rows[at].chars[len] = '\0';

    editor.rows[at].renderSize = 0;
    editor.rows[at].render = NULL;
    editor.rows[at].highlight = NULL;
    editorUpdateRow(&editor.rows[at]);

    editor.rowAmt++;
    editor.isDirty = true;
}

void editorFreeRow(struct TextRow *row) {
    free(row->render);
    free(row->chars);
    free(row->highlight);
}

void editorDeleteRow(int at) {
    if (at < 0 || at >= editor.rowAmt) {
        return;
    }
    editorFreeRow(&editor.rows[at]);
    memmove(&editor.rows[at], &editor.rows[at + 1], sizeof(struct TextRow) * (editor.rowAmt - at - 1));
    editor.rowAmt--;
    editor.isDirty = true;
}

void editorInsertCharIntoRow(struct TextRow *row, int at, int ch) {
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
    editor.isDirty = true;
}

void editorAppendStringToRow(struct TextRow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    editor.isDirty = true;
}

void editorDeleteCharFromRow(struct TextRow *row, int at) {
    if (at < 0 || at >= row->size) {
        return;
    }
    // Shift everything after `at` back one index to delete the character at 
    // index `at`.
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    editor.isDirty = true;
}

/*
 * Editor operations.
 */

void editorInsertChar(int ch) {
    if (editor.cursorY == editor.rowAmt) {
        editorInsertRow(editor.rowAmt, "", 0);
    }
    editorInsertCharIntoRow(&editor.rows[editor.cursorY], editor.cursorX, ch);
    editor.cursorX++;
}

void editorInsertNewline() {
    // We are at the start of a line, we can just add a new empty line.
    if (editor.cursorX == 0) {
        editorInsertRow(editor.cursorY, "", 0);
    }
    // We are pressing ENTER in the middle of an existing line, therefore 
    // we must split it along where the cursor's X position is.
    else {
        struct TextRow *row = &editor.rows[editor.cursorY];
        editorInsertRow(editor.cursorY + 1, &row->chars[editor.cursorX], row->size - editor.cursorX);

        // Reassign the row as it might have been invalidated by the call to 
        // `editorInsertRow`.
        row = &editor.rows[editor.cursorY];
        row->size = editor.cursorX;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    editor.cursorY++;
    editor.cursorX = 0;
}

void editorDelChar() {
    if (editor.cursorY == editor.rowAmt) {
        return;
    }
    if (editor.cursorX == 0 && editor.cursorY == 0) {
        return;
    }

    struct TextRow *row = &editor.rows[editor.cursorY];
    if (editor.cursorX > 0) {
        editorDeleteCharFromRow(row, editor.cursorX - 1);
        editor.cursorX--;
    }
    else {
        editor.cursorX = editor.rows[editor.cursorY - 1].size;
        editorAppendStringToRow(&editor.rows[editor.cursorY - 1], row->chars, row->size);
        editorDeleteRow(editor.cursorY);
        editor.cursorY--;
    }
}

/*
 * File IO.
 */

char *editorRowsToString(int *bufLen) {
    int totalLen = 0;
    for (int i = 0; i < editor.rowAmt; ++i) {
        totalLen += editor.rows[i].size + 1; // +1 for line feeds
    }
    *bufLen = totalLen; // save out param

    // Use the total character length calculated in the previous 
    // pass to allocate the entire buffer at once.
    char *buf = malloc(totalLen);

    // Iterator into the buffer. 
    // Always points to the start position of a line by the end of a loop 
    // iteration.
    char *iter = buf;
    
    for (int i = 0; i < editor.rowAmt; i++) {
        // Copy each row to the buffer.
        memcpy(iter, editor.rows[i].chars, editor.rows[i].size);

        // Move the iterator past the current line.
        iter += editor.rows[i].size;

        // Add a line feed to separate the lines in the file string.
        *iter = '\n';
        iter++;
    }
    return buf;
}

void editorOpen(char *filename) {
    free(editor.filename);
    editor.filename = strdup(filename);

    editorSelectSyntaxHighlight();

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
        editorInsertRow(editor.rowAmt, line, lineLen);
    }
    free(line);
    fclose(fp);

    // When loading the file contents, the file is marked as dirty.
    // We don't want this, so we mark the file as not dirty at the end of this
    // process.
    editor.isDirty = false;
}

void editorSave() {
    if (editor.filename == NULL) {
        editor.filename = editorPrompt("Save as: %s", NULL);

        if (editor.filename == NULL) {
            editorSetStatusMessage("Save aborted.");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(editor.filename, O_RDWR | O_CREAT, 0644);
    
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                // Mark the file as no longer dirty as we are saving it.
                editor.isDirty = false;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }    
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Cannot save file: %s", strerror(errno));
}

/*
 * Finding / text-search.
 */


void editorFindCallback(char *query, int key) {
    static int lastMatch = -1;
    static int direction = 1;

    static int savedHighlightLine;
    static char *savedHighlight = NULL;

    if (savedHighlight) {
        memcpy(editor.rows[savedHighlightLine].highlight, savedHighlight, editor.rows[savedHighlightLine].renderSize);
        free(savedHighlight);
        savedHighlight = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        lastMatch = -1;
        direction = 1;
        return;
    }
    else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    }
    else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    }
    else {
        lastMatch = -1;
        direction = 1;
    }

    if (lastMatch == -1) {
        direction = 1;
    }
    int current = lastMatch;

    // Loop through all the rows to see if we get a match for the 
    // user's search string.
    // If we do get a match, we move the cursor to the first row that 
    // matches.
    for (int i = 0; i < editor.rowAmt; ++i) {
        current += direction;
        if (current == -1) {
            current = editor.rowAmt - 1;
        }
        else if (current == editor.rowAmt) {
            current = 0;
        }

        struct TextRow *row = &editor.rows[current];
        char *match = strstr(row->render, query);

        if (match) {
            lastMatch = current;
            editor.cursorY = current;
            
            // The difference between the match and render pointers is an index into the 
            // render array, not the chars array. Since `editor.cursorX` is supposed to be 
            // an index into the chars array, we need to convert it.
            editor.cursorX = editorRenderCursorXToReal(row, match - row->render);
            editor.rowOffset = editor.rowAmt;

            // Save the line with the match before applying the highlight to 
            // be able to restore it later.
            savedHighlightLine = current;
            savedHighlight = malloc(row->renderSize);
            memcpy(savedHighlight, row->highlight, row->renderSize);

            // Highlight matches.
            memset(&row->highlight[match - row->render], HL_MATCH, strlen(query));

            break;
        }
    }
} 

void editorFind() {
    // Save the cursor position and offset in case we want to 
    // retore the cursor back to its original location, which 
    // happens when the user cancels a search.
    int savedCursorX = editor.cursorX;
    int savedCursorY = editor.cursorY;
    int savedColOffset = editor.colOffset;
    int savedRowOffset = editor.rowOffset;
    
    char *query = editorPrompt("Search %s (Use ESC/Arrow Keys/Enter)", editorFindCallback);
    
    // The user found what they where looking for, therefore we free the query.
    if (query) {
        free(query);
    }
    // The user canceled the search, therefore we restore the cursor's 
    // position and offset.
    else {
        editor.cursorX = savedCursorX;
        editor.cursorY = savedCursorY;
        editor.colOffset = savedColOffset;
        editor.rowOffset = savedRowOffset;
    }
}

/*
 * Input handling.
 */


char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufSize = 128;
    char *buf = malloc(bufSize);

    size_t bufLen = 0;
    buf[0] = '\0';

    while (true) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int ch = editorReadKey();

        if (ch == DEL_KEY || ch == CTRL_KEY('h') || ch == BACKSPACE) {
            if (bufLen != 0) {
                bufLen--;
                buf[bufLen] = '\0';
            }
        }
        else if (ch == '\x1b') {
            editorSetStatusMessage("");

            if (callback) {
                callback(buf, ch);
            }

            free(buf);
            return NULL;
        }
        else if (ch == '\r') {
            if (bufLen != 0) {
                editorSetStatusMessage("");

                if (callback) {
                    callback(buf, ch);
                }

                return buf;
            }
        }
        else if (!iscntrl(ch) && ch < 128) {
            if (bufLen == bufSize - 1) {
                bufSize *= 2;
                buf = realloc(buf, bufSize);
            }
            buf[bufLen] = ch;
            bufLen++;
            buf[bufLen] = '\0';
        }

        if (callback) {
            callback(buf, ch);
        }
    }
}

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
    static int quitTimes = TERMINAL_EDITOR_QUIT_TIMES;

    int ch = editorReadKey();

    switch (ch) {
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('q'):
            // Stop the user from quitting immediately if they have 
            // unsaved changes.
            if (editor.isDirty && quitTimes > 0) {
                editorSetStatusMessage("Warning: Unsaved changes! " 
                    "Press CTRL-Q %d more times to quit.", quitTimes);
                quitTimes--;
                return;
            }

            clearTermScreen();
            resetTermCursor();
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            editor.cursorX = 0;
            break;

        case END_KEY:
            if (editor.cursorY < editor.rowAmt) {
                editor.cursorX = editor.rows[editor.cursorY].size;
            }
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (ch == DEL_KEY) {
                // The delete key deletes the character in front the cursor.
                editorMoveCursor(ARROW_RIGHT);
            }
            editorDelChar();
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

        case CTRL_KEY('l'):
        case '\x1b':
            // Ignore control characters.
            break;

        default:
            editorInsertChar(ch);
            break;
    }

    quitTimes = TERMINAL_EDITOR_QUIT_TIMES;
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

            char *c = &editor.rows[fileRow].render[editor.colOffset];
            unsigned char *hl = &editor.rows[fileRow].highlight[editor.colOffset];
            int currColor = -1;

            for (int i = 0; i < len; ++i) {
                // Handle printing control characters.
                // They are printed using a '?' with inverted colors.
                if (iscntrl(c[i])) {
                    char sym = '?';
                    bufAppend(aBuf, "\x1b[7m", 4);
                    bufAppend(aBuf, &sym, 1);
                    bufAppend(aBuf, "\x1b[m", 3);

                    if (currColor != -1) {
                        char buf[16];
                        int cLen = snprintf(buf, sizeof(buf), "\x1b[%dm", currColor);
                        bufAppend(aBuf, buf, cLen);
                    }
                }
                // If the highlight corrresponding to this character is 
                // `HL_NORMAL` then append a formatting-reset code before 
                // the character.
                else if (hl[i] == HL_NORMAL) {
                   if (currColor != -1) {
                        bufAppend(aBuf, "\x1b[39m", 5);
                        currColor = -1;
                    }
                    bufAppend(aBuf, &c[i], 1); 
                }
                // Otherwise, append a color code before the character.
                else {
                    int color = editorSyntaxToColor(hl[i]);

                    if (color != currColor) {
                        currColor = color;
                        char buf[16];
                        int cLen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        bufAppend(aBuf, buf, cLen);
                    }
                    bufAppend(aBuf, &c[i], 1);
                }
            }
            // Append another formatting-reset code after appending 
            // all the row characters just in case.
            bufAppend(aBuf, "\x1b[39m", 5);
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

    int statusLeftLen = snprintf(statusLeft, sizeof(statusLeft), "%.20s - %d lines %s",
        (editor.filename != NULL)? editor.filename : "[No Filename]", 
        editor.rowAmt,
        (editor.isDirty)? "(modified)" : "");

    int statusRightLen = snprintf(statusRight, sizeof(statusRight), "%s | %d/%d", 
        (editor.syntax)? editor.syntax->fileType : "no file type",
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

    editor.syntax = NULL;

    editor.isDirty = false;

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

    editorSetStatusMessage("HELP: press CTRL-Q to quit or CTRL-S to save or CTRL-F to find");

    while (true) {
        editorRefreshScreen();

        // Blocks until a keypress is read.
        editorProcessKeypress();
    }

    return 0;
}
