/* Kilo -- A very simple editor in less than 1-kilo lines of code (as counted
 *         by "cloc"). Does not depend on libcurses, directly emits VT100
 *         escapes on the terminal.
 *
 * -----------------------------------------------------------------------
 *
 * Copyright (C) 2016 Salvatore Sanfilippo <antirez at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define KILO_VERSION "0.0.1"

#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>

#include <iostream>
#include <exception>
#include <vector>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <SDL.h>
#include <SDL_ttf.h>
using namespace std;
using namespace std::chrono;

class Exception : public exception {
	string msg;
public:
	Exception(string amsg) : msg(amsg) {}
	const char* what() const noexcept {
		return msg.c_str();
	}
};

// SDL Colors
const SDL_Color CYAN    = {0x00, 0xFF, 0xFF};
const SDL_Color YELLOW  = {0xFF, 0xFF, 0x00};
const SDL_Color GREEN   = {0x00, 0xFF, 0x00};
const SDL_Color MAGENTA = {0xFF, 0x00, 0xFF};
const SDL_Color RED     = {0xFF, 0x00, 0x00};
const SDL_Color BLUE    = {0x00, 0x00, 0xFF};
const SDL_Color WHITE   = {0xFF, 0xFF, 0xFF};
const SDL_Color BLACK   = {0x00, 0x00, 0x00};

// Main application class
class App {
	const int DEFAULT_WINDOW_WIDTH = 640;
	const int DEFAULT_WINDOW_HEIGHT = 480;
	SDL_Window *window = NULL;
	SDL_Renderer *renderer = NULL;
	SDL_Event event;
	bool running = true;
    int &argc;
    char **&argv;
	TTF_Font *font;
	int font_width, font_height;
public:
	App(int &_argc, char **&_argv);
	~App();
	void init_sdl();
	void init();
	void run();
	void finish();
	void update(float dt);
	void draw();
	void draw_text(int x, int y, string s, SDL_Color c = WHITE);
	void draw_text(int x, int y, char ch, SDL_Color c = WHITE) {
        string s(1, ch);
        draw_text(x, y, s, c);
    }
	void on_event();
    void getWindowSize(int &ww, int &wh);
    void getFontSize(int &fw, int &fh);
};

void App::getWindowSize(int &ww, int &wh) {
    SDL_GetWindowSize(window, &ww, &wh);
}

void App::getFontSize(int &fw, int &fh) {
    fw = font_width;
    fh = font_height;
}

void App::draw_text(int x, int y, string s, SDL_Color c) {
    if (s.length() == 0) return;
    SDL_Surface *surface;
    //surface = TTF_RenderUTF8_Shaded(font, lines[0].c_str(), WHITE, {255, 0, 0});
    surface = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    //surface = TTF_RenderUTF8_Solid(font, s.c_str(), c);
    if (surface == NULL) {
        throw Exception(TTF_GetError());
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == NULL) {
        throw Exception(TTF_GetError());
    }
    SDL_Rect dst = {x, y, s.length() * font_width, font_height};
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

class KiloEditor {
    const int HL_NORMAL = 0;
};

#define TAB_SIZE 4
#define TAB '\t'

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2   /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8      /* Search match. */

#define HL_HIGHLIGHT_STRINGS (1<<0)
#define HL_HIGHLIGHT_NUMBERS (1<<1)

struct editorSyntax {
    char **filematch;
    char **keywords;
    char singleline_comment_start[3];
    char multiline_comment_start[3];
    char multiline_comment_end[3];
    int flags;
};

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
    int idx;            /* Row index in the file, zero-based. */
    int size;           /* Size of the row, excluding the null term. */
    int rsize;          /* Size of the rendered row. */
    char *chars;        /* Row content. */
    char *render;       /* Row content "rendered" for screen (for TABs). */
    unsigned char *hl;  /* Syntax highlight type for each character in render.*/
    int hl_oc;          /* Row had open comment at end in last syntax highlight
                           check. */
} erow;

typedef struct hlcolor {
    int r,g,b;
} hlcolor;

struct editorConfig {
    int cx,cy;  /* Cursor x and y position in characters */
    int rowoff;     /* Offset of row displayed. */
    int coloff;     /* Offset of column displayed. */
    int screenrows; /* Number of rows that we can show */
    int screencols; /* Number of cols that we can show */
    int numrows;    /* Number of rows */
    int rawmode;    /* Is terminal raw mode enabled? */
    erow *row;      /* Rows */
    int dirty;      /* File modified but not saved. */
    char *filename; /* Currently open filename */
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;    /* Current syntax highlight, or NULL. */
};

static struct editorConfig E;

void editorSetStatusMessage(const char *fmt, ...);

/* =========================== Syntax highlights DB =========================
 *
 * In order to add a new syntax, define two arrays with a list of file name
 * matches and keywords. The file name matches are used in order to match
 * a given syntax with a given file name: if a match pattern starts with a
 * dot, it is matched as the last past of the filename, for example ".c".
 * Otherwise the pattern is just searched inside the filenme, like "Makefile").
 *
 * The list of keywords to highlight is just a list of words, however if they
 * a trailing '|' character is added at the end, they are highlighted in
 * a different color, so that you can have two different sets of keywords.
 *
 * Finally add a stanza in the HLDB global variable with two two arrays
 * of strings, and a set of flags in order to enable highlighting of
 * comments and numbers.
 *
 * The characters for single and multi line comments must be exactly two
 * and must be provided as well (see the C language example).
 *
 * There is no support to highlight patterns currently. */

/* C / C++ */
char *C_HL_extensions[] = {".c",".cpp",NULL};
char *C_HL_keywords[] = {
        /* A few C / C++ keywords */
        "switch","if","while","for","break","continue","return","else",
        "struct","union","typedef","static","enum","class",
        /* C types */
        "int|","long|","double|","float|","char|","unsigned|","signed|",
        "void|",NULL
};

/* Here we define an array of syntax highlights by extensions, keywords,
 * comments delimiters and flags. */
editorSyntax HLDB[] = {
    {
        /* C / C++ */
        C_HL_extensions,
        C_HL_keywords,
        "//","/*","*/",
        HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS
    }
};

#define HLDB_ENTRIES (sizeof(HLDB)/sizeof(HLDB[0]))

/* ====================== Syntax highlight color scheme  ==================== */

int is_separator(int c) {
    return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];",c) != NULL;
}

/* Return true if the specified row last char is part of a multi line comment
 * that starts at this row or at one before, and does not end at the end
 * of the row but spawns to the next row. */
int editorRowHasOpenComment(erow *row) {
    if (row->hl && row->rsize && row->hl[row->rsize-1] == HL_MLCOMMENT &&
        (row->rsize < 2 || (row->render[row->rsize-2] != '*' ||
                            row->render[row->rsize-1] != '/'))) return 1;
    return 0;
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). */
void editorUpdateSyntax(erow *row) {
    row->hl = (unsigned char*) realloc(row->hl,row->rsize);
    memset(row->hl,HL_NORMAL,row->rsize);

    if (E.syntax == NULL) return; /* No syntax, everything is HL_NORMAL. */

    int i, prev_sep, in_string, in_comment;
    char *p;
    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    /* Point to the first non-space char. */
    p = row->render;
    i = 0; /* Current char offset */
    while(*p && isspace(*p)) {
        p++;
        i++;
    }
    prev_sep = 1; /* Tell the parser if 'i' points to start of word. */
    in_string = 0; /* Are we inside "" or '' ? */
    in_comment = 0; /* Are we inside multi-line comment? */

    /* If the previous line has an open comment, this line starts
     * with an open comment state. */
    if (row->idx > 0 && editorRowHasOpenComment(&E.row[row->idx-1]))
        in_comment = 1;

    while(*p) {
        /* Handle // comments. */
        if (prev_sep && *p == scs[0] && *(p+1) == scs[1]) {
            /* From here to end is a comment */
            memset(row->hl+i,HL_COMMENT,row->size-i);
            return;
        }

        /* Handle multi line comments. */
        if (in_comment) {
            row->hl[i] = HL_MLCOMMENT;
            if (*p == mce[0] && *(p+1) == mce[1]) {
                row->hl[i+1] = HL_MLCOMMENT;
                p += 2; i += 2;
                in_comment = 0;
                prev_sep = 1;
                continue;
            } else {
                prev_sep = 0;
                p++; i++;
                continue;
            }
        } else if (*p == mcs[0] && *(p+1) == mcs[1]) {
            row->hl[i] = HL_MLCOMMENT;
            row->hl[i+1] = HL_MLCOMMENT;
            p += 2; i += 2;
            in_comment = 1;
            prev_sep = 0;
            continue;
        }

        /* Handle "" and '' */
        if (in_string) {
            row->hl[i] = HL_STRING;
            if (*p == '\\') {
                row->hl[i+1] = HL_STRING;
                p += 2; i += 2;
                prev_sep = 0;
                continue;
            }
            if (*p == in_string) in_string = 0;
            p++; i++;
            continue;
        } else {
            if (*p == '"' || *p == '\'') {
                in_string = *p;
                row->hl[i] = HL_STRING;
                p++; i++;
                prev_sep = 0;
                continue;
            }
        }

        /* Handle non printable chars. */
        if (!isprint(*p)) {
            row->hl[i] = HL_NONPRINT;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        /* Handle numbers */
        if ((isdigit(*p) && (prev_sep || row->hl[i-1] == HL_NUMBER)) ||
            (*p == '.' && i >0 && row->hl[i-1] == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        /* Handle keywords and lib calls */
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen-1] == '|';
                if (kw2) klen--;

                if (!memcmp(p,keywords[j],klen) &&
                    is_separator(*(p+klen)))
                {
                    /* Keyword */
                    memset(row->hl+i,kw2 ? HL_KEYWORD2 : HL_KEYWORD1,klen);
                    p += klen;
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue; /* We had a keyword match */
            }
        }

        /* Not special chars */
        prev_sep = is_separator(*p);
        p++; i++;
    }

    /* Propagate syntax change to the next row if the open commen
     * state changed. This may recursively affect all the following rows
     * in the file. */
    int oc = editorRowHasOpenComment(row);
    if (row->hl_oc != oc && row->idx+1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx+1]);
    row->hl_oc = oc;
}

/* Maps syntax highlight token types to SDL colors. */
SDL_Color editorSyntaxToColor(int hl) {
    switch(hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return CYAN;
    case HL_KEYWORD1:  return YELLOW;
    case HL_KEYWORD2:  return GREEN;
    case HL_STRING:    return MAGENTA;
    case HL_NUMBER:    return RED;
    case HL_MATCH:     return BLUE;
    default:           return WHITE;
    }
}

/* Select the syntax highlight scheme depending on the filename,
 * setting it in the global state E.syntax. */
void editorSelectSyntaxHighlight(char *filename) {
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = HLDB+j;
        unsigned int i = 0;
        while(s->filematch[i]) {
            char *p;
            int patlen = strlen(s->filematch[i]);
            if ((p = strstr(filename,s->filematch[i])) != NULL) {
                if (s->filematch[i][0] != '.' || p[patlen] == '\0') {
                    E.syntax = s;
                    return;
                }
            }
            i++;
        }
    }
}

/* ======================= Editor rows implementation ======================= */

/* Update the rendered version and the syntax highlight of a row. */
void editorUpdateRow(erow *row) {
    int tabs = 0, nonprint = 0, j, idx;

   /* Create a version of the row we can directly print on the screen,
     * respecting tabs, substituting non printable characters with '?'. */
    free(row->render);
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == TAB) tabs++;

    row->render = (char*) malloc(row->size + tabs*TAB_SIZE + nonprint*9 + 1);
    idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) {
            row->render[idx++] = ' ';
            while((idx+1) % TAB_SIZE != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->rsize = idx;
    row->render[idx] = '\0';

    /* Update the syntax highlighting attributes of the row. */
    editorUpdateSyntax(row);
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(int at, char *s, size_t len) {
    if (at > E.numrows) return;
    E.row = (erow*) realloc(E.row,sizeof(erow)*(E.numrows+1));
    if (at != E.numrows) {
        memmove(E.row+at+1,E.row+at,sizeof(E.row[0])*(E.numrows-at));
        for (int j = at+1; j <= E.numrows; j++) E.row[j].idx++;
    }
    E.row[at].size = len;
    E.row[at].chars = (char*) malloc(len+1);
    memcpy(E.row[at].chars,s,len+1);
    E.row[at].hl = NULL;
    E.row[at].hl_oc = 0;
    E.row[at].render = NULL;
    E.row[at].rsize = 0;
    E.row[at].idx = at;
    editorUpdateRow(E.row+at);
    E.numrows++;
    E.dirty++;
}

/* Free row's heap allocated stuff. */
void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

/* Remove the row at the specified position, shifting the remainign on the
 * top. */
void editorDelRow(int at) {
    erow *row;

    if (at >= E.numrows) return;
    row = E.row+at;
    editorFreeRow(row);
    memmove(E.row+at,E.row+at+1,sizeof(E.row[0])*(E.numrows-at-1));
    for (int j = at; j < E.numrows-1; j++) E.row[j].idx++;
    E.numrows--;
    E.dirty++;
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, escluding
 * the final nulterm. */
char *editorRowsToString(int *buflen) {
    char *buf = NULL, *p;
    int totlen = 0;
    int j;

    /* Compute count of bytes */
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size+1; /* +1 is for "\n" at end of every row */
    *buflen = totlen;
    totlen++; /* Also make space for nulterm */

    p = buf = (char*) malloc(totlen);
    for (j = 0; j < E.numrows; j++) {
        memcpy(p,E.row[j].chars,E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    *p = '\0';
    return buf;
}

/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editorRowInsertChar(erow *row, int at, int c) {
    if (at > row->size) {
        /* Pad the string with spaces if the insert location is outside the
         * current length by more than a single character. */
        int padlen = at-row->size;
        /* In the next line +2 means: new char and null term. */
        row->chars = (char*) realloc(row->chars,row->size+padlen+2);
        memset(row->chars+row->size,' ',padlen);
        row->chars[row->size+padlen+1] = '\0';
        row->size += padlen+1;
    } else {
        /* If we are in the middle of the string just make space for 1 new
         * char plus the (already existing) null term. */
        row->chars = (char*) realloc(row->chars,row->size+2);
        memmove(row->chars+at+1,row->chars+at,row->size-at+1);
        row->size++;
    }
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = (char*) realloc(row->chars,row->size+len+1);
    memcpy(row->chars+row->size,s,len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editorRowDelChar(erow *row, int at) {
    if (row->size <= at) return;
    memmove(row->chars+at,row->chars+at+1,row->size-at);
    editorUpdateRow(row);
    row->size--;
    E.dirty++;
}

/* Insert the specified char at the current prompt position. */
void editorInsertChar(int c) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    /* If the row where the cursor is currently located does not exist in our
     * logical representaion of the file, add enough empty rows as needed. */
    if (!row) {
        while(E.numrows <= filerow)
            editorInsertRow(E.numrows,"",0);
    }
    row = &E.row[filerow];
    editorRowInsertChar(row,filecol,c);
    if (E.cx == E.screencols-1)
        E.coloff++;
    else
        E.cx++;
    E.dirty++;
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed. */
void editorInsertNewline(void) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row) {
        if (filerow == E.numrows) {
            editorInsertRow(filerow,"",0);
            goto fixcursor;
        }
        return;
    }
    /* If the cursor is over the current line size, we want to conceptually
     * think it's just over the last character. */
    if (filecol >= row->size) filecol = row->size;
    if (filecol == 0) {
        editorInsertRow(filerow,"",0);
    } else {
        /* We are in the middle of a line. Split it between two rows. */
        editorInsertRow(filerow+1,row->chars+filecol,row->size-filecol);
        row = &E.row[filerow];
        row->chars[filecol] = '\0';
        row->size = filecol;
        editorUpdateRow(row);
    }
fixcursor:
    if (E.cy == E.screenrows-1) {
        E.rowoff++;
    } else {
        E.cy++;
    }
    E.cx = 0;
    E.coloff = 0;
}

/* Delete the char at the current prompt position. */
void editorDelChar() {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row || (filecol == 0 && filerow == 0)) return;
    if (filecol == 0) {
        /* Handle the case of column 0, we need to move the current line
         * on the right of the previous one. */
        filecol = E.row[filerow-1].size;
        editorRowAppendString(&E.row[filerow-1],row->chars,row->size);
        editorDelRow(filerow);
        row = NULL;
        if (E.cy == 0)
            E.rowoff--;
        else
            E.cy--;
        E.cx = filecol;
        if (E.cx >= E.screencols) {
            int shift = (E.screencols-E.cx)+1;
            E.cx -= shift;
            E.coloff += shift;
        }
    } else {
        editorRowDelChar(row,filecol-1);
        if (E.cx == 0 && E.coloff)
            E.coloff--;
        else
            E.cx--;
    }
    if (row) editorUpdateRow(row);
    E.dirty++;
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
    FILE *fp;

    E.dirty = 0;
    free(E.filename);
    E.filename = strdup(filename);

    fp = fopen(filename,"r");
    if (!fp) {
        if (errno != ENOENT) {
            throw Exception("Opening file");
        }
        return 1;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line,&linecap,fp)) != -1) {
        if (linelen && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            line[--linelen] = '\0';
        editorInsertRow(E.numrows,line,linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
    return 0;
}

/* Save the current file on disk. Return 0 on success, 1 on error. */
int editorSave(void) {
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename,O_RDWR|O_CREAT,0644);
    if (fd == -1) goto writeerr;

    /* Use truncate + a single write(2) call in order to make saving
     * a bit safer, under the limits of what we can do in a small editor. */
    if (ftruncate(fd,len) == -1) goto writeerr;
    if (write(fd,buf,len) != len) goto writeerr;

    close(fd);
    free(buf);
    E.dirty = 0;
    editorSetStatusMessage("%d bytes written on disk", len);
    return 0;

writeerr:
    free(buf);
    if (fd != -1) close(fd);
    editorSetStatusMessage("Can't save! I/O error: %s",strerror(errno));
    return 1;
}

/* ============================= Terminal update ============================ */

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(App &app) {
    char buf[32];
    
    int fw, fh, ww, wh;
    app.getWindowSize(ww, wh);
    app.getFontSize(fw, fh);

    for (int y = 0; y < E.screenrows; y++) {
        int filerow = E.rowoff+y;

        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows/3) {
                char welcome[80];
                int welcomelen = snprintf(
                    welcome, sizeof(welcome),
                    "Kilo editor -- verison %s", KILO_VERSION
                );
                int padding = (E.screencols - welcomelen) / 2;
                string t = "";
                if (padding > 0) {
                    t += "~";
                    padding--;
                }
                while (padding > 0) {
                    t += " ";
                    padding--;
                }
                t += welcome;
                app.draw_text(0, y*fh, t);
            } else {
                app.draw_text(0, y*fh, "~");
            }
            continue;
        }

        erow *r = &E.row[filerow];

        int len = r->rsize - E.coloff;
        SDL_Color color = WHITE;
        if (len > 0) {
            if (len > E.screencols) len = E.screencols;
            char *c = r->render + E.coloff;
            unsigned char *hl = r->hl + E.coloff;
            for (int j = 0; j < len; j++) {
                int cx = j*fw, cy = y*fh;
                if (hl[j] == HL_NONPRINT) {
                    app.draw_text(cx, cy, "?", color);
                } else if (hl[j] == HL_NORMAL) {
                    color = WHITE;
                    app.draw_text(cx, cy, *(c+j), color);
                } else {
                    color = editorSyntaxToColor(hl[j]);
                    app.draw_text(cx, cy, *(c+j), color);
                }
            }
        }
    }

    /* Create a two rows status. First row: */
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename, E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus),
        "%d/%d",E.rowoff+E.cy+1,E.numrows);
    int y = E.screenrows;
    for(int j=0; j<min(len, E.screencols); j++) {
        app.draw_text(fw*j, fh*y, status[j]);
    }
    int jj = max(0, E.screencols - rlen);
    for(int j=0; j<min(rlen, E.screencols); j++, jj++) {
        app.draw_text(fw*jj, fh*y, rstatus[j]);
    }

    /* Second row depends on E.statusmsg and the status message update time. */
    int msglen = strlen(E.statusmsg);
    if (msglen && time(NULL)-E.statusmsg_time < 5) {
        for(int j=0; j<min(msglen, E.screencols); j++) {
            app.draw_text(fw*j, fh*(y + 1), E.statusmsg[j]);
        }
    }
}

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* =============================== Find mode ================================ */

#define KILO_QUERY_LEN 256

void editorFind() {
    //char query[KILO_QUERY_LEN+1] = {0};
    //int qlen = 0;
    //int last_match = -1; /* Last line where a match was found. -1 for none. */
    //int find_next = 0; /* if 1 search next, if -1 search prev. */
    //int saved_hl_line = -1;  /* No saved HL */
    //char *saved_hl = NULL;

//#define FIND_RESTORE_HL do { \
    //if (saved_hl) { \
        //memcpy(E.row[saved_hl_line].hl,saved_hl, E.row[saved_hl_line].rsize); \
        //saved_hl = NULL; \
    //} \
//} while (0)

    ///* Save the cursor position in order to restore it later. */
    //int saved_cx = E.cx, saved_cy = E.cy;
    //int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

    //while(1) {
        //editorSetStatusMessage(
            //"Search: %s (Use ESC/Arrows/Enter)", query);
        //editorRefreshScreen();

        //int c = editorReadKey(fd);
        //if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
            //if (qlen != 0) query[--qlen] = '\0';
            //last_match = -1;
        //} else if (c == ESC || c == ENTER) {
            //if (c == ESC) {
                //E.cx = saved_cx; E.cy = saved_cy;
                //E.coloff = saved_coloff; E.rowoff = saved_rowoff;
            //}
            //FIND_RESTORE_HL;
            //editorSetStatusMessage("");
            //return;
        //} else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
            //find_next = 1;
        //} else if (c == ARROW_LEFT || c == ARROW_UP) {
            //find_next = -1;
        //} else if (isprint(c)) {
            //if (qlen < KILO_QUERY_LEN) {
                //query[qlen++] = c;
                //query[qlen] = '\0';
                //last_match = -1;
            //}
        //}

        ///* Search occurrence. */
        //if (last_match == -1) find_next = 1;
        //if (find_next) {
            //char *match = NULL;
            //int match_offset = 0;
            //int i, current = last_match;

            //for (i = 0; i < E.numrows; i++) {
                //current += find_next;
                //if (current == -1) current = E.numrows-1;
                //else if (current == E.numrows) current = 0;
                //match = strstr(E.row[current].render,query);
                //if (match) {
                    //match_offset = match-E.row[current].render;
                    //break;
                //}
            //}
            //find_next = 0;

            ///* Highlight */
            //FIND_RESTORE_HL;

            //if (match) {
                //erow *row = &E.row[current];
                //last_match = current;
                //if (row->hl) {
                    //saved_hl_line = current;
                    //saved_hl = (char*) malloc(row->rsize);
                    //memcpy(saved_hl,row->hl,row->rsize);
                    //memset(row->hl+match_offset,HL_MATCH,qlen);
                //}
                //E.cy = 0;
                //E.cx = match_offset;
                //E.rowoff = current;
                //E.coloff = 0;
                ///* Scroll horizontally as needed. */
                //if (E.cx > E.screencols) {
                    //int diff = E.cx - E.screencols;
                    //E.cx -= diff;
                    //E.coloff += diff;
                //}
            //}
        //}
    //}
}

/* ========================= Editor events handling  ======================== */

/* Handle cursor position change because arrow keys were pressed. */
void editorMoveCursor(SDL_Keycode key) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    int rowlen;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    switch(key) {
    case SDLK_LEFT:
        if (E.cx == 0) {
            if (E.coloff) {
                E.coloff--;
            } else {
                if (filerow > 0) {
                    E.cy--;
                    E.cx = E.row[filerow-1].size;
                    if (E.cx > E.screencols-1) {
                        E.coloff = E.cx-E.screencols+1;
                        E.cx = E.screencols-1;
                    }
                }
            }
        } else {
            E.cx -= 1;
        }
        break;
    case SDLK_RIGHT:
        if (row && filecol < row->size) {
            if (E.cx == E.screencols-1) {
                E.coloff++;
            } else {
                E.cx += 1;
            }
        } else if (row && filecol == row->size) {
            E.cx = 0;
            E.coloff = 0;
            if (E.cy == E.screenrows-1) {
                E.rowoff++;
            } else {
                E.cy += 1;
            }
        }
        break;
    case SDLK_UP:
        if (E.cy == 0) {
            if (E.rowoff) E.rowoff--;
        } else {
            E.cy -= 1;
        }
        break;
    case SDLK_DOWN:
        if (filerow < E.numrows) {
            if (E.cy == E.screenrows-1) {
                E.rowoff++;
            } else {
                E.cy += 1;
            }
        }
        break;
    }
    /* Fix cx if the current line has not enough chars. */
    filerow = E.rowoff+E.cy;
    filecol = E.coloff+E.cx;
    row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E.cx -= filecol-rowlen;
        if (E.cx < 0) {
            E.coloff += E.cx;
            E.cx = 0;
        }
    }
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define KILO_QUIT_TIMES 3

void editorProcessKeypress(App &app, SDL_Event &event) {
    /* When the file is modified, requires Ctrl-q to be pressed N times
     * before actually quitting. */
    static int quit_times = KILO_QUIT_TIMES;
    
    SDL_Keycode key = event.key.keysym.sym;
    bool is_ctrl = event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL);
    
    if (is_ctrl) {
        switch(key) {
        case SDLK_c:         /* Ctrl-c */
            break;
        case SDLK_q:         /* Ctrl-q */
            /* Quit if the file was already saved. */
            if (E.dirty && quit_times) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                    "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            app.finish();
            return;
        case SDLK_s:         /* Ctrl-s */
            editorSave();
            break;
        case SDLK_f:
            editorFind();
            break;
        case SDLK_h:         /* Ctrl-h */
            editorDelChar();
            break;
        case SDLK_l:         /* ctrl+l, clear screen */
            /* Just refresht the line as side effect. */
            break;
        }
    } else {
        switch(key) {
        case SDLK_RETURN:         /* Enter */
            editorInsertNewline();
            break;
        case SDLK_BACKSPACE:     /* Backspace */
        case SDLK_DELETE:
            editorDelChar();
            break;
        case SDLK_PAGEUP:
        case SDLK_PAGEDOWN:
            if (key == SDLK_PAGEUP && E.cy != 0)
                E.cy = 0;
            else if (key == SDLK_PAGEDOWN && E.cy != E.screenrows-1)
                E.cy = E.screenrows-1;
            {
            int times = E.screenrows;
            while(times--)
                editorMoveCursor(key == SDLK_PAGEUP ? SDLK_UP
                                                    : SDLK_DOWN);
            }
            break;
        case SDLK_UP:
        case SDLK_DOWN:
        case SDLK_LEFT:
        case SDLK_RIGHT:
            editorMoveCursor(key);
            break;
        case SDLK_ESCAPE:
            /* Nothing to do for ESC in this mode. */
            break;
        default:
            //editorInsertChar(key);
            break;
        }
    }

    quit_times = KILO_QUIT_TIMES; /* Reset it to the original value. */
}

int editorFileWasModified(void) {
    return E.dirty;
}

void initEditor(App &app) {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.syntax = NULL;
    int fw, fh, ww, wh;
    app.getWindowSize(ww, wh);
    app.getFontSize(fw, fh);
    E.screenrows = wh / fh;
    E.screencols = ww / fw;
    E.screenrows -= 2; /* Get room for status bar. */
}

App::App(int &_argc, char **&_argv) : argc(_argc), argv(_argv) {}

App::~App() {
	if (window) {
		SDL_DestroyWindow(window);
	}
	if (renderer) {
		SDL_DestroyRenderer(renderer);
	}
	if (font) {
		TTF_CloseFont(font);
	}
	TTF_Quit();
	SDL_Quit();
}

void App::init_sdl() {
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		throw Exception(SDL_GetError());
	}
	if (TTF_Init() < 0) {
		throw Exception(TTF_GetError());
	}
	
	window = SDL_CreateWindow(
		"kilo (SDL clone)", 
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT,
		SDL_WINDOW_SHOWN
	);
	if (window == NULL) {
		throw Exception(SDL_GetError());
	}
	
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (renderer == NULL) {
		throw Exception(SDL_GetError());
	}
    
	font = TTF_OpenFont("SourceCodePro-Medium.ttf", 16);
	if (font == NULL) {
		throw Exception(TTF_GetError());
	}
	if (TTF_SizeUTF8(font, "W", &font_width, &font_height) < 0) {
		throw Exception(TTF_GetError());
	}
}

void App::init() {
    if (argc != 2) {
        throw Exception("Usage: kilo <filename>");
    }
	init_sdl();
    SDL_StartTextInput();
    initEditor(*this);
    editorSelectSyntaxHighlight(argv[1]);
    editorOpen(argv[1]);
    editorSetStatusMessage(
        "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
}

void App::finish() {
	running = false;
}

void App::on_event() {
	switch (event.type) {
		case SDL_QUIT:
			finish();
			break;
		case SDL_KEYDOWN:
            editorProcessKeypress(*this, event);
			break;
		case SDL_TEXTINPUT:
			// cout << "event.text.text: '" << event.text.text << "'" << endl;
            editorInsertChar(*event.text.text);
			break;
	}
}

void App::run() {
	auto t1 = high_resolution_clock::now();
	while (running) {
		while (SDL_PollEvent(&event)) {
			on_event();
		}
		auto t2 = high_resolution_clock::now();
		auto dt = duration_cast<microseconds>(t2 - t1).count();
		t1 = t2;
		update((float)dt / 1e6f);
		draw();
	}
}

void App::update(float dt /* sec. */) {
}

void App::draw() {
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
    editorRefreshScreen(*this);
    SDL_Rect cursor_rect = {
        E.cx * font_width, E.cy * font_height,
        font_width, font_height
    };
	SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(renderer, &cursor_rect);
	SDL_RenderPresent(renderer);
}

int main(int argc, char **argv) {
    App app(argc, argv);
    try {
        app.init();
        app.run();
    } catch(const exception &e) {
        cout << e.what() << endl;
        return 1;
    } catch(...) {
        cout << "Unknow error." << endl;
        return 2;
    }
    return 0;
}
