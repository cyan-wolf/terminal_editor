#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

// Caches the original terminal attributes for later cleanup.
struct termios og_termios;

void disableRawMode() {
    tcsetattr(STDERR_FILENO, TCSAFLUSH, &og_termios);
}

void enableTermRawMode() {
    // Get current terminal attribututes.
    tcgetattr(STDIN_FILENO, &og_termios);
    
    // Set a callback for disabling raw mode (cleeanup).
    atexit(disableRawMode);

    // Copy the current terminal attributes for futher modification.
    struct termios raw = og_termios;

    // Diable the default CTRL-S and CTRL-Q handling.
    raw.c_iflag &= ~(IXON);

    // Disable the:
    // - echo
    // - canonical (line-by-line) mode.
    // - signal processing (i.e. from CTRL-C or CTRL-Z)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // Set the modified terminal attributes.
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
    enableTermRawMode();

    char c;

    while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
        if (iscntrl(c)) { // when c is a control character (non-printable)
            printf("%d\n", c);
        }
        else {
            // Print both as the codepoint and the character.
            printf("%d ('%c')\n", c, c);
        }
    }

    return 0;
}
