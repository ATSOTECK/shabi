#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SHIBA_VER "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define TAB_SIZE 4
#define QUIT_TIMES 2

#define EDT true
#define CMD false

char welcomeMsg[] = "Welcome 傻屄 :)";

typedef enum {false, true} bool;

enum ekeys {
    vk_escape = '\x1b',
    vk_enter = '\r',
    vk_backspace = 127,
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

enum eHighlight {
    HLNormal,
    HLNumber,
    HLMatch
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)

typedef struct editorSyntax {
    char *filetype;
    char **filematch;
    int flags;
} editorSyntax;

typedef struct eline {
    int size;
    int rsize;
    char *data;
    char *rdata;
    unsigned char *hl;
} eline;

struct editorInfo {
    int cx, cy;
    int rx;
    int yoffset;
    int xoffset;
    int w;
    int h;
    int linecount;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsgTime;
    bool statuserror;
    eline *line;
    struct termios origTermios;
    editorSyntax *syntax;
    bool mode;
};

typedef struct abuf {
    char *b;
    int len;
} abuf;

#define ABUF_INIT {NULL, 0}

struct editorInfo editorInfo;

char *cHLExtensions[] = {".c", ".h", NULL};

editorSyntax HLDB[] = {
        {"c", cHLExtensions, HL_HIGHLIGHT_NUMBERS},
};

void ecls();
int eReadKey();
char *ePrompt(const char *prompt, void (*callback)(char *, int));

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

int eRxToCx(eline *line, int rx) {
    int currentRx = 0;
    int cx;
    for ( cx = 0; cx < line->size; ++cx) {
        if (line->data[cx] == '\t') {
            currentRx += (TAB_SIZE - 1) - (currentRx % TAB_SIZE);
        }

        ++currentRx;
        if (currentRx > rx) {
            return cx;
        }
    }

    return cx;
}

bool isSeperator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void eUpdateSyntax(eline *line) {
    line->hl = (unsigned char*)realloc(line->hl, line->rsize);
    memset(line->hl, HLNormal, line->rsize);

    int prevStep = 1;

    int i = 0;
    while (i < line->rsize) {
        char c = line->rdata[i];
        unsigned char prevHL = (i > 0) ? line->hl[i - 1] : HLNormal;

        if (isdigit(c) && (prevStep || prevHL == HLNumber) || (c == '.' && prevHL == HLNumber)) {
            line->hl[i] = HLNumber;
            ++i;
            prevStep = 0;
            continue;
        }

        prevStep = isSeperator(c);
        ++i;
    }
}

int syntaxToColor(int hl) {
    switch (hl) {
        case HLNumber: return 31;
        case HLMatch: return 34;
        default: return 37;
    }
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

    eUpdateSyntax(line);
}

void eInsertLine(int idx, char *line, size_t len) {
    if (idx < 0 || idx > editorInfo.linecount) {
        return;
    }

    editorInfo.line = (eline*)realloc(editorInfo.line, sizeof(eline) * (editorInfo.linecount + 1));
    memmove(&editorInfo.line[idx + 1], &editorInfo.line[idx], sizeof(eline) * (editorInfo.linecount - idx));

    editorInfo.line[idx].size = len;
    editorInfo.line[idx].data = (char*)malloc(len + 1);
    memcpy(editorInfo.line[idx].data, line, len);
    editorInfo.line[idx].data[len] = '\0';

    editorInfo.line[idx].rsize = 0;
    editorInfo.line[idx].rdata = NULL;
    editorInfo.line[idx].hl = NULL;
    eUpdateLine(&editorInfo.line[idx]);

    ++editorInfo.linecount;
    ++editorInfo.dirty;
}

void eInsertNewLine() {
    if (editorInfo.cx == 0) {
        eInsertLine(editorInfo.cy, NULL, 0);
    } else {
        eline *line = &editorInfo.line[editorInfo.cy];
        eInsertLine(editorInfo.cy + 1, &line->data[editorInfo.cx], line->size - editorInfo.cx);
        line = &editorInfo.line[editorInfo.cy];
        line->size = editorInfo.cx;
        line->data[line->size] = '\0';
        eUpdateLine(line);
    }

    ++editorInfo.cy;
    editorInfo.cx = 0;
}

void eFreeLine(eline *line) {
    free(line->rdata);
    free(line->data);
    free(line->hl);
}

void eDeleteLine(int idx) {
    if (idx < 0 || idx >= editorInfo.linecount) {
        return;
    }

    eFreeLine(&editorInfo.line[idx]);
    memmove(&editorInfo.line[idx], &editorInfo.line[idx + 1], sizeof(eline) * (editorInfo.linecount - idx - 1));
    --editorInfo.linecount;
    ++editorInfo.dirty;
}

void eLineInsertChar(eline *line, int idx, int c) {
    if (idx < 0 || idx > line->size) {
        idx = line->size;
    }

    line->data = (char*)realloc(line->data, line->size + 2);
    memmove(&line->data[idx + 1], &line->data[idx], line->size - idx + 1);
    ++line->size;
    line->data[idx] = (char)c;
    eUpdateLine(line);
    ++editorInfo.dirty;
}

void eLineDeleteChar(eline *line, int idx) {
    if (idx < 0 || idx >= line->size) {
        return;
    }

    memmove(&line->data[idx], &line->data[idx + 1], line->size - idx);
    --line->size;
    eUpdateLine(line);
    ++editorInfo.dirty;
}

void eLineAppendString(eline *line, char *s, size_t len) {
    line->data = (char*)realloc(line->data, line->size + len + 1);
    memcpy(&line->data[line->size], s, len);
    line->size += len;
    line->data[line->size] = '\0';
    eUpdateLine(line);
    ++editorInfo.dirty;
}

void eInsertChar(int c) {
    if (editorInfo.cy == editorInfo.linecount) {
        eInsertLine(editorInfo.linecount, NULL, 0);
    }

    eLineInsertChar(&editorInfo.line[editorInfo.cy], editorInfo.cx, c);
    ++editorInfo.cx;
}

void eDeleteChar() {
    if (editorInfo.cy == editorInfo.linecount || (editorInfo.cx == 0 && editorInfo.cy == 0)) {
        return;
    }

    eline *line = &editorInfo.line[editorInfo.cy];
    if (editorInfo.cx > 0) {
        eLineDeleteChar(line, editorInfo.cx - 1);
        --editorInfo.cx;
    } else {
        editorInfo.cx = editorInfo.line[editorInfo.cy - 1].size;
        eLineAppendString(&editorInfo.line[editorInfo.cy - 1], line->data, line->size);
        eDeleteLine(editorInfo.cy);
        --editorInfo.cy;
    }
}

void cls() {
    write(STDOUT_FILENO, "\x1b[2J", 4); //cls
    write(STDOUT_FILENO, "\x1b[H", 3); //move cursor
    write(STDOUT_FILENO, "\x1b[m", 3);
    write(STDOUT_FILENO, "\x1b]1337;CursorShape=0\x07", 21); //set cursor to a block, iTerm2 specific
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

void eSetStatus(const char *fmt, ...) {
    if (fmt == NULL) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(editorInfo.statusmsg, sizeof(editorInfo.statusmsg), fmt, args);
    va_end(args);
    editorInfo.statusmsgTime = time(NULL);
}

void eSetError(const char *fmt, ...) {
    if (fmt == NULL) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(editorInfo.statusmsg, sizeof(editorInfo.statusmsg), fmt, args);
    va_end(args);
    editorInfo.statusmsgTime = time(NULL);
    editorInfo.statuserror = true;
}

char *ePrompt(const char *prompt, void (*callback)(char *, int)) {
    if (prompt == NULL) {
        return NULL;
    }

    size_t bufsize = 128;
    char *buf = (char*)malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (true) {
        eSetStatus(prompt, buf);
        ecls();

        int k = eReadKey();
        if (k == vk_delete || k == CTRL_KEY('h') || k == vk_backspace) {
            if (buflen != 0) {
                buf[--buflen] = '\0';
            }
        } else if (k == vk_escape) {
            eSetStatus("");
            if (callback) {
                callback(buf, k);
            }
            free(buf);

            return NULL;
        } else if (k == vk_enter) {
            if (buflen != 0) {
                eSetStatus("");
                if (callback) {
                    callback(buf, k);
                }
                return buf;
            }
        } else if (!iscntrl(k) && k < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = (char*)realloc(buf, bufsize);
            }

            buf[buflen++] = (char)k;
            buf[buflen] = '\0';
        }

        if (callback) {
            callback(buf, k);
        }
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
    struct winsize ws;

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

char *eLinesToStr(int *buflen) {
    int totlen = 0;
    for (int i = 0; i < editorInfo.linecount; ++i) {
        totlen += editorInfo.line[i].size + 1;
    }
    *buflen = totlen;

    char *buf = (char*)malloc(totlen);
    char *p = buf;

    for (int i = 0; i < editorInfo.linecount; ++i) {
        memcpy(p, editorInfo.line[i].data, editorInfo.line[i].size);
        p += editorInfo.line[i].size;
        *p = '\n';
        ++p;
    }

    return buf;
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

        eInsertLine(editorInfo.linecount, line, linelen);
    }

    free(line);
    fclose(file);

    editorInfo.dirty = 0;
}

void eSave() {
    if (editorInfo.filename == NULL) {
        editorInfo.filename = ePrompt("Write as: %s", NULL);

        if (editorInfo.filename == NULL) {
            eSetStatus("Write aborted");
            return;
        }
    }

    int len;
    char *buf = eLinesToStr(&len);

    int fd = open(editorInfo.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                eSetStatus("%d bytes written to disk", len);
                editorInfo.dirty = 0;

                return;
            }
        }
        close(fd);
    }

    free(buf);
    eSetStatus("Can't save! I/O error: %s", strerror(errno));
}

void eFindCallback(char *q, int key) {
    static int lastMatch = -1;
    static int direction = 1;

    static int savedHLLine;
    static char *savedHL = NULL;

    if (savedHL) {
        memcpy(editorInfo.line[savedHLLine].hl, savedHL, editorInfo.line[savedHLLine].rsize);
        free(savedHL);
        savedHL = NULL;
    }

    if (key == vk_enter || key == vk_escape) {
        lastMatch = -1;
        direction = 1;
        return;
    } else if (key == vk_right || key == vk_down) {
        direction = 1;
    } else if (key == vk_left || key == vk_up) {
        direction = -1;
    } else {
        lastMatch = -1;
        direction = 1;
    }

    if (lastMatch == -1) {
        direction = 1;
    }
    int current = lastMatch;

    for (int i = 0; i < editorInfo.linecount; ++i) {
        current += direction;
        if (current == -1) {
            current = editorInfo.linecount - 1;
        } else if (current == editorInfo.linecount) {
            current = 0;
        }

        eline *line = &editorInfo.line[current];
        char *match = strstr(line->rdata, q);

        if (match) {
            lastMatch = current;
            editorInfo.cy = current;
            editorInfo.cx = eRxToCx(line, (int)(match - line->rdata));
            editorInfo.yoffset = editorInfo.linecount;

            savedHLLine = current;
            savedHL = (char*)malloc(line->rsize);
            memcpy(savedHL, line->hl, line->rsize);
            memset(&line->hl[match - line->rdata], HLMatch, strlen(q));
            break;
        }
    }
}

void eFind() {
    int savedCx = editorInfo.cx;
    int savedCy = editorInfo.cy;
    int savedXOffset = editorInfo.xoffset;
    int savedYOffset = editorInfo.yoffset;

    char *q = ePrompt("Search: %s", eFindCallback);

    if (q != NULL) {
        free(q);
    } else {
        editorInfo.cx = savedCx;
        editorInfo.cy = savedCy;
        editorInfo.xoffset = savedXOffset;
        editorInfo.yoffset = savedYOffset;
    }
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

            char *c = &editorInfo.line[fileline].rdata[editorInfo.xoffset];
            unsigned char *hl = &editorInfo.line[fileline].hl[editorInfo.xoffset];
            int currentColor = -1;
            for (int i = 0; i < len; ++i) {
                if (hl[i] == HLNormal) {
                    if (currentColor != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        currentColor = -1;
                    }
                    abAppend(ab, &c[i], 1);
                } else {
                    int color = syntaxToColor(hl[i]);
                    if (color != currentColor) {
                        currentColor = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[i], 1);
                }
            }

            abAppend(ab, "\x1b[39m", 5);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void eDrawStatusBar(abuf *ab) {
    abAppend(ab, "\x1b[48;5;235m", 11); //set background color x1b[48;5;[]m replace [] with the color
    abAppend(ab, "\x1b[38;5;245m", 11); //set foreground color x1b[38;5;[]m replace [] with the color

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), " %s %.20s%s", editorInfo.mode ? "EDIT" : "CMND",
                       editorInfo.filename ? editorInfo.filename : "[empty]", editorInfo.dirty ? "[+]" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d:%d", editorInfo.cy + 1, editorInfo.linecount, editorInfo.cx);

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
    int msglen = (int)strlen(editorInfo.statusmsg);

    if (msglen > editorInfo.w) {
        msglen = editorInfo.w;
    }

    if (msglen && time(NULL) - editorInfo.statusmsgTime < 5) {
        if (editorInfo.statuserror) {
            abAppend(ab, "\x1b[48;5;131m", 11); //set background color x1b[48;5;[]m replace [] with the color
            abAppend(ab, "\x1b[38;5;232m", 11); //set foreground color x1b[38;5;[]m replace [] with the color
        }

        abAppend(ab, editorInfo.statusmsg, msglen);
        abAppend(ab, "\x1b[m", 3);
    } else {
        editorInfo.statuserror = false;
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
    abAppend(&ab, buf, (int)strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);
    abAppend(&ab, "\x1b]1337;CursorShape=1\x07", 21); //set cursor to vertical bar, iTerm2 specific

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void eTick() {
    static int quitTimes = QUIT_TIMES;

    int k = eReadKey();

    switch (k) {
        case vk_escape: editorInfo.mode = !editorInfo.mode; break;
        case vk_enter: eInsertNewLine(); break;

        case CTRL_KEY('q'):{
            if (editorInfo.dirty && quitTimes > 0) {
                eSetError("No write since last change (%d more times to override)", quitTimes--);
                return;
            } else {
                quit();
            }
        } break;
        case CTRL_KEY('B'): quit(); break;
        case CTRL_KEY('s'): eSave(); break;
        case CTRL_KEY('f'): eFind(); break;

        case vk_home: editorInfo.cx = 0; break;
        case vk_end: {
            if (editorInfo.cy < editorInfo.linecount) {
                editorInfo.cx = editorInfo.line[editorInfo.cy].size;
            }
        } break;

        case vk_backspace:
        case CTRL_KEY('h'):
        case vk_delete: {
            if (k == vk_delete) {
                eMove(vk_right);
            }

            eDeleteChar();
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
        default: eInsertChar(k); break;
    }

    quitTimes = QUIT_TIMES;
}

void eInit() {
    editorInfo.cx = 0;
    editorInfo.cy = 0;
    editorInfo.rx = 0;
    editorInfo.yoffset = 0;
    editorInfo.xoffset = 0;
    editorInfo.linecount = 0;
    editorInfo.line = NULL;
    editorInfo.dirty = 0;
    editorInfo.filename = NULL;
    editorInfo.statusmsg[0] = '\0';
    editorInfo.statusmsgTime = 0;
    editorInfo.statuserror = false;
    editorInfo.syntax = NULL;
    editorInfo.mode = EDT;

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

    if (editorInfo.filename == NULL) {
        editorInfo.mode = CMD;
    }

    eSetStatus(welcomeMsg);

    while (true) {
        ecls();
        eTick();
    }

    return 0;
}
