#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define SHIBA_VER "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define TAB_SIZE 4

enum ekeys {
    vk_left = 1000,
    vk_right,
    vk_up,
    vk_down,
    vk_delete,
    vk_home,
    vk_end,
    vk_pageup,
    vk_pagedown
};

struct eline {
    int size;
    int rsize;
    char *data;
    char *rdata;
};

struct EditorInfo {
    int cx, cy;
    int rx;
    int yoffset;
    int xoffset;
    int w;
    int h;
    int linecount;
    char *filename;
    char statusmsg[80];
    time_t statusmsgTime;
    eline *line;
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

int eCxToRx(eline *line, int cx) {
    int rx = 0;
    for (int i = 0; i < cx; ++i) {
        if (line->data[i] == '\t') {
            rx += (TAB_SIZE - 1) - (rx % TAB_SIZE);
        }

        ++rx;
    }

    return rx;
}

void eUpdateLine(eline *line) {
    int tabs = 0;
    for (int i = 0; i < line->size; ++i) {
        if (line->data[i] == '\t') {
            ++tabs;
        }
    }

    free(line->rdata);
    line->rdata  = (char*)malloc(line->size + tabs * (TAB_SIZE - 1) + 1);

    int idx = 0;
    for (int j = 0; j < line->size; ++j) {
        if (line->data[j] == '\t') {
            line->rdata[idx++] = ' ';
            while (idx % TAB_SIZE != 0) {
                line->rdata[idx++] = ' ';
            }
        } else {
            line->rdata[idx++] = line->data[j];
        }
    }

    line->rdata[idx] = '\0';
    line->rsize = idx;
}

void eAppendLine(char *line, size_t len) {
    editorInfo.line = (eline*)realloc(editorInfo.line, sizeof(eline) * (editorInfo.linecount + 1));

    int idx = editorInfo.linecount;
    editorInfo.line[idx].size = len;
    editorInfo.line[idx].data = (char*)malloc(len + 1);
    memcpy(editorInfo.line[idx].data, line, len);
    editorInfo.line[idx].data[len] = '\0';

    editorInfo.line[idx].rsize = 0;
    editorInfo.line[idx].rdata = NULL;
    eUpdateLine(&editorInfo.line[idx]);

    ++editorInfo.linecount;
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
    eline *line = (editorInfo.cy >= editorInfo.linecount) ? NULL : &editorInfo.line[editorInfo.cy];

    switch (k) {
        case vk_left: {
            if (editorInfo.cx != 0) {
                --editorInfo.cx;
            } else if (editorInfo.cy > 0) {
                --editorInfo.cy;
                editorInfo.cx = editorInfo.line[editorInfo.cy].size;
            }
        } break;

        case vk_right: {
            if (line && editorInfo.cx < line->size) {
                ++editorInfo.cx;
            } else if (line && editorInfo.cx == line->size) {
                ++editorInfo.cy;
                editorInfo.cx = 0;
            }
        } break;

        case vk_up: {
            if (editorInfo.cy != 0) {
                --editorInfo.cy;
            }
        } break;

        case vk_down: {
            if (editorInfo.cy < editorInfo.linecount) {
                ++editorInfo.cy;
            }
        } break;

        default: break;
    }

    line = (editorInfo.cy >= editorInfo.linecount) ? NULL : &editorInfo.line[editorInfo.cy];
    int linelen = line ? line->size : 0;
    if (editorInfo.cx > linelen) {
        editorInfo.cx = linelen;
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
                        case '3': return vk_delete;
                        case '5': return vk_pageup;
                        case '6': return vk_pagedown;
                        case '1':
                        case '7': return vk_home;
                        case '4':
                        case '8': return vk_end;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return vk_up;
                    case 'B': return vk_down;
                    case 'C': return vk_right;
                    case 'D': return vk_left;
                    case 'H': return vk_home;
                    case 'F': return vk_end;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return vk_home;
                case 'F': return vk_end;
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

void eOpen(char *filename) {
    free(editorInfo.filename);
    editorInfo.filename = filename;

    FILE *file = fopen(filename, "r");
    if (!file) {
        die("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, file)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            --linelen;
        }

        eAppendLine(line, linelen);
    }

    free(line);
    fclose(file);
}

void eScroll() {
    editorInfo.rx = editorInfo.cx;
    if (editorInfo.cy < editorInfo.linecount) {
        editorInfo.rx = eCxToRx(&editorInfo.line[editorInfo.cy], editorInfo.cx);
    }

    if (editorInfo.cy < editorInfo.yoffset) {
        editorInfo.yoffset = editorInfo.cy;
    }

    if (editorInfo.cy >= editorInfo.yoffset + editorInfo.h) {
        editorInfo.yoffset = editorInfo.cy - editorInfo.h + 1;
    }

    if (editorInfo.rx < editorInfo.xoffset) {
        editorInfo.xoffset = editorInfo.rx;
    }

    if (editorInfo.rx >= editorInfo.xoffset + editorInfo.w) {
        editorInfo.xoffset = editorInfo.rx - editorInfo.w + 1;
    }
}

void eDrawLines(abuf *ab) {
    for (int y = 0; y < editorInfo.h; ++y) {
        int fileline = y + editorInfo.yoffset;
        if (fileline >= editorInfo.linecount) {
            if (editorInfo.linecount == 0 && y == editorInfo.h / 3) {
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
        } else {
            int len = editorInfo.line[fileline].rsize - editorInfo.xoffset;
            len = len < 0 ? 0 : len;

            if (len > editorInfo.w) {
                len = editorInfo.w;
            }

            abAppend(ab, &editorInfo.line[fileline].rdata[editorInfo.xoffset], len);
        }

        abAppend(ab, "\x1b[K", 3);

        //if (y < editorInfo.h - 1) {
            abAppend(ab, "\r\n", 2);
        //}
    }
}

void eDrawStatusBar(abuf *ab) {
    abAppend(ab, "\x1b[48;5;235m", 11); //set background color x1b[48;5;[]m replace [] with the color
    abAppend(ab, "\x1b[38;5;245m", 11); //set foreground color x1b[38;5;[]m replace [] with the color

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                       editorInfo.filename ? editorInfo.filename : "empty", editorInfo.linecount);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", editorInfo.cy + 1, editorInfo.linecount);

    if (len > editorInfo.w) {
        len = editorInfo.w;
    }
    abAppend(ab, status, len);

    while (len < editorInfo.w) {
        if (editorInfo.w - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            ++len;
        }
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void eDrawMsgBar(abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(editorInfo.statusmsg);

    if (msglen > editorInfo.w) {
        msglen = editorInfo.w;
    }

    if (msglen && time(NULL) - editorInfo.statusmsgTime < 5) {
        abAppend(ab, editorInfo.statusmsg, msglen);
    }
}

void ecls() {
    eScroll();

    abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    eDrawLines(&ab);
    eDrawStatusBar(&ab);
    eDrawMsgBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (editorInfo.cy - editorInfo.yoffset) + 1, (editorInfo.rx - editorInfo.xoffset) + 1);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void eSetStatus(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(editorInfo.statusmsg, sizeof(editorInfo.statusmsg), fmt, args);
    va_end(args);
    editorInfo.statusmsgTime = time(NULL);
}

void eKeypress() {
    int k = eReadKey();

    switch (k) {
        case CTRL_KEY('q'): quit();

        case vk_home: editorInfo.cx = 0; break;
        case vk_end: {
            if (editorInfo.cy < editorInfo.linecount) {
                editorInfo.cx = editorInfo.line[editorInfo.cy].size;
            }
        } break;

        case vk_pagedown:
        case vk_pageup: {
            if (k == vk_pageup) {
                editorInfo.cy = editorInfo.yoffset;
            } else {
                editorInfo.cy = editorInfo.yoffset + editorInfo.h - 1;
                if (editorInfo.cy > editorInfo.linecount) {
                    editorInfo.cy = editorInfo.linecount;
                }
            }

            int times = editorInfo.h;
            while (times--) {
                eMove(k == vk_pageup ? vk_up : vk_down);
            }
        } break;

        case vk_up:
        case vk_left:
        case vk_down:
        case vk_right: eMove(k); break;
        default: break;
    }
}

void eInit() {
    editorInfo.cx = 0;
    editorInfo.cy = 0;
    editorInfo.rx = 0;
    editorInfo.yoffset = 0;
    editorInfo.xoffset = 0;
    editorInfo.linecount = 0;
    editorInfo.line = NULL;
    editorInfo.filename = NULL;
    editorInfo.statusmsg[0] = '\0';
    editorInfo.statusmsgTime = 0;

    if (windowSize(&editorInfo.w, &editorInfo.h) == -1) {
        die("windowSize");
    }

    editorInfo.h -= 2;
}

int main(int argc, char **argv) {
    enableRawMode();
    eInit();

    if (argc >= 2) {
        eOpen(argv[1]);
    }

    eSetStatus("HELP: Ctrl-Q = quit");

    while (true) {
        ecls();
        eKeypress();
    }

    return 0;
}
