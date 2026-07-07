/* Stub termcap implementations for iOS — TVM intercepts tputs at runtime */
#include <string.h>

int tgetent(char *bp, const char *name) { (void)bp; (void)name; return 1; }
int tgetflag(const char *id) { (void)id; return 0; }
int tgetnum(const char *id) { (void)id; return -1; }
char *tgetstr(const char *id, char **area) { (void)id; (void)area; return 0; }
char *tgoto(const char *cap, int col, int row) { (void)cap; (void)col; (void)row; return ""; }
int tputs(const char *str, int affcnt, int (*putc_fn)(int)) { (void)str; (void)affcnt; (void)putc_fn; return 0; }
