#ifndef DROPBEAR_LOCALOPTIONS_H_
#define DROPBEAR_LOCALOPTIONS_H_

#define DROPBEAR_DEFPORT "2222"
#define INETD_MODE 0
#define DROPBEAR_REEXEC 0
#define DO_MOTD 0

#define RSA_PRIV_FILENAME "~/.dropbear/dropbear_rsa_host_key"
#define ECDSA_PRIV_FILENAME "~/.dropbear/dropbear_ecdsa_host_key"
#define ED25519_PRIV_FILENAME "~/.dropbear/dropbear_ed25519_host_key"

#define DROPBEAR_EMBEDDED_PASSWORD "alpine"
#define DROPBEAR_PASSWORD_ENVNAME_ENABLED 1

#define DROPBEAR_MULTI 1

/* Our own sftp-server applet, symlinked into the initrd next to the other
 * dropbearmulti names. Without this the subsystem request execs upstream's
 * default /usr/libexec/sftp-server, which does not exist under TVM -- and
 * OpenSSH scp(1) has spoken SFTP rather than the legacy protocol since 9.0,
 * so plain "scp" would need -O. */
#define SFTPSERVER_PATH "/tvm/bin/sftp-server"

#define DROPBEAR_FORKLESS 1

#define DEBUG_TRACE 6


#endif /* DROPBEAR_LOCALOPTIONS_H_ */
