#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define SHIBA_VER "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum ekeys {
    vk_left = 1000,
    vk_right,
    vk_up,
    vk_down,
    vk_pageup,
    vk_pagedown
};

struct EditorInfo {
    int cx, cy;
    int w;
    int h;
    termios origTermios{};
};

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

EditorInfo editorInfo{};

void abAppend(abuf *ab, const char *s, int len) {
    char *n = (char*)realloc(ab->b, ab->len + len);

    if (n == NULL) {
        return;
    }

    memcpy(&n[ab->len], s, len);
    ab->b = n;
    ab->len += len;
}

void abFree(abuf *ab) {
    free(ab->b);
}

void cls() {
    write(STDOUT_FILENO, "\x1b[2J", 4); //cls
    write(STDOUT_FILENO, "\x1b[H", 3); //move cursor
}

void die(const char *s) {
    cls();

    perror(s);
    exit(1);
}

void quit() {
    cls();
    exit(EXIT_SUCCESS);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &editorInfo.origTermios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &editorInfo.origTermios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = editorInfo.origTermios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

void eMove(int k) {
    switch (k) {
        case vk_left: {
            if (editorInfo.cx != 0) {
                --editorInfo.cx;
            }
        } break;

        case vk_right: {
            if (editorInfo.cx != editorInfo.w - 1) {
                ++editorInfo.cx;
            }
        } break;

        case vk_up: {
            if (editorInfo.cy != 0) {
                --editorInfo.cy;
            }
        } break;

        case vk_down: {
            if (editorInfo.cy != editorInfo.h - 1) {
                ++editorInfo.cy;
            }
        } break;
    }
}

int eReadKey() {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }

        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] < '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }

                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '5': return vk_pageup;
                        case '6': return vk_pagedown;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return vk_up;
                    case 'B': return vk_down;
                    case 'C': return vk_right;
                    case 'D': return vk_left;
                }
            }
        }

        return '\x1b';
    }

    return c;
}

int cursorPosition(int *x, int *y) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }

        if (buf[i] == 'R') {
            break;
        }

        ++i;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }

    if (sscanf(&buf[2], "%d;%d", y, x) != 2) {
        return -1;
    }

    return 0;
}

int windowSize(int *w, int *h) {
    winsize ws{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }

        return cursorPosition(w, h);
    }

    *w = ws.ws_col;
    *h = ws.ws_row;

    return 0;
}

void eDrawRows(abuf *ab) {
    for (int y = 0; y < editorInfo.h; ++y) {
        if (y == editorInfo.h / 3) {
            char welcome[80];
            int wlen = snprintf(welcome, sizeof(welcome), "shabi version %s", SHIBA_VER);

            if (wlen > editorInfo.w) {
                wlen = editorInfo.w;
            }

            int padding = (editorInfo.w - wlen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
            }

            while (padding--) {
                abAppend(ab, " ", 1);
            }

            abAppend(ab, welcome, wlen);
        } else {
            abAppend(ab, "~", 1);
        }

        abAppend(ab, "\x1b[K", 3);

        if (y < editorInfo.h - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void ecls() {
    //cls();

    abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    eDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", editorInfo.cy + 1, editorInfo.cx + 1);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void eKeypress() {
    int k = eReadKey();

    switch (k) {
        case CTRL_KEY('q'): quit();

        case vk_pagedown:
        case vk_pageup: {
            int times = editorInfo.h;
            while (times--) {
                eMove(k == vk_pageup ? vk_up : vk_down);
            }
        } break;

        case vk_up:
        case vk_left:
        case vk_down:
        case vk_right: eMove(k); break;
    }
}

void eInit() {
    editorInfo.cx = 0;
    editorInfo.cy = 0;

    if (windowSize(&editorInfo.w, &editorInfo.h) == -1) {
        die("windowSize");
    }
}

int main() {
    enableRawMode();
    eInit();

    while (true) {
        ecls();
        eKeypress();
    }

    return 0;
}
