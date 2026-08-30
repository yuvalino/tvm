/*
 * Minimal SFTP server for Dropbear.
 *
 * Dropbear implements the "sftp" subsystem request by exec'ing an external
 * sftp-server binary (SFTPSERVER_PATH); it contains no SFTP code of its own
 * and upstream expects OpenSSH's sftp-server to be present. TVM has no
 * OpenSSH to borrow that binary from, so this is a self-contained
 * implementation wired in as another dropbearmulti applet, "sftp-server".
 *
 * It matters because OpenSSH's scp(1) has used the SFTP subsystem rather than
 * the legacy rcp protocol since 9.0 -- without this, plain "scp" to a Dropbear
 * server fails and callers have to pass -O.
 *
 * Protocol: draft-ietf-secsh-filexfer-02 (version 3), which is what OpenSSH's
 * scp(1) and sftp(1) speak. Requests arrive on stdin, replies go to stdout.
 *
 * I/O is non-blocking and select() driven, mirroring OpenSSH's sftp-server:
 * a client may pipeline many requests, and replying to a large read while the
 * client is still writing requests would deadlock a naive blocking loop.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Request/response types */
#define SSH2_FXP_INIT			1
#define SSH2_FXP_VERSION		2
#define SSH2_FXP_OPEN			3
#define SSH2_FXP_CLOSE			4
#define SSH2_FXP_READ			5
#define SSH2_FXP_WRITE			6
#define SSH2_FXP_LSTAT			7
#define SSH2_FXP_FSTAT			8
#define SSH2_FXP_SETSTAT		9
#define SSH2_FXP_FSETSTAT		10
#define SSH2_FXP_OPENDIR		11
#define SSH2_FXP_READDIR		12
#define SSH2_FXP_REMOVE			13
#define SSH2_FXP_MKDIR			14
#define SSH2_FXP_RMDIR			15
#define SSH2_FXP_REALPATH		16
#define SSH2_FXP_STAT			17
#define SSH2_FXP_RENAME			18
#define SSH2_FXP_READLINK		19
#define SSH2_FXP_SYMLINK		20
#define SSH2_FXP_STATUS			101
#define SSH2_FXP_HANDLE			102
#define SSH2_FXP_DATA			103
#define SSH2_FXP_NAME			104
#define SSH2_FXP_ATTRS			105
#define SSH2_FXP_EXTENDED		200
#define SSH2_FXP_EXTENDED_REPLY		201

/* Status codes */
#define SSH2_FX_OK			0
#define SSH2_FX_EOF			1
#define SSH2_FX_NO_SUCH_FILE		2
#define SSH2_FX_PERMISSION_DENIED	3
#define SSH2_FX_FAILURE			4
#define SSH2_FX_BAD_MESSAGE		5
#define SSH2_FX_OP_UNSUPPORTED		8

/* Open flags */
#define SSH2_FXF_READ			0x00000001
#define SSH2_FXF_WRITE			0x00000002
#define SSH2_FXF_APPEND			0x00000004
#define SSH2_FXF_CREAT			0x00000008
#define SSH2_FXF_TRUNC			0x00000010
#define SSH2_FXF_EXCL			0x00000020

/* Attribute flags */
#define SSH2_FILEXFER_ATTR_SIZE		0x00000001
#define SSH2_FILEXFER_ATTR_UIDGID	0x00000002
#define SSH2_FILEXFER_ATTR_PERMISSIONS	0x00000004
#define SSH2_FILEXFER_ATTR_ACMODTIME	0x00000008
#define SSH2_FILEXFER_ATTR_EXTENDED	0x80000000

#define SSH2_FILEXFER_VERSION		3

/* Largest message we will send or accept, matching OpenSSH. */
#define SFTP_MAX_MSG_LENGTH		(256 * 1024)
/* Largest payload we hand back for a single read. */
#define SFTP_MAX_READ_LENGTH		(255 * 1024)
/* Entries per READDIR reply. */
#define SFTP_READDIR_BATCH		100

#define MAX_HANDLES			100

static void fatal(const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 1, 2), noreturn))
#endif
	;

static void fatal(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "sftp-server: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);

	if (p == NULL) {
		fatal("out of memory");
	}
	return p;
}

/* ---------------------------------------------------------------- buffers */

/* Append-at-tail, consume-at-head byte buffer. "off" is how much of the
 * front has already been consumed; buf_compact() reclaims it. */
struct buf {
	unsigned char *p;
	size_t alloc;
	size_t len;
	size_t off;
};

static void buf_free(struct buf *b) {
	free(b->p);
	memset(b, 0x0, sizeof(*b));
}

static void buf_grow(struct buf *b, size_t need) {
	size_t want = b->len + need;
	size_t nalloc;
	unsigned char *np;

	if (want <= b->alloc) {
		return;
	}
	nalloc = b->alloc ? b->alloc : 4096;
	while (nalloc < want) {
		nalloc *= 2;
	}
	np = realloc(b->p, nalloc);
	if (np == NULL) {
		fatal("out of memory");
	}
	b->p = np;
	b->alloc = nalloc;
}

static void buf_put(struct buf *b, const void *d, size_t n) {
	buf_grow(b, n);
	memcpy(b->p + b->len, d, n);
	b->len += n;
}

static void buf_put_u8(struct buf *b, unsigned int v) {
	unsigned char c = (unsigned char)v;

	buf_put(b, &c, 1);
}

static void buf_put_u32(struct buf *b, uint32_t v) {
	unsigned char d[4];

	d[0] = (unsigned char)(v >> 24);
	d[1] = (unsigned char)(v >> 16);
	d[2] = (unsigned char)(v >> 8);
	d[3] = (unsigned char)v;
	buf_put(b, d, 4);
}

static void buf_put_u64(struct buf *b, uint64_t v) {
	buf_put_u32(b, (uint32_t)(v >> 32));
	buf_put_u32(b, (uint32_t)(v & 0xffffffffU));
}

static void buf_put_string(struct buf *b, const void *s, size_t n) {
	buf_put_u32(b, (uint32_t)n);
	buf_put(b, s, n);
}

static void buf_put_cstring(struct buf *b, const char *s) {
	buf_put_string(b, s, strlen(s));
}

static size_t buf_avail(const struct buf *b) {
	return b->len - b->off;
}

/* Malformed input is a protocol violation we cannot recover from -- the
 * framing is lost -- so like OpenSSH we simply die. */
static const unsigned char *buf_take(struct buf *b, size_t n) {
	const unsigned char *p;

	if (buf_avail(b) < n) {
		fatal("truncated message");
	}
	p = b->p + b->off;
	b->off += n;
	return p;
}

static unsigned int buf_get_u8(struct buf *b) {
	return *buf_take(b, 1);
}

static uint32_t buf_get_u32(struct buf *b) {
	const unsigned char *d = buf_take(b, 4);

	return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
		((uint32_t)d[2] << 8) | (uint32_t)d[3];
}

static uint64_t buf_get_u64(struct buf *b) {
	uint64_t hi = buf_get_u32(b);

	return (hi << 32) | buf_get_u32(b);
}

static const unsigned char *buf_get_string(struct buf *b, size_t *lenp) {
	uint32_t n = buf_get_u32(b);

	if (n > SFTP_MAX_MSG_LENGTH) {
		fatal("string too long (%u)", (unsigned int)n);
	}
	*lenp = n;
	return buf_take(b, n);
}

/* Paths and names, as a NUL-terminated copy the caller frees. */
static char *buf_get_cstring(struct buf *b) {
	size_t n;
	const unsigned char *d = buf_get_string(b, &n);
	char *s = xmalloc(n + 1);

	memcpy(s, d, n);
	s[n] = '\0';
	return s;
}

/* Drop the consumed prefix so the buffer does not grow without bound. */
static void buf_compact(struct buf *b) {
	if (b->off == 0) {
		return;
	}
	if (b->off == b->len) {
		b->len = 0;
	} else {
		memmove(b->p, b->p + b->off, b->len - b->off);
		b->len -= b->off;
	}
	b->off = 0;
}

/* ---------------------------------------------------------------- handles */

enum handle_kind { HANDLE_UNUSED, HANDLE_FILE, HANDLE_DIR };

struct handle {
	enum handle_kind kind;
	int fd;
	DIR *dirp;
	char *name;
};

static struct handle handles[MAX_HANDLES];

static int handle_new(enum handle_kind kind, const char *name, int fd, DIR *dirp) {
	int i;

	for (i = 0; i < MAX_HANDLES; i++) {
		if (handles[i].kind == HANDLE_UNUSED) {
			handles[i].kind = kind;
			handles[i].fd = fd;
			handles[i].dirp = dirp;
			handles[i].name = strdup(name);
			if (handles[i].name == NULL) {
				fatal("out of memory");
			}
			return i;
		}
	}
	return -1;
}

/* Handles are opaque to the client; we use a 4-byte index. Consumes the
 * handle string from the message, so it must be called exactly once. */
static struct handle *handle_from_msg_any(struct buf *m) {
	size_t len;
	const unsigned char *d = buf_get_string(m, &len);
	uint32_t i;

	if (len != 4) {
		return NULL;
	}
	i = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
		((uint32_t)d[2] << 8) | (uint32_t)d[3];
	if (i >= MAX_HANDLES || handles[i].kind == HANDLE_UNUSED) {
		return NULL;
	}
	return &handles[i];
}

static struct handle *handle_from_msg(struct buf *m, enum handle_kind kind) {
	struct handle *h = handle_from_msg_any(m);

	return (h != NULL && h->kind == kind) ? h : NULL;
}

static void handle_put(struct buf *b, int i) {
	unsigned char d[4];

	d[0] = (unsigned char)((uint32_t)i >> 24);
	d[1] = (unsigned char)((uint32_t)i >> 16);
	d[2] = (unsigned char)((uint32_t)i >> 8);
	d[3] = (unsigned char)i;
	buf_put_string(b, d, sizeof(d));
}

static int handle_close(struct handle *h) {
	int ret = 0;
	int saved_errno = 0;

	if (h->kind == HANDLE_FILE) {
		ret = close(h->fd);
	} else if (h->kind == HANDLE_DIR) {
		ret = closedir(h->dirp);
	}
	/* Capture before free()/memset, which may clobber errno. */
	if (ret < 0) {
		saved_errno = errno;
	}
	free(h->name);
	memset(h, 0x0, sizeof(*h));
	h->kind = HANDLE_UNUSED;
	errno = saved_errno;
	return ret;
}

/* ------------------------------------------------------------- attributes */

struct sftp_attrs {
	uint32_t flags;
	uint64_t size;
	uint32_t uid, gid;
	uint32_t perm;
	uint32_t atime, mtime;
};

static void attrs_get(struct buf *m, struct sftp_attrs *a) {
	memset(a, 0x0, sizeof(*a));
	a->flags = buf_get_u32(m);
	if (a->flags & SSH2_FILEXFER_ATTR_SIZE) {
		a->size = buf_get_u64(m);
	}
	if (a->flags & SSH2_FILEXFER_ATTR_UIDGID) {
		a->uid = buf_get_u32(m);
		a->gid = buf_get_u32(m);
	}
	if (a->flags & SSH2_FILEXFER_ATTR_PERMISSIONS) {
		a->perm = buf_get_u32(m);
	}
	if (a->flags & SSH2_FILEXFER_ATTR_ACMODTIME) {
		a->atime = buf_get_u32(m);
		a->mtime = buf_get_u32(m);
	}
	if (a->flags & SSH2_FILEXFER_ATTR_EXTENDED) {
		uint32_t count = buf_get_u32(m);
		uint32_t i;
		size_t len;

		for (i = 0; i < count; i++) {
			(void)buf_get_string(m, &len);
			(void)buf_get_string(m, &len);
		}
	}
}

static void attrs_put_stat(struct buf *b, const struct stat *st) {
	buf_put_u32(b, SSH2_FILEXFER_ATTR_SIZE | SSH2_FILEXFER_ATTR_UIDGID |
		SSH2_FILEXFER_ATTR_PERMISSIONS | SSH2_FILEXFER_ATTR_ACMODTIME);
	buf_put_u64(b, (uint64_t)st->st_size);
	buf_put_u32(b, (uint32_t)st->st_uid);
	buf_put_u32(b, (uint32_t)st->st_gid);
	buf_put_u32(b, (uint32_t)st->st_mode);
	buf_put_u32(b, (uint32_t)st->st_atime);
	buf_put_u32(b, (uint32_t)st->st_mtime);
}

/* --------------------------------------------------------------- replies */

static struct buf iqueue;
static struct buf oqueue;

static void send_msg(struct buf *m) {
	buf_put_u32(&oqueue, (uint32_t)m->len);
	buf_put(&oqueue, m->p, m->len);
	buf_free(m);
}

static const char *status_message(uint32_t status) {
	switch (status) {
	case SSH2_FX_OK:
		return "Success";
	case SSH2_FX_EOF:
		return "End of file";
	case SSH2_FX_NO_SUCH_FILE:
		return "No such file";
	case SSH2_FX_PERMISSION_DENIED:
		return "Permission denied";
	case SSH2_FX_BAD_MESSAGE:
		return "Bad message";
	case SSH2_FX_OP_UNSUPPORTED:
		return "Operation unsupported";
	default:
		return "Failure";
	}
}

static void send_status(uint32_t id, uint32_t status) {
	struct buf m;

	memset(&m, 0x0, sizeof(m));
	buf_put_u8(&m, SSH2_FXP_STATUS);
	buf_put_u32(&m, id);
	buf_put_u32(&m, status);
	buf_put_cstring(&m, status_message(status));
	buf_put_cstring(&m, "");
	send_msg(&m);
}

static uint32_t errno_to_portable(int e) {
	switch (e) {
	case 0:
		return SSH2_FX_OK;
	case ENOENT:
	case ENOTDIR:
	case EBADF:
	case ELOOP:
		return SSH2_FX_NO_SUCH_FILE;
	case EPERM:
	case EACCES:
	case EFAULT:
		return SSH2_FX_PERMISSION_DENIED;
	case ENAMETOOLONG:
	case EINVAL:
		return SSH2_FX_BAD_MESSAGE;
	case ENOSYS:
		return SSH2_FX_OP_UNSUPPORTED;
	default:
		return SSH2_FX_FAILURE;
	}
}

static void send_status_errno(uint32_t id, int e) {
	send_status(id, errno_to_portable(e));
}

static void send_handle(uint32_t id, int handle) {
	struct buf m;

	memset(&m, 0x0, sizeof(m));
	buf_put_u8(&m, SSH2_FXP_HANDLE);
	buf_put_u32(&m, id);
	handle_put(&m, handle);
	send_msg(&m);
}

static void send_data(uint32_t id, const unsigned char *data, size_t len) {
	struct buf m;

	memset(&m, 0x0, sizeof(m));
	buf_put_u8(&m, SSH2_FXP_DATA);
	buf_put_u32(&m, id);
	buf_put_string(&m, data, len);
	send_msg(&m);
}

static void send_attrs(uint32_t id, const struct stat *st) {
	struct buf m;

	memset(&m, 0x0, sizeof(m));
	buf_put_u8(&m, SSH2_FXP_ATTRS);
	buf_put_u32(&m, id);
	attrs_put_stat(&m, st);
	send_msg(&m);
}

/* A NAME reply carrying a single entry, used by REALPATH and READLINK.
 * Neither caller has meaningful attributes to report, so they are empty. */
static void send_name_one(uint32_t id, const char *name) {
	struct buf m;

	memset(&m, 0x0, sizeof(m));
	buf_put_u8(&m, SSH2_FXP_NAME);
	buf_put_u32(&m, id);
	buf_put_u32(&m, 1);
	buf_put_cstring(&m, name);
	buf_put_cstring(&m, name);
	buf_put_u32(&m, 0);
	send_msg(&m);
}

/* ------------------------------------------------------------ path helper */

/* realpath(3) fails on a path whose last component does not exist, but
 * clients legitimately REALPATH an upload destination before creating it.
 * So resolve the deepest existing ancestor and re-attach the rest. */
static char *resolve_path(const char *path) {
	char abs[PATH_MAX];
	char resolved[PATH_MAX];
	char head[PATH_MAX];
	char tail[PATH_MAX];
	char *slash;

	if (path[0] == '\0') {
		path = ".";
	}
	if (path[0] == '/') {
		if ((size_t)snprintf(abs, sizeof(abs), "%s", path) >= sizeof(abs)) {
			errno = ENAMETOOLONG;
			return NULL;
		}
	} else {
		char cwd[PATH_MAX];

		if (getcwd(cwd, sizeof(cwd)) == NULL) {
			return NULL;
		}
		if ((size_t)snprintf(abs, sizeof(abs), "%s/%s", cwd, path) >= sizeof(abs)) {
			errno = ENAMETOOLONG;
			return NULL;
		}
	}

	if (realpath(abs, resolved) != NULL) {
		return strdup(resolved);
	}

	/* Split off the final component and resolve the parent. Copy the tail
	 * out first: it points into head, and truncating head below would
	 * otherwise blank it (e.g. "/foo" -> slash==head, head[1]='\0'). */
	if ((size_t)snprintf(head, sizeof(head), "%s", abs) >= sizeof(head)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	slash = strrchr(head, '/');
	if (slash == NULL) {
		return strdup(abs);
	}
	if ((size_t)snprintf(tail, sizeof(tail), "%s", slash + 1) >= sizeof(tail)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	if (slash == head) {
		head[1] = '\0';
	} else {
		*slash = '\0';
	}
	if (realpath(head, resolved) == NULL) {
		/* Parent is missing too; hand back the absolute form unchanged. */
		return strdup(abs);
	}
	if (tail[0] == '\0') {
		return strdup(resolved);
	}
	{
		char out[PATH_MAX];
		const char *sep = (resolved[0] != '\0' &&
			resolved[strlen(resolved) - 1] == '/') ? "" : "/";

		if ((size_t)snprintf(out, sizeof(out), "%s%s%s", resolved, sep, tail)
				>= sizeof(out)) {
			errno = ENAMETOOLONG;
			return NULL;
		}
		return strdup(out);
	}
}

/* --------------------------------------------------------- long name (ls) */

static void mode_to_string(mode_t mode, char *out) {
	const char *rwx = "rwxrwxrwx";
	int i;

	if (S_ISDIR(mode)) {
		out[0] = 'd';
	} else if (S_ISLNK(mode)) {
		out[0] = 'l';
	} else if (S_ISFIFO(mode)) {
		out[0] = 'p';
	} else if (S_ISCHR(mode)) {
		out[0] = 'c';
	} else if (S_ISBLK(mode)) {
		out[0] = 'b';
	} else if (S_ISSOCK(mode)) {
		out[0] = 's';
	} else {
		out[0] = '-';
	}
	for (i = 0; i < 9; i++) {
		out[i + 1] = (mode & (1 << (8 - i))) ? rwx[i] : '-';
	}
	if (mode & S_ISUID) {
		out[3] = (mode & S_IXUSR) ? 's' : 'S';
	}
	if (mode & S_ISGID) {
		out[6] = (mode & S_IXGRP) ? 's' : 'S';
	}
	if (mode & S_ISVTX) {
		out[9] = (mode & S_IXOTH) ? 't' : 'T';
	}
	out[10] = '\0';
}

/* "ls -l"-ish line; clients only display this, they never parse it. */
static void format_longname(char *out, size_t outlen, const char *name,
		const struct stat *st) {
	char modes[12];
	char timebuf[64];
	char ubuf[64], gbuf[64];
	struct passwd *pw;
	struct group *gr;
	struct tm *tm;
	time_t now, mtime;

	mode_to_string(st->st_mode, modes);

	pw = getpwuid(st->st_uid);
	if (pw != NULL) {
		snprintf(ubuf, sizeof(ubuf), "%s", pw->pw_name);
	} else {
		snprintf(ubuf, sizeof(ubuf), "%lu", (unsigned long)st->st_uid);
	}
	gr = getgrgid(st->st_gid);
	if (gr != NULL) {
		snprintf(gbuf, sizeof(gbuf), "%s", gr->gr_name);
	} else {
		snprintf(gbuf, sizeof(gbuf), "%lu", (unsigned long)st->st_gid);
	}

	mtime = st->st_mtime;
	now = time(NULL);
	tm = localtime(&mtime);
	if (tm == NULL) {
		snprintf(timebuf, sizeof(timebuf), "%s", "            ");
	} else if (now - mtime < 180 * 24 * 60 * 60 && mtime <= now) {
		strftime(timebuf, sizeof(timebuf), "%b %e %H:%M", tm);
	} else {
		strftime(timebuf, sizeof(timebuf), "%b %e  %Y", tm);
	}

	snprintf(out, outlen, "%s %3lu %-8s %-8s %8llu %s %s", modes,
		(unsigned long)st->st_nlink, ubuf, gbuf,
		(unsigned long long)st->st_size, timebuf, name);
}

/* ------------------------------------------------------------- operations */

static void process_init(struct buf *m) {
	struct buf r;
	uint32_t version = buf_get_u32(m);

	memset(&r, 0x0, sizeof(r));
	buf_put_u8(&r, SSH2_FXP_VERSION);
	buf_put_u32(&r, version < SSH2_FILEXFER_VERSION ?
		version : SSH2_FILEXFER_VERSION);
	/* Extensions we honour in process_extended(). */
	buf_put_cstring(&r, "posix-rename@openssh.com");
	buf_put_cstring(&r, "1");
	buf_put_cstring(&r, "hardlink@openssh.com");
	buf_put_cstring(&r, "1");
	buf_put_cstring(&r, "fsync@openssh.com");
	buf_put_cstring(&r, "1");
	buf_put_cstring(&r, "limits@openssh.com");
	buf_put_cstring(&r, "1");
	send_msg(&r);
}

static int open_flags_from_pflags(uint32_t pflags) {
	int flags = 0;

	if ((pflags & (SSH2_FXF_READ | SSH2_FXF_WRITE)) ==
			(SSH2_FXF_READ | SSH2_FXF_WRITE)) {
		flags = O_RDWR;
	} else if (pflags & SSH2_FXF_WRITE) {
		flags = O_WRONLY;
	} else {
		flags = O_RDONLY;
	}
	if (pflags & SSH2_FXF_APPEND) {
		flags |= O_APPEND;
	}
	if (pflags & SSH2_FXF_CREAT) {
		flags |= O_CREAT;
	}
	if (pflags & SSH2_FXF_TRUNC) {
		flags |= O_TRUNC;
	}
	if (pflags & SSH2_FXF_EXCL) {
		flags |= O_EXCL;
	}
	return flags;
}

static void process_open(struct buf *m, uint32_t id) {
	char *name = buf_get_cstring(m);
	uint32_t pflags = buf_get_u32(m);
	struct sftp_attrs a;
	mode_t mode;
	int flags, fd, handle;

	attrs_get(m, &a);
	mode = (a.flags & SSH2_FILEXFER_ATTR_PERMISSIONS) ?
		(mode_t)(a.perm & 0777) : 0666;
	flags = open_flags_from_pflags(pflags);

	fd = open(name, flags, mode);
	if (fd < 0) {
		send_status_errno(id, errno);
	} else {
		handle = handle_new(HANDLE_FILE, name, fd, NULL);
		if (handle < 0) {
			close(fd);
			send_status(id, SSH2_FX_FAILURE);
		} else {
			send_handle(id, handle);
		}
	}
	free(name);
}

static void process_close(struct buf *m, uint32_t id) {
	struct handle *h = handle_from_msg_any(m);

	if (h == NULL) {
		send_status(id, SSH2_FX_FAILURE);
		return;
	}
	send_status_errno(id, handle_close(h) < 0 ? errno : 0);
}

static void process_read(struct buf *m, uint32_t id) {
	struct handle *h = handle_from_msg(m, HANDLE_FILE);
	uint64_t off;
	uint32_t len;
	unsigned char *data;
	ssize_t n;

	if (h == NULL) {
		send_status(id, SSH2_FX_FAILURE);
		return;
	}
	off = buf_get_u64(m);
	len = buf_get_u32(m);
	if (len > SFTP_MAX_READ_LENGTH) {
		len = SFTP_MAX_READ_LENGTH;
	}
	if (lseek(h->fd, (off_t)off, SEEK_SET) < 0) {
		send_status_errno(id, errno);
		return;
	}
	data = xmalloc(len);
	n = read(h->fd, data, len);
	if (n < 0) {
		send_status_errno(id, errno);
	} else if (n == 0) {
		send_status(id, SSH2_FX_EOF);
	} else {
		send_data(id, data, (size_t)n);
	}
	free(data);
}

static void process_write(struct buf *m, uint32_t id) {
	struct handle *h = handle_from_msg(m, HANDLE_FILE);
	uint64_t off;
	const unsigned char *data;
	size_t len, done = 0;

	if (h == NULL) {
		send_status(id, SSH2_FX_FAILURE);
		return;
	}
	off = buf_get_u64(m);
	data = buf_get_string(m, &len);
	if (lseek(h->fd, (off_t)off, SEEK_SET) < 0) {
		send_status_errno(id, errno);
		return;
	}
	while (done < len) {
		ssize_t n = write(h->fd, data + done, len - done);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			send_status_errno(id, errno);
			return;
		}
		done += (size_t)n;
	}
	send_status(id, SSH2_FX_OK);
}

static void process_stat(struct buf *m, uint32_t id, int follow) {
	char *name = buf_get_cstring(m);
	struct stat st;
	int ret = follow ? stat(name, &st) : lstat(name, &st);

	if (ret < 0) {
		send_status_errno(id, errno);
	} else {
		send_attrs(id, &st);
	}
	free(name);
}

static void process_fstat(struct buf *m, uint32_t id) {
	struct handle *h = handle_from_msg(m, HANDLE_FILE);
	struct stat st;

	if (h == NULL) {
		send_status(id, SSH2_FX_FAILURE);
		return;
	}
	if (fstat(h->fd, &st) < 0) {
		send_status_errno(id, errno);
	} else {
		send_attrs(id, &st);
	}
}

/* Times are set through the path in both SETSTAT and FSETSTAT: futimes() is
 * not universally available, and every handle keeps its name anyway. */
static int apply_times(const char *name, const struct sftp_attrs *a) {
	struct timeval tv[2];

	tv[0].tv_sec = (time_t)a->atime;
	tv[0].tv_usec = 0;
	tv[1].tv_sec = (time_t)a->mtime;
	tv[1].tv_usec = 0;
	return utimes(name, tv);
}

static void process_setstat(struct buf *m, uint32_t id) {
	char *name = buf_get_cstring(m);
	struct sftp_attrs a;
	uint32_t status = SSH2_FX_OK;

	attrs_get(m, &a);
	/* Every requested attribute is attempted (last error wins), and chown
	 * runs before chmod since chown() clears the setuid/setgid bits. */
	if (a.flags & SSH2_FILEXFER_ATTR_SIZE) {
		if (truncate(name, (off_t)a.size) < 0) {
			status = errno_to_portable(errno);
		}
	}
	if (a.flags & SSH2_FILEXFER_ATTR_UIDGID) {
		if (chown(name, (uid_t)a.uid, (gid_t)a.gid) < 0) {
			status = errno_to_portable(errno);
		}
	}
	if (a.flags & SSH2_FILEXFER_ATTR_PERMISSIONS) {
		if (chmod(name, (mode_t)(a.perm & 07777)) < 0) {
			status = errno_to_portable(errno);
		}
	}
	if (a.flags & SSH2_FILEXFER_ATTR_ACMODTIME) {
		if (apply_times(name, &a) < 0) {
			status = errno_to_portable(errno);
		}
	}
	send_status(id, status);
	free(name);
}

static void process_fsetstat(struct buf *m, uint32_t id) {
	struct handle *h = handle_from_msg(m, HANDLE_FILE);
	struct sftp_attrs a;
	uint32_t status = SSH2_FX_OK;

	if (h == NULL) {
		send_status(id, SSH2_FX_FAILURE);
		return;
	}
	attrs_get(m, &a);
	/* As in process_setstat: attempt all, fchown before fchmod. */
	if (a.flags & SSH2_FILEXFER_ATTR_SIZE) {
		if (ftruncate(h->fd, (off_t)a.size) < 0) {
			status = errno_to_portable(errno);
		}
	}
	if (a.flags & SSH2_FILEXFER_ATTR_UIDGID) {
		if (fchown(h->fd, (uid_t)a.uid, (gid_t)a.gid) < 0) {
			status = errno_to_portable(errno);
		}
	}
	if (a.flags & SSH2_FILEXFER_ATTR_PERMISSIONS) {
		if (fchmod(h->fd, (mode_t)(a.perm & 07777)) < 0) {
			status = errno_to_portable(errno);
		}
	}
	if (a.flags & SSH2_FILEXFER_ATTR_ACMODTIME) {
		if (apply_times(h->name, &a) < 0) {
			status = errno_to_portable(errno);
		}
	}
	send_status(id, status);
}

static void process_opendir(struct buf *m, uint32_t id) {
	char *name = buf_get_cstring(m);
	DIR *dirp = opendir(name);
	int handle;

	if (dirp == NULL) {
		send_status_errno(id, errno);
	} else {
		handle = handle_new(HANDLE_DIR, name, -1, dirp);
		if (handle < 0) {
			closedir(dirp);
			send_status(id, SSH2_FX_FAILURE);
		} else {
			send_handle(id, handle);
		}
	}
	free(name);
}

static void process_readdir(struct buf *m, uint32_t id) {
	struct handle *h = handle_from_msg(m, HANDLE_DIR);
	struct dirent *de;
	struct buf r;
	uint32_t count = 0;
	char path[PATH_MAX];
	char longname[PATH_MAX + 128];

	if (h == NULL) {
		send_status(id, SSH2_FX_FAILURE);
		return;
	}

	memset(&r, 0x0, sizeof(r));
	/* readdir() returns NULL for both end-of-directory and error; the two
	 * are told apart by errno, so clear it first. */
	errno = 0;
	while (count < SFTP_READDIR_BATCH) {
		struct stat st;

		de = readdir(h->dirp);
		if (de == NULL) {
			break;
		}
		/* An entry we can't stat (name too long for a full path, or lstat
		 * failure) is still reported, with zeroed attributes -- dropping it
		 * would make an empty batch look like end-of-directory. */
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", h->name,
				de->d_name) >= sizeof(path) || lstat(path, &st) < 0) {
			memset(&st, 0x0, sizeof(st));
		}
		format_longname(longname, sizeof(longname), de->d_name, &st);
		buf_put_cstring(&r, de->d_name);
		buf_put_cstring(&r, longname);
		attrs_put_stat(&r, &st);
		count++;
	}

	if (count == 0) {
		int e = errno;

		buf_free(&r);
		if (e != 0) {
			send_status_errno(id, e);
		} else {
			send_status(id, SSH2_FX_EOF);
		}
		return;
	}

	{
		struct buf out;

		memset(&out, 0x0, sizeof(out));
		buf_put_u8(&out, SSH2_FXP_NAME);
		buf_put_u32(&out, id);
		buf_put_u32(&out, count);
		buf_put(&out, r.p, r.len);
		buf_free(&r);
		send_msg(&out);
	}
}

static void process_remove(struct buf *m, uint32_t id) {
	char *name = buf_get_cstring(m);

	send_status_errno(id, unlink(name) < 0 ? errno : 0);
	free(name);
}

static void process_mkdir(struct buf *m, uint32_t id) {
	char *name = buf_get_cstring(m);
	struct sftp_attrs a;
	mode_t mode;

	attrs_get(m, &a);
	mode = (a.flags & SSH2_FILEXFER_ATTR_PERMISSIONS) ?
		(mode_t)(a.perm & 0777) : 0777;
	send_status_errno(id, mkdir(name, mode) < 0 ? errno : 0);
	free(name);
}

static void process_rmdir(struct buf *m, uint32_t id) {
	char *name = buf_get_cstring(m);

	send_status_errno(id, rmdir(name) < 0 ? errno : 0);
	free(name);
}

static void process_realpath(struct buf *m, uint32_t id) {
	char *name = buf_get_cstring(m);
	char *resolved;

	errno = 0;
	resolved = resolve_path(name);
	if (resolved == NULL) {
		send_status_errno(id, errno ? errno : ENOENT);
	} else {
		send_name_one(id, resolved);
		free(resolved);
	}
	free(name);
}

/* v3 RENAME must not clobber an existing target. */
static void process_rename(struct buf *m, uint32_t id) {
	char *oldpath = buf_get_cstring(m);
	char *newpath = buf_get_cstring(m);
	struct stat st;

	if (lstat(newpath, &st) == 0) {
		send_status(id, SSH2_FX_FAILURE);
	} else {
		send_status_errno(id, rename(oldpath, newpath) < 0 ? errno : 0);
	}
	free(oldpath);
	free(newpath);
}

static void process_readlink(struct buf *m, uint32_t id) {
	char *name = buf_get_cstring(m);
	char buf[PATH_MAX];
	ssize_t n = readlink(name, buf, sizeof(buf) - 1);

	if (n < 0) {
		send_status_errno(id, errno);
	} else {
		buf[n] = '\0';
		send_name_one(id, buf);
	}
	free(name);
}

/* Argument order follows OpenSSH's server, which the clients match: the
 * link target comes first, the new link name second. */
static void process_symlink(struct buf *m, uint32_t id) {
	char *oldpath = buf_get_cstring(m);
	char *newpath = buf_get_cstring(m);

	send_status_errno(id, symlink(oldpath, newpath) < 0 ? errno : 0);
	free(oldpath);
	free(newpath);
}

static void process_extended(struct buf *m, uint32_t id) {
	char *request = buf_get_cstring(m);

	if (strcmp(request, "posix-rename@openssh.com") == 0) {
		char *oldpath = buf_get_cstring(m);
		char *newpath = buf_get_cstring(m);

		send_status_errno(id, rename(oldpath, newpath) < 0 ? errno : 0);
		free(oldpath);
		free(newpath);
	} else if (strcmp(request, "hardlink@openssh.com") == 0) {
		char *oldpath = buf_get_cstring(m);
		char *newpath = buf_get_cstring(m);

		send_status_errno(id, link(oldpath, newpath) < 0 ? errno : 0);
		free(oldpath);
		free(newpath);
	} else if (strcmp(request, "fsync@openssh.com") == 0) {
		struct handle *h = handle_from_msg(m, HANDLE_FILE);

		if (h == NULL) {
			send_status(id, SSH2_FX_FAILURE);
		} else {
			send_status_errno(id, fsync(h->fd) < 0 ? errno : 0);
		}
	} else if (strcmp(request, "limits@openssh.com") == 0) {
		struct buf r;

		memset(&r, 0x0, sizeof(r));
		buf_put_u8(&r, SSH2_FXP_EXTENDED_REPLY);
		buf_put_u32(&r, id);
		buf_put_u64(&r, SFTP_MAX_MSG_LENGTH);
		buf_put_u64(&r, SFTP_MAX_READ_LENGTH);
		buf_put_u64(&r, SFTP_MAX_READ_LENGTH);
		/* max-open-handles: the table size, not the current free count. */
		buf_put_u64(&r, MAX_HANDLES);
		send_msg(&r);
	} else {
		send_status(id, SSH2_FX_OP_UNSUPPORTED);
	}
	free(request);
}

/* ------------------------------------------------------------- main loop */

static void process_message(struct buf *m) {
	unsigned int type = buf_get_u8(m);
	uint32_t id;

	if (type == SSH2_FXP_INIT) {
		process_init(m);
		return;
	}

	id = buf_get_u32(m);
	switch (type) {
	case SSH2_FXP_OPEN:
		process_open(m, id);
		break;
	case SSH2_FXP_CLOSE:
		process_close(m, id);
		break;
	case SSH2_FXP_READ:
		process_read(m, id);
		break;
	case SSH2_FXP_WRITE:
		process_write(m, id);
		break;
	case SSH2_FXP_LSTAT:
		process_stat(m, id, 0);
		break;
	case SSH2_FXP_FSTAT:
		process_fstat(m, id);
		break;
	case SSH2_FXP_SETSTAT:
		process_setstat(m, id);
		break;
	case SSH2_FXP_FSETSTAT:
		process_fsetstat(m, id);
		break;
	case SSH2_FXP_OPENDIR:
		process_opendir(m, id);
		break;
	case SSH2_FXP_READDIR:
		process_readdir(m, id);
		break;
	case SSH2_FXP_REMOVE:
		process_remove(m, id);
		break;
	case SSH2_FXP_MKDIR:
		process_mkdir(m, id);
		break;
	case SSH2_FXP_RMDIR:
		process_rmdir(m, id);
		break;
	case SSH2_FXP_REALPATH:
		process_realpath(m, id);
		break;
	case SSH2_FXP_STAT:
		process_stat(m, id, 1);
		break;
	case SSH2_FXP_RENAME:
		process_rename(m, id);
		break;
	case SSH2_FXP_READLINK:
		process_readlink(m, id);
		break;
	case SSH2_FXP_SYMLINK:
		process_symlink(m, id);
		break;
	case SSH2_FXP_EXTENDED:
		process_extended(m, id);
		break;
	default:
		send_status(id, SSH2_FX_OP_UNSUPPORTED);
		break;
	}
}

/* Pull complete messages off the input queue. Stops once the output queue
 * holds a full message's worth, so a slow reader applies backpressure
 * instead of letting oqueue grow without bound. */
static void process_input(void) {
	while (buf_avail(&oqueue) < SFTP_MAX_MSG_LENGTH) {
		uint32_t msglen;
		struct buf m;

		if (buf_avail(&iqueue) < 4) {
			break;
		}
		msglen = ((uint32_t)iqueue.p[iqueue.off] << 24) |
			((uint32_t)iqueue.p[iqueue.off + 1] << 16) |
			((uint32_t)iqueue.p[iqueue.off + 2] << 8) |
			(uint32_t)iqueue.p[iqueue.off + 3];
		if (msglen == 0 || msglen > SFTP_MAX_MSG_LENGTH) {
			fatal("bad message length %u", (unsigned int)msglen);
		}
		if (buf_avail(&iqueue) < msglen + 4) {
			break;
		}
		iqueue.off += 4;

		/* A view onto the message body; process_* consume from it. */
		memset(&m, 0x0, sizeof(m));
		m.p = iqueue.p + iqueue.off;
		m.len = msglen;
		process_message(&m);

		iqueue.off += msglen;
		buf_compact(&iqueue);
	}
}

static void set_nonblock(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		fatal("fcntl: %s", strerror(errno));
	}
}

int main(int argc, char **argv) {
	int in = STDIN_FILENO;
	int out = STDOUT_FILENO;
	/* static: TVM runs exec'd programs on threads, whose stacks are far
	 * smaller than a process's. */
	static unsigned char rbuf[64 * 1024];
	fd_set rset, wset;
	int maxfd;
	int eof = 0;

	(void)argc;
	(void)argv;

	/* A client that vanishes mid-transfer must not take us down with a
	 * signal before we can report the error. */
	signal(SIGPIPE, SIG_IGN);

	set_nonblock(in);
	set_nonblock(out);

	memset(&iqueue, 0x0, sizeof(iqueue));
	memset(&oqueue, 0x0, sizeof(oqueue));

	maxfd = (in > out ? in : out) + 1;

	for (;;) {
		ssize_t n;

		FD_ZERO(&rset);
		FD_ZERO(&wset);

		/* Read more only when there is room for a full message *and its
		 * 4-byte length prefix* -- gating at SFTP_MAX_MSG_LENGTH alone would
		 * stop reading with a max-size message still 4 bytes short of
		 * complete, and with oqueue empty select() would then block forever
		 * on an empty fd set. Stop reading entirely once the client has sent
		 * EOF. */
		if (!eof && buf_avail(&iqueue) < SFTP_MAX_MSG_LENGTH + 4 &&
				buf_avail(&oqueue) < SFTP_MAX_MSG_LENGTH) {
			FD_SET(in, &rset);
		}
		if (buf_avail(&oqueue) > 0) {
			FD_SET(out, &wset);
		}

		/* After EOF, exit only once every assembled reply has been flushed;
		 * process_input() below has already drained iqueue into oqueue. */
		if (eof && buf_avail(&oqueue) == 0) {
			break;
		}

		if (select(maxfd, &rset, &wset, NULL, NULL) < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			}
			fatal("select: %s", strerror(errno));
		}

		if (FD_ISSET(in, &rset)) {
			n = read(in, rbuf, sizeof(rbuf));
			if (n < 0) {
				if (errno != EINTR && errno != EAGAIN) {
					fatal("read: %s", strerror(errno));
				}
			} else if (n == 0) {
				/* Client half-closed; stop reading but keep replying to
				 * requests already received. */
				eof = 1;
			} else {
				buf_put(&iqueue, rbuf, (size_t)n);
			}
		}

		if (FD_ISSET(out, &wset)) {
			n = write(out, oqueue.p + oqueue.off, buf_avail(&oqueue));
			if (n < 0) {
				if (errno != EINTR && errno != EAGAIN) {
					fatal("write: %s", strerror(errno));
				}
			} else {
				oqueue.off += (size_t)n;
				buf_compact(&oqueue);
			}
		}

		process_input();
	}

	buf_free(&iqueue);
	buf_free(&oqueue);
	return 0;
}
