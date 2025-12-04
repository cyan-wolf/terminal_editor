#include <unistd.h>
#include <termios.h>
#include <stdlib.h>

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

    // Disable echo and canonical (line-by-line) mode.
    raw.c_lflag &= ~(ECHO | ICANON);

    // Set the modified terminal attributes.
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
    enableTermRawMode();

    char c;

    while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {

    }

    return 0;
}
