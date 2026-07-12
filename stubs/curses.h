/* iOS stub for curses.h — TVM intercepts tputs at runtime */
#ifndef _IOS_CURSES_STUB_H
#define _IOS_CURSES_STUB_H

#define OK 0
#define ERR (-1)

int tgetent(char *bp, const char *name);
int tgetflag(const char *id);
int tgetnum(const char *id);
char *tgetstr(const char *id, char **area);
char *tgoto(const char *cap, int col, int row);
int tputs(const char *str, int affcnt, int (*putc_fn)(int));

#endif
