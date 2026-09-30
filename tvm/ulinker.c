#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ulinker.h"


static void (*print_cb)(char *, int) = NULL;
#define PRINT0(_msg, _len) do { \
    if (print_cb) { print_cb(_msg, _len); } \
    else { write(1, _msg, _len); } \
} while (0)

#define PRINT(MSG, ...) do { \
    char __msgbuf[4096]; \
    int __len = snprintf(__msgbuf, sizeof(__msgbuf), "%s:%d %s: " MSG "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    PRINT0(__msgbuf, __len); \
} while (0)

// not thread-safe (same as POSIX dlerror)
static char errormsg[4096] = {0};
#define ERROR(_msg, ...) do { snprintf(errormsg, sizeof(errormsg), (_msg), ##__VA_ARGS__); PRINT("%s", errormsg); } while (0)

static void (*event_cb)(int, void *);

static int noemu = 0; // deprecated — kept for ULINKER_NOEMU compat

#define ULINKER_EVENT(_event, _arg) do { if (event_cb) event_cb((_event), (void *)(_arg)); } while (0)

#define ULINKER_EVENT_LOAD(_event, _dl) do {    \
    struct ulinker_load_event __ULE = {         \
        .ule_private = DLPRIVATE(_dl),          \
        .ule_base = DLBASE(_dl)->dl_base,       \
        .ule_size = DLBASE(_dl)->dl_size};      \
    ULINKER_EVENT((_event), &__ULE);            \
} while (0)

static size_t page_size() {
    return sysconf(_SC_PAGE_SIZE);
}

static uintptr_t page_start(uintptr_t x) {
    return x & ~(page_size() - 1);
}

// Returns the offset of address 'x' in its page.
static uintptr_t page_offset(uintptr_t x) {
    return x & (page_size() - 1);
}

// Returns the address of the next page after address 'x', unless 'x' is
// itself at the start of a page.
static uintptr_t page_end(uintptr_t x) {
    return page_start(x + page_size() - 1);
}

/* -------------------- ELF -------------------- */

/* Type for a 16-bit quantity.  */
typedef uint16_t Elf32_Half;
typedef uint16_t Elf64_Half;

/* Types for signed and unsigned 32-bit quantities.  */
typedef uint32_t Elf32_Word;
typedef	int32_t  Elf32_Sword;
typedef uint32_t Elf64_Word;
typedef	int32_t  Elf64_Sword;

/* Types for signed and unsigned 64-bit quantities.  */
typedef uint64_t Elf32_Xword;
typedef	int64_t  Elf32_Sxword;
typedef uint64_t Elf64_Xword;
typedef	int64_t  Elf64_Sxword;

/* Type of addresses.  */
typedef uint32_t Elf32_Addr;
typedef uint64_t Elf64_Addr;

/* Type of file offsets.  */
typedef uint32_t Elf32_Off;
typedef uint64_t Elf64_Off;

/* Type for section indices, which are 16-bit quantities.  */
typedef uint16_t Elf32_Section;
typedef uint16_t Elf64_Section;

/* Type for version symbol information.  */
typedef Elf32_Half Elf32_Versym;
typedef Elf64_Half Elf64_Versym;

/* The ELF file header.  This appears at the start of every ELF file.  */

#define EI_NIDENT (16)

typedef struct
{
  unsigned char	e_ident[EI_NIDENT];	/* Magic number and other info */
  Elf32_Half	e_type;			/* Object file type */
  Elf32_Half	e_machine;		/* Architecture */
  Elf32_Word	e_version;		/* Object file version */
  Elf32_Addr	e_entry;		/* Entry point virtual address */
  Elf32_Off	e_phoff;		/* Program header table file offset */
  Elf32_Off	e_shoff;		/* Section header table file offset */
  Elf32_Word	e_flags;		/* Processor-specific flags */
  Elf32_Half	e_ehsize;		/* ELF header size in bytes */
  Elf32_Half	e_phentsize;		/* Program header table entry size */
  Elf32_Half	e_phnum;		/* Program header table entry count */
  Elf32_Half	e_shentsize;		/* Section header table entry size */
  Elf32_Half	e_shnum;		/* Section header table entry count */
  Elf32_Half	e_shstrndx;		/* Section header string table index */
} Elf32_Ehdr;

typedef struct
{
  unsigned char	e_ident[EI_NIDENT];	/* Magic number and other info */
  Elf64_Half	e_type;			/* Object file type */
  Elf64_Half	e_machine;		/* Architecture */
  Elf64_Word	e_version;		/* Object file version */
  Elf64_Addr	e_entry;		/* Entry point virtual address */
  Elf64_Off	e_phoff;		/* Program header table file offset */
  Elf64_Off	e_shoff;		/* Section header table file offset */
  Elf64_Word	e_flags;		/* Processor-specific flags */
  Elf64_Half	e_ehsize;		/* ELF header size in bytes */
  Elf64_Half	e_phentsize;		/* Program header table entry size */
  Elf64_Half	e_phnum;		/* Program header table entry count */
  Elf64_Half	e_shentsize;		/* Section header table entry size */
  Elf64_Half	e_shnum;		/* Section header table entry count */
  Elf64_Half	e_shstrndx;		/* Section header string table index */
} Elf64_Ehdr;

/* Fields in the e_ident array.  The EI_* macros are indices into the
   array.  The macros under each EI_* macro are the values the byte
   may have.  */

#define EI_MAG0		0		/* File identification byte 0 index */
#define ELFMAG0		0x7f		/* Magic number byte 0 */

#define EI_MAG1		1		/* File identification byte 1 index */
#define ELFMAG1		'E'		/* Magic number byte 1 */

#define EI_MAG2		2		/* File identification byte 2 index */
#define ELFMAG2		'L'		/* Magic number byte 2 */

#define EI_MAG3		3		/* File identification byte 3 index */
#define ELFMAG3		'F'		/* Magic number byte 3 */

/* Conglomeration of the identification bytes, for easy testing as a word.  */
#define	ELFMAG		"\177ELF"
#define	SELFMAG		4

#define EI_CLASS	4		/* File class byte index */
#define ELFCLASSNONE	0		/* Invalid class */
#define ELFCLASS32	1		/* 32-bit objects */
#define ELFCLASS64	2		/* 64-bit objects */
#define ELFCLASSNUM	3

#define EI_DATA		5		/* Data encoding byte index */
#define ELFDATANONE	0		/* Invalid data encoding */
#define ELFDATA2LSB	1		/* 2's complement, little endian */
#define ELFDATA2MSB	2		/* 2's complement, big endian */
#define ELFDATANUM	3

#define EI_VERSION	6		/* File version byte index */
					/* Value must be EV_CURRENT */

#define EI_OSABI	7		/* OS ABI identification */
#define ELFOSABI_NONE		0	/* UNIX System V ABI */
#define ELFOSABI_SYSV		0	/* Alias.  */
#define ELFOSABI_HPUX		1	/* HP-UX */
#define ELFOSABI_NETBSD		2	/* NetBSD.  */
#define ELFOSABI_GNU		3	/* Object uses GNU ELF extensions.  */
#define ELFOSABI_LINUX		ELFOSABI_GNU /* Compatibility alias.  */
#define ELFOSABI_SOLARIS	6	/* Sun Solaris.  */
#define ELFOSABI_AIX		7	/* IBM AIX.  */
#define ELFOSABI_IRIX		8	/* SGI Irix.  */
#define ELFOSABI_FREEBSD	9	/* FreeBSD.  */
#define ELFOSABI_TRU64		10	/* Compaq TRU64 UNIX.  */
#define ELFOSABI_MODESTO	11	/* Novell Modesto.  */
#define ELFOSABI_OPENBSD	12	/* OpenBSD.  */
#define ELFOSABI_ARM_AEABI	64	/* ARM EABI */
#define ELFOSABI_ARM		97	/* ARM */
#define ELFOSABI_STANDALONE	255	/* Standalone (embedded) application */

#define EI_ABIVERSION	8		/* ABI version */

#define EI_PAD		9		/* Byte index of padding bytes */

static const uint8_t elf_magic[4] = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3};

/* Legal values for e_type (object file type).  */

#define ET_NONE		0		/* No file type */
#define ET_REL		1		/* Relocatable file */
#define ET_EXEC		2		/* Executable file */
#define ET_DYN		3		/* Shared object file */
#define ET_CORE		4		/* Core file */
#define	ET_NUM		5		/* Number of defined types */
#define ET_LOOS		0xfe00		/* OS-specific range start */
#define ET_HIOS		0xfeff		/* OS-specific range end */
#define ET_LOPROC	0xff00		/* Processor-specific range start */
#define ET_HIPROC	0xffff		/* Processor-specific range end */

/* Legal values for e_machine (architecture).  */

#define EM_NONE		 0	/* No machine */
#define EM_X86_64	62	/* AMD x86-64 architecture */
#define EM_AARCH64	183	/* ARM AARCH64 */

/* Program segment header.  */

typedef struct
{
  Elf32_Word	p_type;			/* Segment type */
  Elf32_Off	p_offset;		/* Segment file offset */
  Elf32_Addr	p_vaddr;		/* Segment virtual address */
  Elf32_Addr	p_paddr;		/* Segment physical address */
  Elf32_Word	p_filesz;		/* Segment size in file */
  Elf32_Word	p_memsz;		/* Segment size in memory */
  Elf32_Word	p_flags;		/* Segment flags */
  Elf32_Word	p_align;		/* Segment alignment */
} Elf32_Phdr;

typedef struct
{
  Elf64_Word	p_type;			/* Segment type */
  Elf64_Word	p_flags;		/* Segment flags */
  Elf64_Off	p_offset;		/* Segment file offset */
  Elf64_Addr	p_vaddr;		/* Segment virtual address */
  Elf64_Addr	p_paddr;		/* Segment physical address */
  Elf64_Xword	p_filesz;		/* Segment size in file */
  Elf64_Xword	p_memsz;		/* Segment size in memory */
  Elf64_Xword	p_align;		/* Segment alignment */
} Elf64_Phdr;

/* Special value for e_phnum.  This indicates that the real number of
   program headers is too large to fit into e_phnum.  Instead the real
   value is in the field sh_info of section 0.  */

#define PN_XNUM		0xffff

/* Legal values for p_type (segment type).  */

#define	PT_NULL		0		/* Program header table entry unused */
#define PT_LOAD		1		/* Loadable program segment */
#define PT_DYNAMIC	2		/* Dynamic linking information */
#define PT_INTERP	3		/* Program interpreter */
#define PT_NOTE		4		/* Auxiliary information */
#define PT_SHLIB	5		/* Reserved */
#define PT_PHDR		6		/* Entry for header table itself */
#define PT_TLS		7		/* Thread-local storage segment */
#define	PT_NUM		8		/* Number of defined types */
#define PT_LOOS		0x60000000	/* Start of OS-specific */
#define PT_GNU_EH_FRAME	0x6474e550	/* GCC .eh_frame_hdr segment */
#define PT_GNU_STACK	0x6474e551	/* Indicates stack executability */
#define PT_GNU_RELRO	0x6474e552	/* Read-only after relocation */
#define PT_GNU_PROPERTY	0x6474e553	/* GNU property */
#define PT_LOSUNW	0x6ffffffa
#define PT_SUNWBSS	0x6ffffffa	/* Sun Specific segment */
#define PT_SUNWSTACK	0x6ffffffb	/* Stack segment */
#define PT_HISUNW	0x6fffffff
#define PT_HIOS		0x6fffffff	/* End of OS-specific */
#define PT_LOPROC	0x70000000	/* Start of processor-specific */
#define PT_HIPROC	0x7fffffff	/* End of processor-specific */

/* Legal values for p_flags (segment flags).  */

#define PF_X		(1 << 0)	/* Segment is executable */
#define PF_W		(1 << 1)	/* Segment is writable */
#define PF_R		(1 << 2)	/* Segment is readable */
#define PF_MASKOS	0x0ff00000	/* OS-specific */
#define PF_MASKPROC	0xf0000000	/* Processor-specific */

#define MAYBE_MAP_FLAG(x, from, to)  (((x) & (from)) ? (to) : 0)
#define PFLAGS_TO_PROT(x)            (MAYBE_MAP_FLAG((x), PF_R, PROT_READ) | \
                                      MAYBE_MAP_FLAG((x), PF_W, PROT_WRITE) | \
                                      MAYBE_MAP_FLAG((x), PF_X, PROT_EXEC))

/* Dynamic section entry.  */

typedef struct
{
  Elf32_Sword	d_tag;			/* Dynamic entry type */
  union
    {
      Elf32_Word d_val;			/* Integer value */
      Elf32_Addr d_ptr;			/* Address value */
    } d_un;
} Elf32_Dyn;

typedef struct
{
  Elf64_Sxword	d_tag;			/* Dynamic entry type */
  union
    {
      Elf64_Xword d_val;		/* Integer value */
      Elf64_Addr d_ptr;			/* Address value */
    } d_un;
} Elf64_Dyn;

/* Legal values for d_tag (dynamic entry type).  */

#define DT_NULL		0		/* Marks end of dynamic section */
#define DT_NEEDED	1		/* Name of needed library */
#define DT_PLTRELSZ	2		/* Size in bytes of PLT relocs */
#define DT_PLTGOT	3		/* Processor defined value */
#define DT_HASH		4		/* Address of symbol hash table */
#define DT_STRTAB	5		/* Address of string table */
#define DT_SYMTAB	6		/* Address of symbol table */
#define DT_RELA		7		/* Address of Rela relocs */
#define DT_RELASZ	8		/* Total size of Rela relocs */
#define DT_RELAENT	9		/* Size of one Rela reloc */
#define DT_STRSZ	10		/* Size of string table */
#define DT_SYMENT	11		/* Size of one symbol table entry */
#define DT_INIT		12		/* Address of init function */
#define DT_FINI		13		/* Address of termination function */
#define DT_SONAME	14		/* Name of shared object */
#define DT_RPATH	15		/* Library search path (deprecated) */
#define DT_SYMBOLIC	16		/* Start symbol search here */
#define DT_REL		17		/* Address of Rel relocs */
#define DT_RELSZ	18		/* Total size of Rel relocs */
#define DT_RELENT	19		/* Size of one Rel reloc */
#define DT_PLTREL	20		/* Type of reloc in PLT */
#define DT_DEBUG	21		/* For debugging; unspecified */
#define DT_TEXTREL	22		/* Reloc might modify .text */
#define DT_JMPREL	23		/* Address of PLT relocs */
#define	DT_BIND_NOW	24		/* Process relocations of object */
#define	DT_INIT_ARRAY	25		/* Array with addresses of init fct */
#define	DT_FINI_ARRAY	26		/* Array with addresses of fini fct */
#define	DT_INIT_ARRAYSZ	27		/* Size in bytes of DT_INIT_ARRAY */
#define	DT_FINI_ARRAYSZ	28		/* Size in bytes of DT_FINI_ARRAY */
#define DT_RUNPATH	29		/* Library search path */
#define DT_FLAGS	30		/* Flags for the object being loaded */
#define DT_ENCODING	32		/* Start of encoded range */
#define DT_PREINIT_ARRAY 32		/* Array with addresses of preinit fct*/
#define DT_PREINIT_ARRAYSZ 33		/* size in bytes of DT_PREINIT_ARRAY */
#define DT_SYMTAB_SHNDX	34		/* Address of SYMTAB_SHNDX section */
#define	DT_NUM		35		/* Number used */
#define DT_LOOS		0x6000000d	/* Start of OS-specific */
#define DT_HIOS		0x6ffff000	/* End of OS-specific */
#define DT_LOPROC	0x70000000	/* Start of processor-specific */
#define DT_HIPROC	0x7fffffff	/* End of processor-specific */
#define	DT_PROCNUM	DT_MIPS_NUM	/* Most used by any processor */

/* DT_* entries which fall between DT_VALRNGHI & DT_VALRNGLO use the
   Dyn.d_un.d_val field of the Elf*_Dyn structure.  This follows Sun's
   approach.  */
#define DT_VALRNGLO	0x6ffffd00
#define DT_GNU_PRELINKED 0x6ffffdf5	/* Prelinking timestamp */
#define DT_GNU_CONFLICTSZ 0x6ffffdf6	/* Size of conflict section */
#define DT_GNU_LIBLISTSZ 0x6ffffdf7	/* Size of library list */
#define DT_CHECKSUM	0x6ffffdf8
#define DT_PLTPADSZ	0x6ffffdf9
#define DT_MOVEENT	0x6ffffdfa
#define DT_MOVESZ	0x6ffffdfb
#define DT_FEATURE_1	0x6ffffdfc	/* Feature selection (DTF_*).  */
#define DT_POSFLAG_1	0x6ffffdfd	/* Flags for DT_* entries, effecting
					   the following DT_* entry.  */
#define DT_SYMINSZ	0x6ffffdfe	/* Size of syminfo table (in bytes) */
#define DT_SYMINENT	0x6ffffdff	/* Entry size of syminfo */
#define DT_VALRNGHI	0x6ffffdff
#define DT_VALTAGIDX(tag)	(DT_VALRNGHI - (tag))	/* Reverse order! */
#define DT_VALNUM 12

/* DT_* entries which fall between DT_ADDRRNGHI & DT_ADDRRNGLO use the
   Dyn.d_un.d_ptr field of the Elf*_Dyn structure.

   If any adjustment is made to the ELF object after it has been
   built these entries will need to be adjusted.  */
#define DT_ADDRRNGLO	0x6ffffe00
#define DT_GNU_HASH	0x6ffffef5	/* GNU-style hash table.  */
#define DT_TLSDESC_PLT	0x6ffffef6
#define DT_TLSDESC_GOT	0x6ffffef7
#define DT_GNU_CONFLICT	0x6ffffef8	/* Start of conflict section */
#define DT_GNU_LIBLIST	0x6ffffef9	/* Library list */
#define DT_CONFIG	0x6ffffefa	/* Configuration information.  */
#define DT_DEPAUDIT	0x6ffffefb	/* Dependency auditing.  */
#define DT_AUDIT	0x6ffffefc	/* Object auditing.  */
#define	DT_PLTPAD	0x6ffffefd	/* PLT padding.  */
#define	DT_MOVETAB	0x6ffffefe	/* Move table.  */
#define DT_SYMINFO	0x6ffffeff	/* Syminfo table.  */
#define DT_ADDRRNGHI	0x6ffffeff
#define DT_ADDRTAGIDX(tag)	(DT_ADDRRNGHI - (tag))	/* Reverse order! */
#define DT_ADDRNUM 11

/* The versioning entry types.  The next are defined as part of the
   GNU extension.  */
#define DT_VERSYM	0x6ffffff0

#define DT_RELACOUNT	0x6ffffff9
#define DT_RELCOUNT	0x6ffffffa

/* These were chosen by Sun.  */
#define DT_FLAGS_1	0x6ffffffb	/* State flags, see DF_1_* below.  */
#define	DT_VERDEF	0x6ffffffc	/* Address of version definition
					   table */
#define	DT_VERDEFNUM	0x6ffffffd	/* Number of version definitions */
#define	DT_VERNEED	0x6ffffffe	/* Address of table with needed
					   versions */
#define	DT_VERNEEDNUM	0x6fffffff	/* Number of needed versions */
#define DT_VERSIONTAGIDX(tag)	(DT_VERNEEDNUM - (tag))	/* Reverse order! */
#define DT_VERSIONTAGNUM 16

/* Sun added these machine-independent extensions in the "processor-specific"
   range.  Be compatible.  */
#define DT_AUXILIARY    0x7ffffffd      /* Shared object to load before self */
#define DT_FILTER       0x7fffffff      /* Shared object to get values from */
#define DT_EXTRATAGIDX(tag)	((Elf32_Word)-((Elf32_Sword) (tag) <<1>>1)-1)
#define DT_EXTRANUM	3

/* Values of `d_un.d_val' in the DT_FLAGS entry.  */
#define DF_ORIGIN	0x00000001	/* Object may use DF_ORIGIN */
#define DF_SYMBOLIC	0x00000002	/* Symbol resolutions starts here */
#define DF_TEXTREL	0x00000004	/* Object contains text relocations */
#define DF_BIND_NOW	0x00000008	/* No lazy binding for this object */
#define DF_STATIC_TLS	0x00000010	/* Module uses the static TLS model */

/* State flags selectable in the `d_un.d_val' element of the DT_FLAGS_1
   entry in the dynamic section.  */
#define DF_1_NOW	0x00000001	/* Set RTLD_NOW for this object.  */
#define DF_1_GLOBAL	0x00000002	/* Set RTLD_GLOBAL for this object.  */
#define DF_1_GROUP	0x00000004	/* Set RTLD_GROUP for this object.  */
#define DF_1_NODELETE	0x00000008	/* Set RTLD_NODELETE for this object.*/
#define DF_1_LOADFLTR	0x00000010	/* Trigger filtee loading at runtime.*/
#define DF_1_INITFIRST	0x00000020	/* Set RTLD_INITFIRST for this object*/
#define DF_1_NOOPEN	0x00000040	/* Set RTLD_NOOPEN for this object.  */
#define DF_1_ORIGIN	0x00000080	/* $ORIGIN must be handled.  */
#define DF_1_DIRECT	0x00000100	/* Direct binding enabled.  */
#define DF_1_TRANS	0x00000200
#define DF_1_INTERPOSE	0x00000400	/* Object is used to interpose.  */
#define DF_1_NODEFLIB	0x00000800	/* Ignore default lib search path.  */
#define DF_1_NODUMP	0x00001000	/* Object can't be dldump'ed.  */
#define DF_1_CONFALT	0x00002000	/* Configuration alternative created.*/
#define DF_1_ENDFILTEE	0x00004000	/* Filtee terminates filters search. */
#define	DF_1_DISPRELDNE	0x00008000	/* Disp reloc applied at build time. */
#define	DF_1_DISPRELPND	0x00010000	/* Disp reloc applied at run-time.  */
#define	DF_1_NODIRECT	0x00020000	/* Object has no-direct binding. */
#define	DF_1_IGNMULDEF	0x00040000
#define	DF_1_NOKSYMS	0x00080000
#define	DF_1_NOHDR	0x00100000
#define	DF_1_EDITED	0x00200000	/* Object is modified after built.  */
#define	DF_1_NORELOC	0x00400000
#define	DF_1_SYMINTPOSE	0x00800000	/* Object has individual interposers.  */
#define	DF_1_GLOBAUDIT	0x01000000	/* Global auditing required.  */
#define	DF_1_SINGLETON	0x02000000	/* Singleton symbols are used.  */
#define	DF_1_STUB	0x04000000
#define	DF_1_PIE	0x08000000
#define	DF_1_KMOD       0x10000000
#define	DF_1_WEAKFILTER 0x20000000
#define	DF_1_NOCOMMON   0x40000000

/* Flags for the feature selection in DT_FEATURE_1.  */
#define DTF_1_PARINIT	0x00000001
#define DTF_1_CONFEXP	0x00000002

/* Flags in the DT_POSFLAG_1 entry effecting only the next DT_* entry.  */
#define DF_P1_LAZYLOAD	0x00000001	/* Lazyload following object.  */
#define DF_P1_GROUPPERM	0x00000002	/* Symbols from next object are not
					   generally available.  */

typedef struct
{
  Elf32_Word	sh_name;		/* Section name (string tbl index) */
  Elf32_Word	sh_type;		/* Section type */
  Elf32_Word	sh_flags;		/* Section flags */
  Elf32_Addr	sh_addr;		/* Section virtual addr at execution */
  Elf32_Off	sh_offset;		/* Section file offset */
  Elf32_Word	sh_size;		/* Section size in bytes */
  Elf32_Word	sh_link;		/* Link to another section */
  Elf32_Word	sh_info;		/* Additional section information */
  Elf32_Word	sh_addralign;		/* Section alignment */
  Elf32_Word	sh_entsize;		/* Entry size if section holds table */
} Elf32_Shdr;

typedef struct
{
  Elf64_Word	sh_name;		/* Section name (string tbl index) */
  Elf64_Word	sh_type;		/* Section type */
  Elf64_Xword	sh_flags;		/* Section flags */
  Elf64_Addr	sh_addr;		/* Section virtual addr at execution */
  Elf64_Off	sh_offset;		/* Section file offset */
  Elf64_Xword	sh_size;		/* Section size in bytes */
  Elf64_Word	sh_link;		/* Link to another section */
  Elf64_Word	sh_info;		/* Additional section information */
  Elf64_Xword	sh_addralign;		/* Section alignment */
  Elf64_Xword	sh_entsize;		/* Entry size if section holds table */
} Elf64_Shdr;

#define SHN_UNDEF	0		/* Undefined section */
#define SHN_LORESERVE	0xff00		/* Start of reserved indices */
#define SHN_LOPROC	0xff00		/* Start of processor-specific */
#define SHN_BEFORE	0xff00		/* Order section before all others
					   (Solaris).  */
#define SHN_AFTER	0xff01		/* Order section after all others
					   (Solaris).  */
#define SHN_HIPROC	0xff1f		/* End of processor-specific */
#define SHN_LOOS	0xff20		/* Start of OS-specific */
#define SHN_HIOS	0xff3f		/* End of OS-specific */
#define SHN_ABS		0xfff1		/* Associated symbol is absolute */
#define SHN_COMMON	0xfff2		/* Associated symbol is common */
#define SHN_XINDEX	0xffff		/* Index is in extra table.  */
#define SHN_HIRESERVE	0xffff		/* End of reserved indices */

/* Legal values for sh_type (section type).  */

#define SHT_NULL	  0		/* Section header table entry unused */
#define SHT_PROGBITS	  1		/* Program data */
#define SHT_SYMTAB	  2		/* Symbol table */
#define SHT_STRTAB	  3		/* String table */
#define SHT_RELA	  4		/* Relocation entries with addends */
#define SHT_HASH	  5		/* Symbol hash table */
#define SHT_DYNAMIC	  6		/* Dynamic linking information */
#define SHT_NOTE	  7		/* Notes */
#define SHT_NOBITS	  8		/* Program space with no data (bss) */
#define SHT_REL		  9		/* Relocation entries, no addends */
#define SHT_SHLIB	  10		/* Reserved */
#define SHT_DYNSYM	  11		/* Dynamic linker symbol table */
#define SHT_INIT_ARRAY	  14		/* Array of constructors */
#define SHT_FINI_ARRAY	  15		/* Array of destructors */
#define SHT_PREINIT_ARRAY 16		/* Array of pre-constructors */
#define SHT_GROUP	  17		/* Section group */
#define SHT_SYMTAB_SHNDX  18		/* Extended section indices */
#define	SHT_NUM		  19		/* Number of defined types.  */
#define SHT_LOOS	  0x60000000	/* Start OS-specific.  */
#define SHT_GNU_ATTRIBUTES 0x6ffffff5	/* Object attributes.  */
#define SHT_GNU_HASH	  0x6ffffff6	/* GNU-style hash table.  */
#define SHT_GNU_LIBLIST	  0x6ffffff7	/* Prelink library list */
#define SHT_CHECKSUM	  0x6ffffff8	/* Checksum for DSO content.  */
#define SHT_LOSUNW	  0x6ffffffa	/* Sun-specific low bound.  */
#define SHT_SUNW_move	  0x6ffffffa
#define SHT_SUNW_COMDAT   0x6ffffffb
#define SHT_SUNW_syminfo  0x6ffffffc
#define SHT_GNU_verdef	  0x6ffffffd	/* Version definition section.  */
#define SHT_GNU_verneed	  0x6ffffffe	/* Version needs section.  */
#define SHT_GNU_versym	  0x6fffffff	/* Version symbol table.  */
#define SHT_HISUNW	  0x6fffffff	/* Sun-specific high bound.  */
#define SHT_HIOS	  0x6fffffff	/* End OS-specific type */
#define SHT_LOPROC	  0x70000000	/* Start of processor-specific */
#define SHT_HIPROC	  0x7fffffff	/* End of processor-specific */
#define SHT_LOUSER	  0x80000000	/* Start of application-specific */
#define SHT_HIUSER	  0x8fffffff	/* End of application-specific */

/* Legal values for sh_flags (section flags).  */

#define SHF_WRITE	     (1 << 0)	/* Writable */
#define SHF_ALLOC	     (1 << 1)	/* Occupies memory during execution */
#define SHF_EXECINSTR	     (1 << 2)	/* Executable */
#define SHF_MERGE	     (1 << 4)	/* Might be merged */
#define SHF_STRINGS	     (1 << 5)	/* Contains nul-terminated strings */
#define SHF_INFO_LINK	     (1 << 6)	/* `sh_info' contains SHT index */
#define SHF_LINK_ORDER	     (1 << 7)	/* Preserve order after combining */
#define SHF_OS_NONCONFORMING (1 << 8)	/* Non-standard OS specific handling
					   required */
#define SHF_GROUP	     (1 << 9)	/* Section is member of a group.  */
#define SHF_TLS		     (1 << 10)	/* Section hold thread-local data.  */
#define SHF_COMPRESSED	     (1 << 11)	/* Section with compressed data. */
#define SHF_MASKOS	     0x0ff00000	/* OS-specific.  */
#define SHF_MASKPROC	     0xf0000000	/* Processor-specific */
#define SHF_GNU_RETAIN	     (1 << 21)  /* Not to be GCed by linker.  */
#define SHF_ORDERED	     (1 << 30)	/* Special ordering requirement
					   (Solaris).  */
#define SHF_EXCLUDE	     (1U << 31)	/* Section is excluded unless
					   referenced or allocated (Solaris).*/

/* Symbol table entry.  */

typedef struct
{
  Elf32_Word	st_name;		/* Symbol name (string tbl index) */
  Elf32_Addr	st_value;		/* Symbol value */
  Elf32_Word	st_size;		/* Symbol size */
  unsigned char	st_info;		/* Symbol type and binding */
  unsigned char	st_other;		/* Symbol visibility */
  Elf32_Section	st_shndx;		/* Section index */
} Elf32_Sym;

typedef struct
{
  Elf64_Word	st_name;		/* Symbol name (string tbl index) */
  unsigned char	st_info;		/* Symbol type and binding */
  unsigned char st_other;		/* Symbol visibility */
  Elf64_Section	st_shndx;		/* Section index */
  Elf64_Addr	st_value;		/* Symbol value */
  Elf64_Xword	st_size;		/* Symbol size */
} Elf64_Sym;

/* The syminfo section if available contains additional information about
   every dynamic symbol.  */

typedef struct
{
  Elf32_Half si_boundto;		/* Direct bindings, symbol bound to */
  Elf32_Half si_flags;			/* Per symbol flags */
} Elf32_Syminfo;

typedef struct
{
  Elf64_Half si_boundto;		/* Direct bindings, symbol bound to */
  Elf64_Half si_flags;			/* Per symbol flags */
} Elf64_Syminfo;

/* Possible values for si_boundto.  */
#define SYMINFO_BT_SELF		0xffff	/* Symbol bound to self */
#define SYMINFO_BT_PARENT	0xfffe	/* Symbol bound to parent */
#define SYMINFO_BT_LOWRESERVE	0xff00	/* Beginning of reserved entries */

/* Possible bitmasks for si_flags.  */
#define SYMINFO_FLG_DIRECT	0x0001	/* Direct bound symbol */
#define SYMINFO_FLG_PASSTHRU	0x0002	/* Pass-thru symbol for translator */
#define SYMINFO_FLG_COPY	0x0004	/* Symbol is a copy-reloc */
#define SYMINFO_FLG_LAZYLOAD	0x0008	/* Symbol bound to object to be lazy
					   loaded */
/* Syminfo version values.  */
#define SYMINFO_NONE		0
#define SYMINFO_CURRENT		1
#define SYMINFO_NUM		2


/* How to extract and insert information held in the st_info field.  */

#define ELF32_ST_BIND(val)		(((unsigned char) (val)) >> 4)
#define ELF32_ST_TYPE(val)		((val) & 0xf)
#define ELF32_ST_INFO(bind, type)	(((bind) << 4) + ((type) & 0xf))

/* Both Elf32_Sym and Elf64_Sym use the same one-byte st_info field.  */
#define ELF64_ST_BIND(val)		ELF32_ST_BIND (val)
#define ELF64_ST_TYPE(val)		ELF32_ST_TYPE (val)
#define ELF64_ST_INFO(bind, type)	ELF32_ST_INFO ((bind), (type))

/* Legal values for ST_BIND subfield of st_info (symbol binding).  */

#define STB_LOCAL	0		/* Local symbol */
#define STB_GLOBAL	1		/* Global symbol */
#define STB_WEAK	2		/* Weak symbol */
#define	STB_NUM		3		/* Number of defined types.  */
#define STB_LOOS	10		/* Start of OS-specific */
#define STB_GNU_UNIQUE	10		/* Unique symbol.  */
#define STB_HIOS	12		/* End of OS-specific */
#define STB_LOPROC	13		/* Start of processor-specific */
#define STB_HIPROC	15		/* End of processor-specific */

/* Legal values for ST_TYPE subfield of st_info (symbol type).  */

#define STT_NOTYPE	0		/* Symbol type is unspecified */
#define STT_OBJECT	1		/* Symbol is a data object */
#define STT_FUNC	2		/* Symbol is a code object */
#define STT_SECTION	3		/* Symbol associated with a section */
#define STT_FILE	4		/* Symbol's name is file name */
#define STT_COMMON	5		/* Symbol is a common data object */
#define STT_TLS		6		/* Symbol is thread-local data object*/
#define	STT_NUM		7		/* Number of defined types.  */
#define STT_LOOS	10		/* Start of OS-specific */
#define STT_GNU_IFUNC	10		/* Symbol is indirect code object */
#define STT_HIOS	12		/* End of OS-specific */
#define STT_LOPROC	13		/* Start of processor-specific */
#define STT_HIPROC	15		/* End of processor-specific */


/* Symbol table indices are found in the hash buckets and chain table
   of a symbol hash table section.  This special index value indicates
   the end of a chain, meaning no further symbols are found in that bucket.  */

#define STN_UNDEF	0		/* End of a chain.  */


/* How to extract and insert information held in the st_other field.  */

#define ELF32_ST_VISIBILITY(o)	((o) & 0x03)

/* For ELF64 the definitions are the same.  */
#define ELF64_ST_VISIBILITY(o)	ELF32_ST_VISIBILITY (o)

/* Symbol visibility specification encoded in the st_other field.  */
#define STV_DEFAULT	0		/* Default symbol visibility rules */
#define STV_INTERNAL	1		/* Processor specific hidden class */
#define STV_HIDDEN	2		/* Sym unavailable in other modules */
#define STV_PROTECTED	3		/* Not preemptible, not exported */


/* Relocation table entry without addend (in section of type SHT_REL).  */

typedef struct
{
  Elf32_Addr	r_offset;		/* Address */
  Elf32_Word	r_info;			/* Relocation type and symbol index */
} Elf32_Rel;

/* I have seen two different definitions of the Elf64_Rel and
   Elf64_Rela structures, so we'll leave them out until Novell (or
   whoever) gets their act together.  */
/* The following, at least, is used on Sparc v9, MIPS, and Alpha.  */

typedef struct
{
  Elf64_Addr	r_offset;		/* Address */
  Elf64_Xword	r_info;			/* Relocation type and symbol index */
} Elf64_Rel;

/* Relocation table entry with addend (in section of type SHT_RELA).  */

typedef struct
{
  Elf32_Addr	r_offset;		/* Address */
  Elf32_Word	r_info;			/* Relocation type and symbol index */
  Elf32_Sword	r_addend;		/* Addend */
} Elf32_Rela;

typedef struct
{
  Elf64_Addr	r_offset;		/* Address */
  Elf64_Xword	r_info;			/* Relocation type and symbol index */
  Elf64_Sxword	r_addend;		/* Addend */
} Elf64_Rela;

/* How to extract and insert information held in the r_info field.  */

#define ELF32_R_SYM(val)		((val) >> 8)
#define ELF32_R_TYPE(val)		((val) & 0xff)
#define ELF32_R_INFO(sym, type)		(((sym) << 8) + ((type) & 0xff))

#define ELF64_R_SYM(i)			((i) >> 32)
#define ELF64_R_TYPE(i)			((i) & 0xffffffff)
#define ELF64_R_INFO(sym,type)		((((Elf64_Xword) (sym)) << 32) + (type))

#define R_X86_64_64		1	/* Direct 64-bit */
#define R_AARCH64_ABS64        257	/* Direct 64-bit.  */

#define R_X86_64_COPY		5	/* Copy symbol at runtime */
#define R_AARCH64_COPY         1024	/* Copy symbol at runtime.  */

#define R_X86_64_JUMP_SLOT	7	/* Create PLT entry */
#define R_AARCH64_JUMP_SLOT    1026	/* Create PLT entry.  */

#define R_X86_64_GLOB_DAT	6	/* Create GOT entry */
#define R_AARCH64_GLOB_DAT     1025	/* Create GOT entry.  */

#define R_X86_64_RELATIVE	8	/* Adjust by program base */
#define R_AARCH64_RELATIVE     1027	/* Adjust by program base.  */

#define R_X86_64_IRELATIVE	37	/* Adjust indirectly by program base */
#define R_AARCH64_IRELATIVE	1032	/* STT_GNU_IFUNC relocation.  */

/* -------------------- Mach-O -------------------- */

/*
 * The 64-bit mach header appears at the very beginning of object files for
 * 64-bit architectures.
 */
struct mach_header_64 {
	uint32_t	magic;		/* mach magic number identifier */
	int32_t		cputype;	/* cpu specifier */
	int32_t		cpusubtype;	/* machine specifier */
	uint32_t	filetype;	/* type of file */
	uint32_t	ncmds;		/* number of load commands */
	uint32_t	sizeofcmds;	/* the size of all the load commands */
	uint32_t	flags;		/* flags */
	uint32_t	reserved;	/* reserved */
};

/* Constant for the magic field of the mach_header_64 (64-bit architectures) */
#define MH_MAGIC_64 0xfeedfacf /* the 64-bit mach magic number */

/*
 * The layout of the file depends on the filetype.  For all but the MH_OBJECT
 * file type the segments are padded out and aligned on a segment alignment
 * boundary for efficient demand pageing.  The MH_EXECUTE, MH_FVMLIB, MH_DYLIB,
 * MH_DYLINKER and MH_BUNDLE file types also have the headers included as part
 * of their first segment.
 * 
 * The file type MH_OBJECT is a compact format intended as output of the
 * assembler and input (and possibly output) of the link editor (the .o
 * format).  All sections are in one unnamed segment with no segment padding. 
 * This format is used as an executable format when the file is so small the
 * segment padding greatly increases its size.
 *
 * The file type MH_PRELOAD is an executable format intended for things that
 * are not executed under the kernel (proms, stand alones, kernels, etc).  The
 * format can be executed under the kernel but may demand paged it and not
 * preload it before execution.
 *
 * A core file is in MH_CORE format and can be any in an arbritray legal
 * Mach-O file.
 *
 * Constants for the filetype field of the mach_header
 */
#define	MH_OBJECT	0x1		/* relocatable object file */
#define	MH_EXECUTE	0x2		/* demand paged executable file */
#define	MH_FVMLIB	0x3		/* fixed VM shared library file */
#define	MH_CORE		0x4		/* core file */
#define	MH_PRELOAD	0x5		/* preloaded executable file */
#define	MH_DYLIB	0x6		/* dynamically bound shared library */
#define	MH_DYLINKER	0x7		/* dynamic link editor */
#define	MH_BUNDLE	0x8		/* dynamically bound bundle file */
#define	MH_DYLIB_STUB	0x9		/* shared library stub for static
					   linking only, no section contents */
#define	MH_DSYM		0xa		/* companion file with only debug
					   sections */
#define	MH_KEXT_BUNDLE	0xb		/* x86_64 kexts */
#define MH_FILESET	0xc		/* a file composed of other Mach-Os to
					   be run in the same userspace sharing
					   a single linkedit. */
#define	MH_GPU_EXECUTE	0xd		/* gpu program */
#define	MH_GPU_DYLIB	0xe		/* gpu support functions */


/* Constants for the flags field of the mach_header */
#define	MH_NOUNDEFS	0x1		/* the object file has no undefined
					   references */
#define	MH_INCRLINK	0x2		/* the object file is the output of an
					   incremental link against a base file
					   and can't be link edited again */
#define MH_DYLDLINK	0x4		/* the object file is input for the
					   dynamic linker and can't be staticly
					   link edited again */
#define MH_BINDATLOAD	0x8		/* the object file's undefined
					   references are bound by the dynamic
					   linker when loaded. */
#define MH_PREBOUND	0x10		/* the file has its dynamic undefined
					   references prebound. */
#define MH_SPLIT_SEGS	0x20		/* the file has its read-only and
					   read-write segments split */
#define MH_LAZY_INIT	0x40		/* the shared library init routine is
					   to be run lazily via catching memory
					   faults to its writeable segments
					   (obsolete) */
#define MH_TWOLEVEL	0x80		/* the image is using two-level name
					   space bindings */
#define MH_FORCE_FLAT	0x100		/* the executable is forcing all images
					   to use flat name space bindings */
#define MH_NOMULTIDEFS	0x200		/* this umbrella guarantees no multiple
					   defintions of symbols in its
					   sub-images so the two-level namespace
					   hints can always be used. */
#define MH_NOFIXPREBINDING 0x400	/* do not have dyld notify the
					   prebinding agent about this
					   executable */
#define MH_PREBINDABLE  0x800           /* the binary is not prebound but can
					   have its prebinding redone. only used
                                           when MH_PREBOUND is not set. */
#define MH_ALLMODSBOUND 0x1000		/* indicates that this binary binds to
                                           all two-level namespace modules of
					   its dependent libraries. only used
					   when MH_PREBINDABLE and MH_TWOLEVEL
					   are both set. */ 
#define MH_SUBSECTIONS_VIA_SYMBOLS 0x2000/* safe to divide up the sections into
					    sub-sections via symbols for dead
					    code stripping */
#define MH_CANONICAL    0x4000		/* the binary has been canonicalized
					   via the unprebind operation */
#define MH_WEAK_DEFINES	0x8000		/* the final linked image contains
					   external weak symbols */
#define MH_BINDS_TO_WEAK 0x10000	/* the final linked image uses
					   weak symbols */

#define MH_ALLOW_STACK_EXECUTION 0x20000/* When this bit is set, all stacks 
					   in the task will be given stack
					   execution privilege.  Only used in
					   MH_EXECUTE filetypes. */
#define MH_ROOT_SAFE 0x40000           /* When this bit is set, the binary 
					  declares it is safe for use in
					  processes with uid zero */
                                         
#define MH_SETUID_SAFE 0x80000         /* When this bit is set, the binary 
					  declares it is safe for use in
					  processes when issetugid() is true */

#define MH_NO_REEXPORTED_DYLIBS 0x100000 /* When this bit is set on a dylib, 
					  the static linker does not need to
					  examine dependent dylibs to see
					  if any are re-exported */
#define	MH_PIE 0x200000			/* When this bit is set, the OS will
					   load the main executable at a
					   random address.  Only used in
					   MH_EXECUTE filetypes. */
#define	MH_DEAD_STRIPPABLE_DYLIB 0x400000 /* Only for use on dylibs.  When
					     linking against a dylib that
					     has this bit set, the static linker
					     will automatically not create a
					     LC_LOAD_DYLIB load command to the
					     dylib if no symbols are being
					     referenced from the dylib. */
#define MH_HAS_TLV_DESCRIPTORS 0x800000 /* Contains a section of type 
					    S_THREAD_LOCAL_VARIABLES */

#define MH_NO_HEAP_EXECUTION 0x1000000	/* When this bit is set, the OS will
					   run the main executable with
					   a non-executable heap even on
					   platforms (e.g. i386) that don't
					   require it. Only used in MH_EXECUTE
					   filetypes. */

#define MH_APP_EXTENSION_SAFE 0x02000000 /* The code was linked for use in an
					    application extension. */

#define	MH_NLIST_OUTOFSYNC_WITH_DYLDINFO 0x04000000 /* The external symbols
					   listed in the nlist symbol table do
					   not include all the symbols listed in
					   the dyld info. */

#define	MH_SIM_SUPPORT 0x08000000	/* Allow LC_MIN_VERSION_MACOS and
					   LC_BUILD_VERSION load commands with
					   the platforms macOS, macCatalyst,
					   iOSSimulator, tvOSSimulator and
					   watchOSSimulator. */

#define	MH_IMPLICIT_PAGEZERO 0x10000000	/* main executable has no __PAGEZERO
					   segment.  Instead, loader (xnu)
					   will load program high and block
					   out all memory below it. */

#define MH_DYLIB_IN_CACHE 0x80000000	/* Only for use on dylibs. When this bit
					   is set, the dylib is part of the dyld
					   shared cache, rather than loose in
					   the filesystem. */

/*
 * Capability bits used in the definition of cpu_type.
 */
#define CPU_ARCH_MASK           0xff000000      /* mask for architecture bits */
#define CPU_ARCH_ABI64          0x01000000      /* 64 bit ABI */
#define CPU_ARCH_ABI64_32       0x02000000      /* ABI for 64-bit hardware with 32-bit types; LP32 */

/*
 *	Machine types known by all.
 */

typedef int                     cpu_type_t;

#define CPU_TYPE_X86            ((cpu_type_t) 7)
#define CPU_TYPE_I386           CPU_TYPE_X86            /* compatibility */
#define CPU_TYPE_X86_64         (CPU_TYPE_X86 | CPU_ARCH_ABI64)

#define CPU_TYPE_ARM            ((cpu_type_t) 12)
#define CPU_TYPE_ARM64          (CPU_TYPE_ARM | CPU_ARCH_ABI64)

/*
 * The load commands directly follow the mach_header.  The total size of all
 * of the commands is given by the sizeofcmds field in the mach_header.  All
 * load commands must have as their first two fields cmd and cmdsize.  The cmd
 * field is filled in with a constant for that command type.  Each command type
 * has a structure specifically for it.  The cmdsize field is the size in bytes
 * of the particular load command structure plus anything that follows it that
 * is a part of the load command (i.e. section structures, strings, etc.).  To
 * advance to the next load command the cmdsize can be added to the offset or
 * pointer of the current load command.  The cmdsize for 32-bit architectures
 * MUST be a multiple of 4 bytes and for 64-bit architectures MUST be a multiple
 * of 8 bytes (these are forever the maximum alignment of any load commands).
 * The padded bytes must be zero.  All tables in the object file must also
 * follow these rules so the file can be memory mapped.  Otherwise the pointers
 * to these tables will not work well or at all on some machines.  With all
 * padding zeroed like objects will compare byte for byte.
 */
struct load_command {
	uint32_t cmd;		/* type of load command */
	uint32_t cmdsize;	/* total size of command in bytes */
};

/*
 * After MacOS X 10.1 when a new load command is added that is required to be
 * understood by the dynamic linker for the image to execute properly the
 * LC_REQ_DYLD bit will be or'ed into the load command constant.  If the dynamic
 * linker sees such a load command it it does not understand will issue a
 * "unknown load command required for execution" error and refuse to use the
 * image.  Other load commands without this bit that are not understood will
 * simply be ignored.
 */
#define LC_REQ_DYLD 0x80000000

/* Constants for the cmd field of all load commands, the type */
#define	LC_SEGMENT	0x1	/* segment of this file to be mapped */
#define	LC_SYMTAB	0x2	/* link-edit stab symbol table info */
#define	LC_SYMSEG	0x3	/* link-edit gdb symbol table info (obsolete) */
#define	LC_THREAD	0x4	/* thread */
#define	LC_UNIXTHREAD	0x5	/* unix thread (includes a stack) */
#define	LC_LOADFVMLIB	0x6	/* load a specified fixed VM shared library */
#define	LC_IDFVMLIB	0x7	/* fixed VM shared library identification */
#define	LC_IDENT	0x8	/* object identification info (obsolete) */
#define LC_FVMFILE	0x9	/* fixed VM file inclusion (internal use) */
#define LC_PREPAGE      0xa     /* prepage command (internal use) */
#define	LC_DYSYMTAB	0xb	/* dynamic link-edit symbol table info */
#define	LC_LOAD_DYLIB	0xc	/* load a dynamically linked shared library */
#define	LC_ID_DYLIB	0xd	/* dynamically linked shared lib ident */
#define LC_LOAD_DYLINKER 0xe	/* load a dynamic linker */
#define LC_ID_DYLINKER	0xf	/* dynamic linker identification */
#define	LC_PREBOUND_DYLIB 0x10	/* modules prebound for a dynamically */
				/*  linked shared library */
#define	LC_ROUTINES	0x11	/* image routines */
#define	LC_SUB_FRAMEWORK 0x12	/* sub framework */
#define	LC_SUB_UMBRELLA 0x13	/* sub umbrella */
#define	LC_SUB_CLIENT	0x14	/* sub client */
#define	LC_SUB_LIBRARY  0x15	/* sub library */
#define	LC_TWOLEVEL_HINTS 0x16	/* two-level namespace lookup hints */
#define	LC_PREBIND_CKSUM  0x17	/* prebind checksum */

/*
 * load a dynamically linked shared library that is allowed to be missing
 * (all symbols are weak imported).
 */
#define	LC_LOAD_WEAK_DYLIB (0x18 | LC_REQ_DYLD)

#define	LC_SEGMENT_64	0x19	/* 64-bit segment of this file to be
				   mapped */
#define	LC_ROUTINES_64	0x1a	/* 64-bit image routines */
#define LC_UUID		0x1b	/* the uuid */
#define LC_RPATH       (0x1c | LC_REQ_DYLD)    /* runpath additions */
#define LC_CODE_SIGNATURE 0x1d	/* local of code signature */
#define LC_SEGMENT_SPLIT_INFO 0x1e /* local of info to split segments */
#define LC_REEXPORT_DYLIB (0x1f | LC_REQ_DYLD) /* load and re-export dylib */
#define	LC_LAZY_LOAD_DYLIB 0x20	/* delay load of dylib until first use */
#define	LC_ENCRYPTION_INFO 0x21	/* encrypted segment information */
#define	LC_DYLD_INFO 	0x22	/* compressed dyld information */
#define	LC_DYLD_INFO_ONLY (0x22|LC_REQ_DYLD)	/* compressed dyld information only */
#define	LC_LOAD_UPWARD_DYLIB (0x23 | LC_REQ_DYLD) /* load upward dylib */
#define LC_VERSION_MIN_MACOSX 0x24   /* build for MacOSX min OS version */
#define LC_VERSION_MIN_IPHONEOS 0x25 /* build for iPhoneOS min OS version */
#define LC_FUNCTION_STARTS 0x26 /* compressed table of function start addresses */
#define LC_DYLD_ENVIRONMENT 0x27 /* string for dyld to treat
				    like environment variable */
#define LC_MAIN (0x28|LC_REQ_DYLD) /* replacement for LC_UNIXTHREAD */
#define LC_DATA_IN_CODE 0x29 /* table of non-instructions in __text */
#define LC_SOURCE_VERSION 0x2A /* source version used to build binary */
#define LC_DYLIB_CODE_SIGN_DRS 0x2B /* Code signing DRs copied from linked dylibs */
#define	LC_ENCRYPTION_INFO_64 0x2C /* 64-bit encrypted segment information */
#define LC_LINKER_OPTION 0x2D /* linker options in MH_OBJECT files */
#define LC_LINKER_OPTIMIZATION_HINT 0x2E /* optimization hints in MH_OBJECT files */
#define LC_VERSION_MIN_TVOS 0x2F /* build for AppleTV min OS version */
#define LC_VERSION_MIN_WATCHOS 0x30 /* build for Watch min OS version */
#define LC_NOTE 0x31 /* arbitrary data included within a Mach-O file */
#define LC_BUILD_VERSION 0x32 /* build for platform min OS version */
#define LC_DYLD_EXPORTS_TRIE (0x33 | LC_REQ_DYLD) /* used with linkedit_data_command, payload is trie */
#define LC_DYLD_CHAINED_FIXUPS (0x34 | LC_REQ_DYLD) /* used with linkedit_data_command */
#define LC_FILESET_ENTRY (0x35 | LC_REQ_DYLD) /* used with fileset_entry_command */
#define LC_ATOM_INFO 0x36 /* used with linkedit_data_command */

/*
 * A variable length string in a load command is represented by an lc_str
 * union.  The strings are stored just after the load command structure and
 * the offset is from the start of the load command structure.  The size
 * of the string is reflected in the cmdsize field of the load command.
 * Once again any padded bytes to bring the cmdsize field to a multiple
 * of 4 bytes must be zero.
 */
union lc_str {
	uint32_t	offset;	/* offset to the string */
#ifndef __LP64__
	char		*ptr;	/* pointer to the string */
#endif 
};

/*
 * The segment load command indicates that a part of this file is to be
 * mapped into the task's address space.  The size of this segment in memory,
 * vmsize, maybe equal to or larger than the amount to map from this file,
 * filesize.  The file is mapped starting at fileoff to the beginning of
 * the segment in memory, vmaddr.  The rest of the memory of the segment,
 * if any, is allocated zero fill on demand.  The segment's maximum virtual
 * memory protection and initial virtual memory protection are specified
 * by the maxprot and initprot fields.  If the segment has sections then the
 * section structures directly follow the segment command and their size is
 * reflected in cmdsize.
 */
struct segment_command { /* for 32-bit architectures */
	uint32_t	cmd;		/* LC_SEGMENT */
	uint32_t	cmdsize;	/* includes sizeof section structs */
	char		segname[16];	/* segment name */
	uint32_t	vmaddr;		/* memory address of this segment */
	uint32_t	vmsize;		/* memory size of this segment */
	uint32_t	fileoff;	/* file offset of this segment */
	uint32_t	filesize;	/* amount to map from the file */
	int32_t		maxprot;	/* maximum VM protection */
	int32_t		initprot;	/* initial VM protection */
	uint32_t	nsects;		/* number of sections in segment */
	uint32_t	flags;		/* flags */
};

/*
 * The 64-bit segment load command indicates that a part of this file is to be
 * mapped into a 64-bit task's address space.  If the 64-bit segment has
 * sections then section_64 structures directly follow the 64-bit segment
 * command and their size is reflected in cmdsize.
 */
struct segment_command_64 { /* for 64-bit architectures */
	uint32_t	cmd;		/* LC_SEGMENT_64 */
	uint32_t	cmdsize;	/* includes sizeof section_64 structs */
	char		segname[16];	/* segment name */
	uint64_t	vmaddr;		/* memory address of this segment */
	uint64_t	vmsize;		/* memory size of this segment */
	uint64_t	fileoff;	/* file offset of this segment */
	uint64_t	filesize;	/* amount to map from the file */
	int32_t		maxprot;	/* maximum VM protection */
	int32_t		initprot;	/* initial VM protection */
	uint32_t	nsects;		/* number of sections in segment */
	uint32_t	flags;		/* flags */
};

/* Constants for the flags field of the segment_command */
#define	SG_HIGHVM	0x1	/* the file contents for this segment is for
				   the high part of the VM space, the low part
				   is zero filled (for stacks in core files) */
#define	SG_FVMLIB	0x2	/* this segment is the VM that is allocated by
				   a fixed VM library, for overlap checking in
				   the link editor */
#define	SG_NORELOC	0x4	/* this segment has nothing that was relocated
				   in it and nothing relocated to it, that is
				   it maybe safely replaced without relocation*/
#define SG_PROTECTED_VERSION_1	0x8 /* This segment is protected.  If the
				       segment starts at file offset 0, the
				       first page of the segment is not
				       protected.  All other pages of the
				       segment are protected. */
#define SG_READ_ONLY    0x10 /* This segment is made read-only after fixups */



/*
 * A segment is made up of zero or more sections.  Non-MH_OBJECT files have
 * all of their segments with the proper sections in each, and padded to the
 * specified segment alignment when produced by the link editor.  The first
 * segment of a MH_EXECUTE and MH_FVMLIB format file contains the mach_header
 * and load commands of the object file before its first section.  The zero
 * fill sections are always last in their segment (in all formats).  This
 * allows the zeroed segment padding to be mapped into memory where zero fill
 * sections might be. The gigabyte zero fill sections, those with the section
 * type S_GB_ZEROFILL, can only be in a segment with sections of this type.
 * These segments are then placed after all other segments.
 *
 * The MH_OBJECT format has all of its sections in one segment for
 * compactness.  There is no padding to a specified segment boundary and the
 * mach_header and load commands are not part of the segment.
 *
 * Sections with the same section name, sectname, going into the same segment,
 * segname, are combined by the link editor.  The resulting section is aligned
 * to the maximum alignment of the combined sections and is the new section's
 * alignment.  The combined sections are aligned to their original alignment in
 * the combined section.  Any padded bytes to get the specified alignment are
 * zeroed.
 *
 * The format of the relocation entries referenced by the reloff and nreloc
 * fields of the section structure for mach object files is described in the
 * header file <reloc.h>.
 */
struct section { /* for 32-bit architectures */
	char		sectname[16];	/* name of this section */
	char		segname[16];	/* segment this section goes in */
	uint32_t	addr;		/* memory address of this section */
	uint32_t	size;		/* size in bytes of this section */
	uint32_t	offset;		/* file offset of this section */
	uint32_t	align;		/* section alignment (power of 2) */
	uint32_t	reloff;		/* file offset of relocation entries */
	uint32_t	nreloc;		/* number of relocation entries */
	uint32_t	flags;		/* flags (section type and attributes)*/
	uint32_t	reserved1;	/* reserved (for offset or index) */
	uint32_t	reserved2;	/* reserved (for count or sizeof) */
};

struct section_64 { /* for 64-bit architectures */
	char		sectname[16];	/* name of this section */
	char		segname[16];	/* segment this section goes in */
	uint64_t	addr;		/* memory address of this section */
	uint64_t	size;		/* size in bytes of this section */
	uint32_t	offset;		/* file offset of this section */
	uint32_t	align;		/* section alignment (power of 2) */
	uint32_t	reloff;		/* file offset of relocation entries */
	uint32_t	nreloc;		/* number of relocation entries */
	uint32_t	flags;		/* flags (section type and attributes)*/
	uint32_t	reserved1;	/* reserved (for offset or index) */
	uint32_t	reserved2;	/* reserved (for count or sizeof) */
	uint32_t	reserved3;	/* reserved */
};

/*
 * The flags field of a section structure is separated into two parts a section
 * type and section attributes.  The section types are mutually exclusive (it
 * can only have one type) but the section attributes are not (it may have more
 * than one attribute).
 */
#define SECTION_TYPE		 0x000000ff	/* 256 section types */
#define SECTION_ATTRIBUTES	 0xffffff00	/*  24 section attributes */

/* Constants for the type of a section */
#define	S_REGULAR		0x0	/* regular section */
#define	S_ZEROFILL		0x1	/* zero fill on demand section */
#define	S_CSTRING_LITERALS	0x2	/* section with only literal C strings*/
#define	S_4BYTE_LITERALS	0x3	/* section with only 4 byte literals */
#define	S_8BYTE_LITERALS	0x4	/* section with only 8 byte literals */
#define	S_LITERAL_POINTERS	0x5	/* section with only pointers to */
					/*  literals */
/*
 * For the two types of symbol pointers sections and the symbol stubs section
 * they have indirect symbol table entries.  For each of the entries in the
 * section the indirect symbol table entries, in corresponding order in the
 * indirect symbol table, start at the index stored in the reserved1 field
 * of the section structure.  Since the indirect symbol table entries
 * correspond to the entries in the section the number of indirect symbol table
 * entries is inferred from the size of the section divided by the size of the
 * entries in the section.  For symbol pointers sections the size of the entries
 * in the section is 4 bytes and for symbol stubs sections the byte size of the
 * stubs is stored in the reserved2 field of the section structure.
 */
#define	S_NON_LAZY_SYMBOL_POINTERS	0x6	/* section with only non-lazy
						   symbol pointers */
#define	S_LAZY_SYMBOL_POINTERS		0x7	/* section with only lazy symbol
						   pointers */
#define	S_SYMBOL_STUBS			0x8	/* section with only symbol
						   stubs, byte size of stub in
						   the reserved2 field */
#define	S_MOD_INIT_FUNC_POINTERS	0x9	/* section with only function
						   pointers for initialization*/
#define	S_MOD_TERM_FUNC_POINTERS	0xa	/* section with only function
						   pointers for termination */
#define	S_COALESCED			0xb	/* section contains symbols that
						   are to be coalesced */
#define	S_GB_ZEROFILL			0xc	/* zero fill on demand section
						   (that can be larger than 4
						   gigabytes) */
#define	S_INTERPOSING			0xd	/* section with only pairs of
						   function pointers for
						   interposing */
#define	S_16BYTE_LITERALS		0xe	/* section with only 16 byte
						   literals */
#define	S_DTRACE_DOF			0xf	/* section contains 
						   DTrace Object Format */
#define	S_LAZY_DYLIB_SYMBOL_POINTERS	0x10	/* section with only lazy
						   symbol pointers to lazy
						   loaded dylibs */
/*
 * Section types to support thread local variables
 */
#define S_THREAD_LOCAL_REGULAR                   0x11  /* template of initial 
							  values for TLVs */
#define S_THREAD_LOCAL_ZEROFILL                  0x12  /* template of initial 
							  values for TLVs */
#define S_THREAD_LOCAL_VARIABLES                 0x13  /* TLV descriptors */
#define S_THREAD_LOCAL_VARIABLE_POINTERS         0x14  /* pointers to TLV 
                                                          descriptors */
#define S_THREAD_LOCAL_INIT_FUNCTION_POINTERS    0x15  /* functions to call
							  to initialize TLV
							  values */
#define S_INIT_FUNC_OFFSETS                      0x16  /* 32-bit offsets to
							  initializers */

/*
 * Constants for the section attributes part of the flags field of a section
 * structure.
 */
#define SECTION_ATTRIBUTES_USR	 0xff000000	/* User setable attributes */
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000	/* section contains only true
						   machine instructions */
#define S_ATTR_NO_TOC 		 0x40000000	/* section contains coalesced
						   symbols that are not to be
						   in a ranlib table of
						   contents */
#define S_ATTR_STRIP_STATIC_SYMS 0x20000000	/* ok to strip static symbols
						   in this section in files
						   with the MH_DYLDLINK flag */
#define S_ATTR_NO_DEAD_STRIP	 0x10000000	/* no dead stripping */
#define S_ATTR_LIVE_SUPPORT	 0x08000000	/* blocks are live if they
						   reference live blocks */
#define S_ATTR_SELF_MODIFYING_CODE 0x04000000	/* Used with i386 code stubs
						   written on by dyld */
/*
 * If a segment contains any sections marked with S_ATTR_DEBUG then all
 * sections in that segment must have this attribute.  No section other than
 * a section marked with this attribute may reference the contents of this
 * section.  A section with this attribute may contain no symbols and must have
 * a section type S_REGULAR.  The static linker will not copy section contents
 * from sections with this attribute into its output file.  These sections
 * generally contain DWARF debugging info.
 */ 
#define	S_ATTR_DEBUG		 0x02000000	/* a debug section */
#define SECTION_ATTRIBUTES_SYS	 0x00ffff00	/* system setable attributes */
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400	/* section contains some
						   machine instructions */
#define S_ATTR_EXT_RELOC	 0x00000200	/* section has external
						   relocation entries */
#define S_ATTR_LOC_RELOC	 0x00000100	/* section has local
						   relocation entries */


/*
 * The names of segments and sections in them are mostly meaningless to the
 * link-editor.  But there are few things to support traditional UNIX
 * executables that require the link-editor and assembler to use some names
 * agreed upon by convention.
 *
 * The initial protection of the "__TEXT" segment has write protection turned
 * off (not writeable).
 *
 * The link-editor will allocate common symbols at the end of the "__common"
 * section in the "__DATA" segment.  It will create the section and segment
 * if needed.
 */

/* The currently known segment names and the section names in those segments */

#define	SEG_PAGEZERO	"__PAGEZERO"	/* the pagezero segment which has no */
					/* protections and catches NULL */
					/* references for MH_EXECUTE files */


#define	SEG_TEXT	"__TEXT"	/* the tradition UNIX text segment */
#define	SECT_TEXT	"__text"	/* the real text part of the text */
					/* section no headers, and no padding */
#define SECT_FVMLIB_INIT0 "__fvmlib_init0"	/* the fvmlib initialization */
						/*  section */
#define SECT_FVMLIB_INIT1 "__fvmlib_init1"	/* the section following the */
					        /*  fvmlib initialization */
						/*  section */

#define	SEG_DATA	"__DATA"	/* the tradition UNIX data segment */
#define	SECT_DATA	"__data"	/* the real initialized data section */
					/* no padding, no bss overlap */
#define	SECT_BSS	"__bss"		/* the real uninitialized data section*/
					/* no padding */
#define SECT_COMMON	"__common"	/* the section common symbols are */
					/* allocated in by the link editor */

#define	SEG_OBJC	"__OBJC"	/* objective-C runtime segment */
#define SECT_OBJC_SYMBOLS "__symbol_table"	/* symbol table */
#define SECT_OBJC_MODULES "__module_info"	/* module information */
#define SECT_OBJC_STRINGS "__selector_strs"	/* string table */
#define SECT_OBJC_REFS "__selector_refs"	/* string table */

#define	SEG_ICON	 "__ICON"	/* the icon segment */
#define	SECT_ICON_HEADER "__header"	/* the icon headers */
#define	SECT_ICON_TIFF   "__tiff"	/* the icons in tiff format */

#define	SEG_LINKEDIT	"__LINKEDIT"	/* the segment containing all structs */
					/* created and maintained by the link */
					/* editor.  Created with -seglinkedit */
					/* option to ld(1) for MH_EXECUTE and */
					/* FVMLIB file types only */

#define SEG_UNIXSTACK	"__UNIXSTACK"	/* the unix stack segment */

#define SEG_IMPORT	"__IMPORT"	/* the segment for the self (dyld) */
					/* modifing code stubs that has read, */
					/* write and execute permissions */

typedef int             vm_prot_t;

/*
 *	Protection values, defined as bits within the vm_prot_t type
 */

#define VM_PROT_NONE    ((vm_prot_t) 0x00)

#define VM_PROT_READ    ((vm_prot_t) 0x01)      /* read permission */
#define VM_PROT_WRITE   ((vm_prot_t) 0x02)      /* write permission */
#define VM_PROT_EXECUTE ((vm_prot_t) 0x04)      /* execute permission */

#define VMPROT_TO_PROT(x)            (MAYBE_MAP_FLAG((x), VM_PROT_READ, PROT_READ) | \
                                      MAYBE_MAP_FLAG((x), VM_PROT_WRITE, PROT_WRITE) | \
                                      MAYBE_MAP_FLAG((x), VM_PROT_EXECUTE, PROT_EXEC))

/*
 * Dynamically linked shared libraries are identified by two things.  The
 * pathname (the name of the library as found for execution), and the
 * compatibility version number.  The pathname must match and the compatibility
 * number in the user of the library must be greater than or equal to the
 * library being used.  The time stamp is used to record the time a library was
 * built and copied into user so it can be use to determined if the library used
 * at runtime is exactly the same as used to built the program.
 */
struct dylib {
    union lc_str  name;			/* library's path name */
    uint32_t timestamp;			/* library's build time stamp */
    uint32_t current_version;		/* library's current version number */
    uint32_t compatibility_version;	/* library's compatibility vers number*/
};

/*
 * A dynamically linked shared library (filetype == MH_DYLIB in the mach header)
 * contains a dylib_command (cmd == LC_ID_DYLIB) to identify the library.
 * An object that uses a dynamically linked shared library also contains a
 * dylib_command (cmd == LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, or
 * LC_REEXPORT_DYLIB) for each library it uses.
 */
struct dylib_command {
	uint32_t	cmd;		/* LC_ID_DYLIB, LC_LOAD_{,WEAK_}DYLIB,
					   LC_REEXPORT_DYLIB */
	uint32_t	cmdsize;	/* includes pathname string */
	struct dylib	dylib;		/* the library identification */
};

/*
 * The symtab_command contains the offsets and sizes of the link-edit 4.3BSD
 * "stab" style symbol table information as described in the header files
 * <nlist.h> and <stab.h>.
 */
struct symtab_command {
	uint32_t	cmd;		/* LC_SYMTAB */
	uint32_t	cmdsize;	/* sizeof(struct symtab_command) */
	uint32_t	symoff;		/* symbol table offset */
	uint32_t	nsyms;		/* number of symbol table entries */
	uint32_t	stroff;		/* string table offset */
	uint32_t	strsize;	/* string table size in bytes */
};

/*
 * Format of a symbol table entry of a Mach-O file for 32-bit architectures.
 * Modified from the BSD format.  The modifications from the original format
 * were changing n_other (an unused field) to n_sect and the addition of the
 * N_SECT type.  These modifications are required to support symbols in a larger
 * number of sections not just the three sections (text, data and bss) in a BSD
 * file.
 */
struct nlist {
	union {
#ifndef __LP64__
		char *n_name;	/* for use when in-core */
#endif
		uint32_t n_strx;	/* index into the string table */
	} n_un;
	uint8_t n_type;		/* type flag, see below */
	uint8_t n_sect;		/* section number or NO_SECT */
	int16_t n_desc;		/* see <mach-o/stab.h> */
	uint32_t n_value;	/* value of this symbol (or stab offset) */
};

/*
 * This is the symbol table entry structure for 64-bit architectures.
 */
struct nlist_64 {
    union {
        uint32_t  n_strx; /* index into the string table */
    } n_un;
    uint8_t n_type;        /* type flag, see below */
    uint8_t n_sect;        /* section number or NO_SECT */
    uint16_t n_desc;       /* see <mach-o/stab.h> */
    uint64_t n_value;      /* value of this symbol (or stab offset) */
};

/*
 * This is the second set of the symbolic information which is used to support
 * the data structures for the dynamically link editor.
 *
 * The original set of symbolic information in the symtab_command which contains
 * the symbol and string tables must also be present when this load command is
 * present.  When this load command is present the symbol table is organized
 * into three groups of symbols:
 *	local symbols (static and debugging symbols) - grouped by module
 *	defined external symbols - grouped by module (sorted by name if not lib)
 *	undefined external symbols (sorted by name if MH_BINDATLOAD is not set,
 *	     			    and in order the were seen by the static
 *				    linker if MH_BINDATLOAD is set)
 * In this load command there are offsets and counts to each of the three groups
 * of symbols.
 *
 * This load command contains a the offsets and sizes of the following new
 * symbolic information tables:
 *	table of contents
 *	module table
 *	reference symbol table
 *	indirect symbol table
 * The first three tables above (the table of contents, module table and
 * reference symbol table) are only present if the file is a dynamically linked
 * shared library.  For executable and object modules, which are files
 * containing only one module, the information that would be in these three
 * tables is determined as follows:
 * 	table of contents - the defined external symbols are sorted by name
 *	module table - the file contains only one module so everything in the
 *		       file is part of the module.
 *	reference symbol table - is the defined and undefined external symbols
 *
 * For dynamically linked shared library files this load command also contains
 * offsets and sizes to the pool of relocation entries for all sections
 * separated into two groups:
 *	external relocation entries
 *	local relocation entries
 * For executable and object modules the relocation entries continue to hang
 * off the section structures.
 */
struct dysymtab_command {
    uint32_t cmd;	/* LC_DYSYMTAB */
    uint32_t cmdsize;	/* sizeof(struct dysymtab_command) */

    /*
     * The symbols indicated by symoff and nsyms of the LC_SYMTAB load command
     * are grouped into the following three groups:
     *    local symbols (further grouped by the module they are from)
     *    defined external symbols (further grouped by the module they are from)
     *    undefined symbols
     *
     * The local symbols are used only for debugging.  The dynamic binding
     * process may have to use them to indicate to the debugger the local
     * symbols for a module that is being bound.
     *
     * The last two groups are used by the dynamic binding process to do the
     * binding (indirectly through the module table and the reference symbol
     * table when this is a dynamically linked shared library file).
     */
    uint32_t ilocalsym;	/* index to local symbols */
    uint32_t nlocalsym;	/* number of local symbols */

    uint32_t iextdefsym;/* index to externally defined symbols */
    uint32_t nextdefsym;/* number of externally defined symbols */

    uint32_t iundefsym;	/* index to undefined symbols */
    uint32_t nundefsym;	/* number of undefined symbols */

    /*
     * For the for the dynamic binding process to find which module a symbol
     * is defined in the table of contents is used (analogous to the ranlib
     * structure in an archive) which maps defined external symbols to modules
     * they are defined in.  This exists only in a dynamically linked shared
     * library file.  For executable and object modules the defined external
     * symbols are sorted by name and is use as the table of contents.
     */
    uint32_t tocoff;	/* file offset to table of contents */
    uint32_t ntoc;	/* number of entries in table of contents */

    /*
     * To support dynamic binding of "modules" (whole object files) the symbol
     * table must reflect the modules that the file was created from.  This is
     * done by having a module table that has indexes and counts into the merged
     * tables for each module.  The module structure that these two entries
     * refer to is described below.  This exists only in a dynamically linked
     * shared library file.  For executable and object modules the file only
     * contains one module so everything in the file belongs to the module.
     */
    uint32_t modtaboff;	/* file offset to module table */
    uint32_t nmodtab;	/* number of module table entries */

    /*
     * To support dynamic module binding the module structure for each module
     * indicates the external references (defined and undefined) each module
     * makes.  For each module there is an offset and a count into the
     * reference symbol table for the symbols that the module references.
     * This exists only in a dynamically linked shared library file.  For
     * executable and object modules the defined external symbols and the
     * undefined external symbols indicates the external references.
     */
    uint32_t extrefsymoff;	/* offset to referenced symbol table */
    uint32_t nextrefsyms;	/* number of referenced symbol table entries */

    /*
     * The sections that contain "symbol pointers" and "routine stubs" have
     * indexes and (implied counts based on the size of the section and fixed
     * size of the entry) into the "indirect symbol" table for each pointer
     * and stub.  For every section of these two types the index into the
     * indirect symbol table is stored in the section header in the field
     * reserved1.  An indirect symbol table entry is simply a 32bit index into
     * the symbol table to the symbol that the pointer or stub is referring to.
     * The indirect symbol table is ordered to match the entries in the section.
     */
    uint32_t indirectsymoff; /* file offset to the indirect symbol table */
    uint32_t nindirectsyms;  /* number of indirect symbol table entries */

    /*
     * To support relocating an individual module in a library file quickly the
     * external relocation entries for each module in the library need to be
     * accessed efficiently.  Since the relocation entries can't be accessed
     * through the section headers for a library file they are separated into
     * groups of local and external entries further grouped by module.  In this
     * case the presents of this load command who's extreloff, nextrel,
     * locreloff and nlocrel fields are non-zero indicates that the relocation
     * entries of non-merged sections are not referenced through the section
     * structures (and the reloff and nreloc fields in the section headers are
     * set to zero).
     *
     * Since the relocation entries are not accessed through the section headers
     * this requires the r_address field to be something other than a section
     * offset to identify the item to be relocated.  In this case r_address is
     * set to the offset from the vmaddr of the first LC_SEGMENT command.
     * For MH_SPLIT_SEGS images r_address is set to the the offset from the
     * vmaddr of the first read-write LC_SEGMENT command.
     *
     * The relocation entries are grouped by module and the module table
     * entries have indexes and counts into them for the group of external
     * relocation entries for that the module.
     *
     * For sections that are merged across modules there must not be any
     * remaining external relocation entries for them (for merged sections
     * remaining relocation entries must be local).
     */
    uint32_t extreloff;	/* offset to external relocation entries */
    uint32_t nextrel;	/* number of external relocation entries */

    /*
     * All the local relocation entries are grouped together (they are not
     * grouped by their module since they are only used if the object is moved
     * from it staticly link edited address).
     */
    uint32_t locreloff;	/* offset to local relocation entries */
    uint32_t nlocrel;	/* number of local relocation entries */

};

/*
 * The linkedit_data_command contains the offsets and sizes of a blob
 * of data in the __LINKEDIT segment.  
 */
struct linkedit_data_command {
    uint32_t	cmd;		/* LC_CODE_SIGNATURE, LC_SEGMENT_SPLIT_INFO,
				   LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
				   LC_DYLIB_CODE_SIGN_DRS, LC_ATOM_INFO,
				   LC_LINKER_OPTIMIZATION_HINT,
				   LC_DYLD_EXPORTS_TRIE, or
				   LC_DYLD_CHAINED_FIXUPS. */
    uint32_t	cmdsize;	/* sizeof(struct linkedit_data_command) */
    uint32_t	dataoff;	/* file offset of data in __LINKEDIT segment */
    uint32_t	datasize;	/* file size of data in __LINKEDIT segment  */
};

// header of the LC_DYLD_CHAINED_FIXUPS payload
struct dyld_chained_fixups_header
{
    uint32_t    fixups_version;    // 0
    uint32_t    starts_offset;     // offset of dyld_chained_starts_in_image in chain_data
    uint32_t    imports_offset;    // offset of imports table in chain_data
    uint32_t    symbols_offset;    // offset of symbol strings in chain_data
    uint32_t    imports_count;     // number of imported symbol names
    uint32_t    imports_format;    // DYLD_CHAINED_IMPORT*
    uint32_t    symbols_format;    // 0 => uncompressed, 1 => zlib compressed
};

// This struct is embedded in LC_DYLD_CHAINED_FIXUPS payload
struct dyld_chained_starts_in_image
{
    uint32_t    seg_count;
    uint32_t    seg_info_offset[1];  // each entry is offset into this struct for that segment
    // followed by pool of dyld_chain_starts_in_segment data
};

// This struct is embedded in dyld_chain_starts_in_image
// and passed down to the kernel for page-in linking
struct dyld_chained_starts_in_segment
{
    uint32_t    size;               // size of this (amount kernel needs to copy)
    uint16_t    page_size;          // 0x1000 or 0x4000
    uint16_t    pointer_format;     // DYLD_CHAINED_PTR_*
    uint64_t    segment_offset;     // offset in memory to start of segment
    uint32_t    max_valid_pointer;  // for 32-bit OS, any value beyond this is not a pointer
    uint16_t    page_count;         // how many pages are in array
    uint16_t    page_start[1];      // each entry is offset in each page of first element in chain
                                    // or DYLD_CHAINED_PTR_START_NONE if no fixups on page
 // uint16_t    chain_starts[1];    // some 32-bit formats may require multiple starts per page.
                                    // for those, if high bit is set in page_starts[], then it
                                    // is index into chain_starts[] which is a list of starts
                                    // the last of which has the high bit set
};

enum {
    DYLD_CHAINED_PTR_START_NONE   = 0xFFFF, // used in page_start[] to denote a page with no fixups
    DYLD_CHAINED_PTR_START_MULTI  = 0x8000, // used in page_start[] to denote a page which has multiple starts
    DYLD_CHAINED_PTR_START_LAST   = 0x8000, // used in chain_starts[] to denote last start in list for page
};

// This struct is embedded in __TEXT,__chain_starts section in firmware
struct dyld_chained_starts_offsets
{
    uint32_t    pointer_format;     // DYLD_CHAINED_PTR_32_FIRMWARE
    uint32_t    starts_count;       // number of starts in array
    uint32_t    chain_starts[1];    // array chain start offsets
};


// values for dyld_chained_starts_in_segment.pointer_format
enum {
    DYLD_CHAINED_PTR_ARM64E                 =  1,    // stride 8, unauth target is vmaddr
    DYLD_CHAINED_PTR_64                     =  2,    // target is vmaddr
    DYLD_CHAINED_PTR_32                     =  3,
    DYLD_CHAINED_PTR_32_CACHE               =  4,
    DYLD_CHAINED_PTR_32_FIRMWARE            =  5,
    DYLD_CHAINED_PTR_64_OFFSET              =  6,    // target is vm offset
    DYLD_CHAINED_PTR_ARM64E_OFFSET          =  7,    // old name
    DYLD_CHAINED_PTR_ARM64E_KERNEL          =  7,    // stride 4, unauth target is vm offset
    DYLD_CHAINED_PTR_64_KERNEL_CACHE        =  8,
    DYLD_CHAINED_PTR_ARM64E_USERLAND        =  9,    // stride 8, unauth target is vm offset
    DYLD_CHAINED_PTR_ARM64E_FIRMWARE        = 10,    // stride 4, unauth target is vmaddr
    DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE    = 11,    // stride 1, x86_64 kernel caches
    DYLD_CHAINED_PTR_ARM64E_USERLAND24      = 12,    // stride 8, unauth target is vm offset, 24-bit bind
    DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE    = 13,    // stride 8, regular/auth targets both vm offsets.  Only A keys supported
};

// DYLD_CHAINED_PTR_ARM64E
struct dyld_chained_ptr_arm64e_rebase
{
    uint64_t    target   : 43,
                high8    :  8,
                next     : 11,    // 4 or 8-byte stide
                bind     :  1,    // == 0
                auth     :  1;    // == 0
};

// DYLD_CHAINED_PTR_ARM64E
struct dyld_chained_ptr_arm64e_bind
{
    uint64_t    ordinal   : 16,
                zero      : 16,
                addend    : 19,    // +/-256K
                next      : 11,    // 4 or 8-byte stide
                bind      :  1,    // == 1
                auth      :  1;    // == 0
};

// DYLD_CHAINED_PTR_ARM64E
struct dyld_chained_ptr_arm64e_auth_rebase
{
    uint64_t    target    : 32,   // runtimeOffset
                diversity : 16,
                addrDiv   :  1,
                key       :  2,
                next      : 11,    // 4 or 8-byte stide
                bind      :  1,    // == 0
                auth      :  1;    // == 1
};

// DYLD_CHAINED_PTR_ARM64E
struct dyld_chained_ptr_arm64e_auth_bind
{
    uint64_t    ordinal   : 16,
                zero      : 16,
                diversity : 16,
                addrDiv   :  1,
                key       :  2,
                next      : 11,    // 4 or 8-byte stide
                bind      :  1,    // == 1
                auth      :  1;    // == 1
};

// DYLD_CHAINED_PTR_64/DYLD_CHAINED_PTR_64_OFFSET
struct dyld_chained_ptr_64_rebase
{
    uint64_t    target    : 36,    // 64GB max image size (DYLD_CHAINED_PTR_64 => vmAddr, DYLD_CHAINED_PTR_64_OFFSET => runtimeOffset)
                high8     :  8,    // top 8 bits set to this (DYLD_CHAINED_PTR_64 => after slide added, DYLD_CHAINED_PTR_64_OFFSET => before slide added)
                reserved  :  7,    // all zeros
                next      : 12,    // 4-byte stride
                bind      :  1;    // == 0
};


// DYLD_CHAINED_PTR_ARM64E_USERLAND24
struct dyld_chained_ptr_arm64e_bind24
{
    uint64_t    ordinal   : 24,
                zero      :  8,
                addend    : 19,    // +/-256K
                next      : 11,    // 8-byte stide
                bind      :  1,    // == 1
                auth      :  1;    // == 0
};

// DYLD_CHAINED_PTR_ARM64E_USERLAND24
struct dyld_chained_ptr_arm64e_auth_bind24
{
    uint64_t    ordinal   : 24,
                zero      :  8,
                diversity : 16,
                addrDiv   :  1,
                key       :  2,
                next      : 11,    // 8-byte stide
                bind      :  1,    // == 1
                auth      :  1;    // == 1
};


// DYLD_CHAINED_PTR_64
struct dyld_chained_ptr_64_bind
{
    uint64_t    ordinal   : 24,
                addend    :  8,   // 0 thru 255
                reserved  : 19,   // all zeros
                next      : 12,   // 4-byte stride
                bind      :  1;   // == 1
};

// DYLD_CHAINED_PTR_64_KERNEL_CACHE, DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE
struct dyld_chained_ptr_64_kernel_cache_rebase
{
    uint64_t    target     : 30,   // basePointers[cacheLevel] + target
                cacheLevel :  2,   // what level of cache to bind to (indexes a mach_header array)
                diversity  : 16,
                addrDiv    :  1,
                key        :  2,
                next       : 12,    // 1 or 4-byte stide
                isAuth     :  1;    // 0 -> not authenticated.  1 -> authenticated
};

// DYLD_CHAINED_PTR_32
// Note: for DYLD_CHAINED_PTR_32 some non-pointer values are co-opted into the chain
// as out of range rebases.  If an entry in the chain is > max_valid_pointer, then it
// is not a pointer.  To restore the value, subtract off the bias, which is
// (64MB+max_valid_pointer)/2.
struct dyld_chained_ptr_32_rebase
{
    uint32_t    target    : 26,   // vmaddr, 64MB max image size
                next      :  5,   // 4-byte stride
                bind      :  1;   // == 0
};

// DYLD_CHAINED_PTR_32
struct dyld_chained_ptr_32_bind
{
    uint32_t    ordinal   : 20,
                addend    :  6,   // 0 thru 63
                next      :  5,   // 4-byte stride
                bind      :  1;   // == 1
};

// DYLD_CHAINED_PTR_32_CACHE
struct dyld_chained_ptr_32_cache_rebase
{
    uint32_t    target    : 30,   // 1GB max dyld cache TEXT and DATA
                next      :  2;   // 4-byte stride
};


// DYLD_CHAINED_PTR_32_FIRMWARE
struct dyld_chained_ptr_32_firmware_rebase
{
    uint32_t    target   : 26,   // 64MB max firmware TEXT and DATA
                next     :  6;   // 4-byte stride
};

// DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE
struct dyld_chained_ptr_arm64e_shared_cache_rebase
{
    uint64_t    runtimeOffset   : 34,   // offset from the start of the shared cache
                high8           :  8,
                unused          : 10,
                next            : 11,   // 8-byte stide
                auth            :  1;   // == 0
};

// DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE
struct dyld_chained_ptr_arm64e_shared_cache_auth_rebase
{
    uint64_t    runtimeOffset   : 34,   // offset from the start of the shared cache
                diversity       : 16,
                addrDiv         :  1,
                keyIsData       :  1,   // implicitly always the 'A' key.  0 -> IA.  1 -> DA
                next            : 11,   // 8-byte stide
                auth            :  1;   // == 1
};



// values for dyld_chained_fixups_header.imports_format
enum {
    DYLD_CHAINED_IMPORT          = 1,
    DYLD_CHAINED_IMPORT_ADDEND   = 2,
    DYLD_CHAINED_IMPORT_ADDEND64 = 3,
};

// DYLD_CHAINED_IMPORT
struct dyld_chained_import
{
    uint32_t    lib_ordinal :  8,   // -15 .. 240 (0xF1 .. 0xF0)
                weak_import :  1,
                name_offset : 23;
};

// DYLD_CHAINED_IMPORT_ADDEND
struct dyld_chained_import_addend
{
    uint32_t    lib_ordinal :  8,   // -15 .. 240 (0xF1 .. 0xF0)
                weak_import :  1,
                name_offset : 23;
    int32_t     addend;
};

// DYLD_CHAINED_IMPORT_ADDEND64
struct dyld_chained_import_addend64
{
    uint64_t    lib_ordinal : 16,   // -15 .. 65520 (0xFFF1 .. 0xFFF0)
                weak_import :  1,
                reserved    : 15,
                name_offset : 32;
    uint64_t    addend;
};

/* -------------------- Options -------------------- */

static void *malloc_wrapper(void *arg, size_t size)             { return malloc(size); }
static void  free_wrapper(void *arg, void *ptr)                 { return free(ptr); }
static void *realloc_wrapper(void *arg, void *ptr, size_t size) { return realloc(ptr, size); }

static int     open_wrapper(void *arg, const char *name)                { return open(name, O_RDONLY); }
static int     close_wrapper(void *arg, int fd)                         { return close(fd); }
static ssize_t read_wrapper(void *arg, int fd, void *buf, size_t count) { return read(fd, buf, count); }
static int     fstat_wrapper(void *arg, int fd, struct stat *statbuf)   { return fstat(fd, statbuf); }

static void *mmap_wrapper(void *arg, void *hint, size_t length, int prot, int flags, int fd, off_t offset) { return mmap(hint, length, prot, flags, fd, offset); }
static int   munmap_wrapper(void *arg, void *addr, size_t size)                                            { return munmap(addr, size); }
static int   mprotect_wrapper(void *arg, void *addr, size_t size, int flags)                               { return mprotect(addr, size, flags); }

static void *call_wrapper(void *arg, void *addr) { return ((void *(*)(void))addr)(); }

static struct ulinker_ops uo = {
    .uo_malloc = malloc_wrapper,
    .uo_free = free_wrapper,
    .uo_realloc = realloc_wrapper,

    .uo_open = open_wrapper,
    .uo_close = close_wrapper,
    .uo_read = read_wrapper,
    .uo_fstat = fstat_wrapper,

    .uo_mmap = mmap_wrapper,
    .uo_munmap = munmap_wrapper,
    .uo_mprotect = mprotect_wrapper,

    .uo_call = call_wrapper,
};

/* ------------------- GDB JIT Interface ------------------- */

enum jit_actions {
    JIT_NOACTION = 0,
    JIT_REGISTER_FN = 1,
    JIT_UNREGISTER_FN = 2
};

struct jit_code_entry {
    struct jit_code_entry *next;
    struct jit_code_entry *prev;
    const char *symfile_addr;
    uint64_t symfile_size;
};

struct jit_debug_descriptor {
    uint32_t version;                 // must be 1
    uint32_t action_flag;              // jit_actions
    struct jit_code_entry *relevant_entry;
    struct jit_code_entry *first_entry;
};

struct jit_debug_descriptor __jit_debug_descriptor = {
    .version = 1,
    .action_flag = JIT_NOACTION,
    .relevant_entry = NULL,
    .first_entry = NULL
};

__attribute__((noinline))
void __jit_debug_register_code(void) {
    // GDB sets a breakpoint here
}

pthread_mutex_t __jit_debug_descriptor_mtx = PTHREAD_MUTEX_INITIALIZER;

void gdb_jit_register(struct jit_code_entry *entry)
{
    pthread_mutex_lock(&__jit_debug_descriptor_mtx);

    entry->prev = NULL;
    entry->next = __jit_debug_descriptor.first_entry;
    if (entry->next)
        entry->next->prev = entry;
    __jit_debug_descriptor.first_entry = entry;
    __jit_debug_descriptor.relevant_entry = entry;
    __jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
    __jit_debug_register_code();

    pthread_mutex_unlock(&__jit_debug_descriptor_mtx);
}

void gdb_jit_unregister(struct jit_code_entry *entry)
{
    pthread_mutex_lock(&__jit_debug_descriptor_mtx);
    
    if (entry->prev)
        entry->prev->next = entry->next;
    else
        __jit_debug_descriptor.first_entry = entry->next;

    if (entry->next)
        entry->next->prev = entry->prev;

    __jit_debug_descriptor.relevant_entry = entry;
    __jit_debug_descriptor.action_flag = JIT_UNREGISTER_FN;
    __jit_debug_register_code();
    pthread_mutex_unlock(&__jit_debug_descriptor_mtx);
}

/* -------------------- Linker Instance -------------------- */

struct ulinker {
    pthread_mutex_t  ul_dlinfo_mtx;
    struct dlinfo   *ul_dlinfo;
    void *ul_private;

    void *ul_cb_arg;
    void (*ul_cb_mmap)(struct ulinker *, void *, size_t, void *, size_t, int);
};

#define DLINFO_LOCK(_ul)   pthread_mutex_lock(&(_ul)->ul_dlinfo_mtx)
#define DLINFO_UNLOCK(_ul) pthread_mutex_unlock(&(_ul)->ul_dlinfo_mtx)

/* -------------------- Dynamic Library Info -------------------- */

enum {
    DLT_EXTERNAL,
    DLT_ELF64,
    DLT_MACHO64
};

#define DLINFO_NAME_LEN 128
#define DLINFO_PATH_LEN 4096
struct dlinfo {
    struct dlinfo *dl_next;
    struct ulinker *dl_linker; // back pointer to owning linker
    void *dl_private;
    char dl_name[DLINFO_NAME_LEN];
    char dl_pathname[DLINFO_PATH_LEN];
    uint64_t dl_refcnt;
    int dl_type; // DLT_*
    int dl_arch;
    int dl_abi;
    void *dl_base;
    size_t dl_size;
    int dl_linked;
    int dl_constructors_called;
    int dl_flags;  // flags as passed by dlopen()
    int dl_emu;  // != 0 means no PROT_EXEC mappings, made to run under emulator
};

#define DLBASE(_ptr) ((struct dlinfo *)_ptr)
#define DLPRIVATE(_ptr) (DLBASE(_ptr)->dl_private)

struct dlinfo_elf64 {
    struct dlinfo dle64_base;

    Elf64_Ehdr dle64_ehdr;
    Elf64_Addr dle64_load_bias;
    const Elf64_Phdr *dle64_phdr;
    size_t dle64_phdr_count;

    char  *dle64_strtab;
    size_t dle64_strtab_size;

    Elf64_Sym *dle64_symtab;

    size_t      dle64_gnu_nbucket;
    uint32_t   *dle64_gnu_bucket;
    uint32_t   *dle64_gnu_chain;
    uint32_t    dle64_gnu_maskwords;
    uint32_t    dle64_gnu_shift2;
    Elf64_Addr *dle64_gnu_bloom_filter;

    size_t dle64_plt_rel_type;
    union {
        void *dle64_plt_rel_opaque;
        Elf64_Rela *dle64_plt_rel_rela;
        Elf64_Rel  *dle64_plt_rel_rel;
    };
    size_t dle64_plt_rel_size;

    Elf64_Rela *dle64_rela;
    size_t      dle64_rela_size;
    size_t      dle64_rela_ent;

    void       *dle64_init_array;
    size_t      dle64_init_array_size;
    void       *dle64_init_func;

    void       *dle64_shsymtab_map;
    size_t      dle64_shsymtab_map_size;
    Elf64_Sym  *dle64_shsymtab;
    size_t      dle64_shsymtab_size;

    void       *dle64_shstrtab_map;
    size_t      dle64_shstrtab_map_size;
    const char *dle64_shstrtab;
    size_t      dle64_shstrtab_size;

    struct jit_code_entry dle64_jit_entry;
};

struct dlinfo_macho64 {
    struct dlinfo dlm64_base;

    struct mach_header_64 dlm64_hdr;
    size_t dlm64_sizeoflc;
    uintptr_t dlm64_load_bias;
    uintptr_t dlm64_pagezero;
    const struct load_command *dlm64_lc;

    struct nlist_64 *dlm64_symtab;
    size_t           dlm64_symtabsize;
    const char      *dlm64_strtab;
    size_t           dlm64_strtabsize;

    size_t dlm64_dysymtab_iextdefsym;
    size_t dlm64_dysymtab_nextdefsym;

    const struct dyld_chained_starts_in_image *dlm64_chainedfixups_starts;

    const char *dlm64_chainedfixups_strtab;
    int         dlm64_chainedfixups_imports_format;
    size_t      dlm64_chainedfixups_imports_count;
    void       *dlm64_chainedfixups_imports;

    struct jit_code_entry dlm64_jit_entry;

    // image slot reuse, see macho_slot_take()
    struct macho_slot_key {
        dev_t    dev;
        ino_t    ino;
        off_t    size;
        long     mtime_sec;
        long     mtime_nsec;
        size_t   load_size;
    } dlm64_slot_key;
    int dlm64_slot_cacheable;   // native image with executable segments, all mapped
    int dlm64_slot_reused;      // executable segments came from a parked slot
};

struct dlinfo_external {
    struct dlinfo dlext_base;
    void *(*dlext_dlsym)(void *, const char *, int);
};

static pthread_mutex_t g_external_dlinfos_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct dlinfo_external *g_external_dlinfos = NULL;

/* -------------------- DWARF fixup helpers -------------------- */

static const uint8_t *read_uleb128(const uint8_t *p, const uint8_t *end, uint64_t *out)
{
    uint64_t result = 0;
    int shift = 0;
    while (p < end) {
        uint8_t byte = *p++;
        result |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    *out = result;
    return p;
}

static void dwarf_fixup_debug_line(uint8_t *sec, size_t size, int64_t load_bias)
{
    uint8_t *p = sec;
    uint8_t *end = sec + size;

    while (p < end) {
        uint8_t *cu_start = p;
        if (p + 4 > end) break;
        uint32_t unit_length = *(uint32_t *)p; p += 4;
        if (unit_length == 0xFFFFFFFF) break; // DWARF64 — skip
        uint8_t *cu_end = p + unit_length;
        if (cu_end > end) break;

        if (p + 2 > cu_end) break;
        uint16_t version = *(uint16_t *)p; p += 2;

        if (p + 4 > cu_end) break;
        uint32_t header_length = *(uint32_t *)p; p += 4;
        uint8_t *after_header = p + header_length;

        if (p + 4 > after_header) { p = cu_end; continue; }
        uint8_t min_inst_length = *p++;
        if (version >= 4) p++; // max_ops_per_instruction
        p++; // default_is_stmt
        p++; // line_base (signed)
        uint8_t line_range = *p++;
        uint8_t opcode_base = *p++;

        uint8_t std_opcode_lengths[256];
        for (int i = 1; i < opcode_base && p < after_header; i++)
            std_opcode_lengths[i] = *p++;

        // skip directory table (null-terminated strings, ended by empty string)
        while (p < after_header && *p != 0) {
            while (p < after_header && *p != 0) p++;
            if (p < after_header) p++; // skip null terminator
        }
        if (p < after_header) p++; // skip final null

        // skip file table
        while (p < after_header && *p != 0) {
            while (p < after_header && *p != 0) p++;
            if (p < after_header) p++; // skip null
            uint64_t tmp;
            p = (uint8_t *)read_uleb128(p, after_header, &tmp); // dir index
            p = (uint8_t *)read_uleb128(p, after_header, &tmp); // time
            p = (uint8_t *)read_uleb128(p, after_header, &tmp); // size
        }
        if (p < after_header) p++; // skip final null

        // walk line program opcodes
        while (p < cu_end) {
            uint8_t op = *p++;
            if (op == 0) {
                // extended opcode
                uint64_t ext_len;
                p = (uint8_t *)read_uleb128(p, cu_end, &ext_len);
                uint8_t *ext_end = p + ext_len;
                if (ext_end > cu_end) break;
                if (ext_len >= 1) {
                    uint8_t ext_op = *p;
                    if (ext_op == 0x02 && ext_len >= 9) { // DW_LNE_set_address
                        uint64_t *addr = (uint64_t *)(p + 1);
                        *addr += load_bias;
                    }
                }
                p = ext_end;
            } else if (op < opcode_base) {
                // standard opcode — skip ULEB128 args
                uint64_t tmp;
                for (int i = 0; i < std_opcode_lengths[op]; i++)
                    p = (uint8_t *)read_uleb128(p, cu_end, &tmp);
            }
            // else: special opcode, no operands
        }

        p = cu_end;
    }
}

static void dwarf_fixup_debug_aranges(uint8_t *sec, size_t size, int64_t load_bias)
{
    uint8_t *p = sec;
    uint8_t *end = sec + size;

    while (p < end) {
        if (p + 4 > end) break;
        uint32_t unit_length = *(uint32_t *)p; p += 4;
        if (unit_length == 0xFFFFFFFF) break;
        uint8_t *cu_end = p + unit_length;
        if (cu_end > end) break;

        if (p + 2 > cu_end) break;
        /* uint16_t version = */ p += 2;
        /* uint32_t debug_info_offset = */ p += 4;
        if (p + 2 > cu_end) break;
        uint8_t addr_size = *p++;
        uint8_t seg_size = *p++;

        // align to 2 * addr_size
        size_t tuple_size = 2 * addr_size;
        size_t off = (p - sec);
        size_t align = tuple_size;
        size_t aligned_off = (off + align - 1) & ~(align - 1);
        p = sec + aligned_off;

        while (p + tuple_size <= cu_end) {
            if (addr_size == 8) {
                uint64_t addr = *(uint64_t *)p;
                uint64_t len  = *(uint64_t *)(p + 8);
                if (addr == 0 && len == 0) { p += tuple_size; break; }
                *(uint64_t *)p = addr + load_bias;
            } else if (addr_size == 4) {
                uint32_t addr = *(uint32_t *)p;
                uint32_t len  = *(uint32_t *)(p + 4);
                if (addr == 0 && len == 0) { p += tuple_size; break; }
                *(uint32_t *)p = addr + (uint32_t)load_bias;
            }
            p += tuple_size;
        }

        p = cu_end;
    }
}

static const uint8_t *skip_form(const uint8_t *p, const uint8_t *end, uint64_t form,
                                 uint8_t addr_size, uint16_t version, int64_t load_bias)
{
    uint64_t tmp;
    switch (form) {
    case 0x01: // DW_FORM_addr
        if (p + addr_size <= end) {
            if (addr_size == 8) *(uint64_t *)((uintptr_t)p) += load_bias;
            else if (addr_size == 4) *(uint32_t *)((uintptr_t)p) += (uint32_t)load_bias;
        }
        return p + addr_size;
    case 0x03: // DW_FORM_block2
        if (p + 2 > end) return end;
        { uint16_t len = *(uint16_t *)p; return p + 2 + len; }
    case 0x04: // DW_FORM_block4
        if (p + 4 > end) return end;
        { uint32_t len = *(uint32_t *)p; return p + 4 + len; }
    case 0x05: return p + 2;  // DW_FORM_data2
    case 0x06: return p + 4;  // DW_FORM_data4
    case 0x07: return p + 8;  // DW_FORM_data8
    case 0x08: // DW_FORM_string (null-terminated)
        while (p < end && *p) p++;
        return p < end ? p + 1 : end;
    case 0x09: // DW_FORM_block (ULEB128 length + data)
        p = read_uleb128(p, end, &tmp);
        return p + tmp;
    case 0x0a: // DW_FORM_block1
        if (p >= end) return end;
        { uint8_t len = *p; return p + 1 + len; }
    case 0x0b: return p + 1;  // DW_FORM_data1
    case 0x0c: return p + 1;  // DW_FORM_flag
    case 0x0d: // DW_FORM_sdata
        return (uint8_t *)read_uleb128(p, end, &tmp);
    case 0x0e: return p + 4;  // DW_FORM_strp
    case 0x0f: // DW_FORM_udata
        return (uint8_t *)read_uleb128(p, end, &tmp);
    case 0x10: // DW_FORM_ref_addr
        return p + ((version >= 3) ? 4 : addr_size);
    case 0x11: return p + 1;  // DW_FORM_ref1
    case 0x12: return p + 2;  // DW_FORM_ref2
    case 0x13: return p + 4;  // DW_FORM_ref4
    case 0x14: return p + 8;  // DW_FORM_ref8
    case 0x15: // DW_FORM_ref_udata
        return (uint8_t *)read_uleb128(p, end, &tmp);
    case 0x16: // DW_FORM_indirect
    {
        p = read_uleb128(p, end, &tmp);
        return skip_form(p, end, tmp, addr_size, version, load_bias);
    }
    case 0x17: return p + 4;  // DW_FORM_sec_offset
    case 0x18: // DW_FORM_exprloc
        p = read_uleb128(p, end, &tmp);
        return p + tmp;
    case 0x19: return p;      // DW_FORM_flag_present (0 bytes)
    case 0x20: return p + 8;  // DW_FORM_ref_sig8
    default:
        return end; // unknown form — bail
    }
}

static void dwarf_fixup_debug_info(uint8_t *info, size_t info_size,
                                    uint8_t *abbrev, size_t abbrev_size,
                                    int64_t load_bias)
{
    uint8_t *p = info;
    uint8_t *end = info + info_size;
    uint8_t *abbrev_end = abbrev + abbrev_size;

    while (p < end) {
        if (p + 4 > end) break;
        uint32_t unit_length = *(uint32_t *)p; p += 4;
        if (unit_length == 0xFFFFFFFF) break;
        uint8_t *cu_end = p + unit_length;
        if (cu_end > end) break;

        if (p + 2 > cu_end) break;
        uint16_t version = *(uint16_t *)p; p += 2;
        if (p + 4 > cu_end) break;
        uint32_t abbrev_offset = *(uint32_t *)p; p += 4;
        if (p + 1 > cu_end) break;
        uint8_t addr_size = *p++;

        while (p < cu_end) {
            uint64_t abbrev_code;
            p = (uint8_t *)read_uleb128(p, cu_end, &abbrev_code);
            if (abbrev_code == 0) continue; // null DIE

            // find abbreviation in .debug_abbrev
            const uint8_t *ap = abbrev + abbrev_offset;
            uint64_t ab_code, ab_tag, ab_children;
            for (;;) {
                if (ap >= abbrev_end) goto next_cu;
                ap = read_uleb128(ap, abbrev_end, &ab_code);
                if (ab_code == 0) goto next_cu; // end of table
                ap = read_uleb128(ap, abbrev_end, &ab_tag);
                if (ap >= abbrev_end) goto next_cu;
                ab_children = *ap++;

                if (ab_code == abbrev_code) break;

                // skip attributes
                for (;;) {
                    uint64_t attr, form;
                    ap = read_uleb128(ap, abbrev_end, &attr);
                    ap = read_uleb128(ap, abbrev_end, &form);
                    if (attr == 0 && form == 0) break;
                }
            }

            // process attributes for this DIE
            for (;;) {
                uint64_t attr, form;
                ap = read_uleb128(ap, abbrev_end, &attr);
                ap = read_uleb128(ap, abbrev_end, &form);
                if (attr == 0 && form == 0) break;
                p = (uint8_t *)skip_form(p, cu_end, form, addr_size, version, load_bias);
            }
        }
next_cu:
        p = cu_end;
    }
}

static void dwarf_fixup_debug_ranges(uint8_t *sec, size_t size, int64_t load_bias, uint8_t addr_size)
{
    uint8_t *p = sec;
    uint8_t *end = sec + size;

    if (addr_size == 8) {
        while (p + 16 <= end) {
            uint64_t start = *(uint64_t *)p;
            uint64_t stop  = *(uint64_t *)(p + 8);
            if (start == 0 && stop == 0) { p += 16; continue; }
            if (start == 0xFFFFFFFFFFFFFFFF) {
                // base address selection — fix the base in 'stop'
                *(uint64_t *)(p + 8) = stop + load_bias;
            } else {
                *(uint64_t *)p = start + load_bias;
                *(uint64_t *)(p + 8) = stop + load_bias;
            }
            p += 16;
        }
    } else if (addr_size == 4) {
        while (p + 8 <= end) {
            uint32_t start = *(uint32_t *)p;
            uint32_t stop  = *(uint32_t *)(p + 4);
            if (start == 0 && stop == 0) { p += 8; continue; }
            if (start == 0xFFFFFFFF) {
                *(uint32_t *)(p + 4) = stop + (uint32_t)load_bias;
            } else {
                *(uint32_t *)p = start + (uint32_t)load_bias;
                *(uint32_t *)(p + 4) = stop + (uint32_t)load_bias;
            }
            p += 8;
        }
    }
}

/* -------------------- Private functions -------------------- */

int create_elf64_jit_entry(struct dlinfo_elf64 *dl, const char *filename)
{
    int ret = -1;
    int fd = -1;
    
    fd = uo.uo_open(DLPRIVATE(dl), filename);
    if (-1 == fd) {
        ERROR("failed to open ELF \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }
    
    struct stat statbuf;
    if (-1 == uo.uo_fstat(DLPRIVATE(dl), fd, &statbuf)) {
        ERROR("failed to stat ELF \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }

    char *data = uo.uo_malloc(DLPRIVATE(dl), statbuf.st_size);
    if (!data) {
        ERROR("failed to allocate debug data for ELF \"%s\": -%d (%s)", filename, ENOMEM, strerror(ENOMEM));
        goto out;
    }

    size_t total_read = 0;
    while (total_read < statbuf.st_size) {
        ssize_t current_read = uo.uo_read(DLPRIVATE(dl), fd, data + total_read, statbuf.st_size - total_read);
        if (current_read == -1 || current_read == 0) {
            ERROR("failed to read ELF \"%s\": -%d (%s)", filename, errno, strerror(errno));
            goto out;
        }
        total_read += current_read;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;

    // GDB expects an ET_REL
    ehdr->e_type = ET_REL;


    Elf64_Phdr *phdr0 = (Elf64_Phdr *)(data + ehdr->e_phoff);
    for (Elf64_Phdr *phdr = phdr0; phdr < &phdr0[ehdr->e_phnum]; phdr++) {
        if (phdr->p_memsz || phdr->p_vaddr) {
            phdr->p_vaddr += dl->dle64_load_bias;
            phdr->p_paddr += dl->dle64_load_bias;
        }
    }

    Elf64_Shdr *symtab = NULL;
    const char *strtab_str = NULL;

    Elf64_Shdr *shdr0 = (Elf64_Shdr *)(data + ehdr->e_shoff);
    const char *shnstrtab_str = (const char *)(data + shdr0[ehdr->e_shstrndx].sh_offset);

    // find symbol and string table
    for (Elf64_Shdr *shdr = shdr0; shdr < &shdr0[ehdr->e_shnum]; shdr++) {

        if (0 == strcmp(&shnstrtab_str[shdr->sh_name], ".symtab"))
            symtab = shdr;
        else if (0 == strcmp(&shnstrtab_str[shdr->sh_name], ".strtab"))
            strtab_str = (const char *)(data + shdr->sh_offset);
    }

    // fixup symbols to be calculated from section start
    if (symtab) {
        Elf64_Sym *sym0 = (Elf64_Sym *)(data + symtab->sh_offset);
        for (Elf64_Sym *sym = sym0; sym < &sym0[symtab->sh_size / sizeof(Elf64_Sym)]; sym++) {
            if (sym->st_shndx < ehdr->e_shnum)
                sym->st_value -= shdr0[sym->st_shndx].sh_addr;
        }
    }

    // fixup DWARF debug sections to match loaded addresses
    {
        Elf64_Shdr *debug_line = NULL, *debug_aranges = NULL;
        Elf64_Shdr *debug_info = NULL, *debug_abbrev = NULL;
        Elf64_Shdr *debug_ranges = NULL;

        for (Elf64_Shdr *shdr = shdr0; shdr < &shdr0[ehdr->e_shnum]; shdr++) {
            const char *name = &shnstrtab_str[shdr->sh_name];
            if (0 == strcmp(name, ".debug_line"))        debug_line = shdr;
            else if (0 == strcmp(name, ".debug_aranges")) debug_aranges = shdr;
            else if (0 == strcmp(name, ".debug_info"))    debug_info = shdr;
            else if (0 == strcmp(name, ".debug_abbrev"))  debug_abbrev = shdr;
            else if (0 == strcmp(name, ".debug_ranges"))  debug_ranges = shdr;
        }

        int64_t bias = (int64_t)dl->dle64_load_bias;

        if (debug_line)
            dwarf_fixup_debug_line((uint8_t *)(data + debug_line->sh_offset),
                                   debug_line->sh_size, bias);
        if (debug_aranges)
            dwarf_fixup_debug_aranges((uint8_t *)(data + debug_aranges->sh_offset),
                                      debug_aranges->sh_size, bias);
        if (debug_info && debug_abbrev)
            dwarf_fixup_debug_info((uint8_t *)(data + debug_info->sh_offset),
                                   debug_info->sh_size,
                                   (uint8_t *)(data + debug_abbrev->sh_offset),
                                   debug_abbrev->sh_size, bias);
        if (debug_ranges)
            dwarf_fixup_debug_ranges((uint8_t *)(data + debug_ranges->sh_offset),
                                     debug_ranges->sh_size, bias, 8);
    }

    // fixup section addresses to absolute values
    for (Elf64_Shdr *shdr = shdr0; shdr < &shdr0[ehdr->e_shnum]; shdr++) {
        if (shdr->sh_addr) {
            shdr->sh_addr += dl->dle64_load_bias;
        }
    }

    dl->dle64_jit_entry.symfile_addr = data;
    dl->dle64_jit_entry.symfile_size = statbuf.st_size;
    ret = 0;

out:
    if (ret) {
        if (data) {
            uo.uo_free(DLPRIVATE(dl), data);
            data = NULL;
        }      
    }
    if (fd != -1) {
        uo.uo_close(DLPRIVATE(dl), fd);
        fd = -1;
    }
    return ret;
}

int create_macho64_jit_entry(struct dlinfo_macho64 *dl, const char *filename)
{
    int ret = -1;
    int fd = -1;
    char *data = NULL;

    fd = uo.uo_open(DLPRIVATE(dl), filename);
    if (-1 == fd) {
        ERROR("failed to open Mach-O \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }

    struct stat statbuf;
    if (-1 == uo.uo_fstat(DLPRIVATE(dl), fd, &statbuf)) {
        ERROR("failed to stat Mach-O \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }

    data = uo.uo_malloc(DLPRIVATE(dl), statbuf.st_size);
    if (!data) {
        ERROR("failed to allocate debug data for Mach-O \"%s\": -%d (%s)", filename, ENOMEM, strerror(ENOMEM));
        goto out;
    }

    size_t total_read = 0;
    while (total_read < statbuf.st_size) {
        ssize_t current_read = uo.uo_read(DLPRIVATE(dl), fd, data + total_read, statbuf.st_size - total_read);
        if (current_read == -1 || current_read == 0) {
            ERROR("failed to read Mach-O \"%s\": -%d (%s)", filename, errno, strerror(errno));
            goto out;
        }
        total_read += current_read;
    }

    struct mach_header_64 *hdr = (struct mach_header_64 *)data;
    int64_t bias = (int64_t)dl->dlm64_load_bias;

    // LLDB reads symtab from the buffer for MH_OBJECT, but from process
    // memory for MH_EXECUTE/MH_DYLIB — same reason ELF uses ET_REL
    hdr->filetype = MH_OBJECT;

    struct section_64 *debug_line = NULL, *debug_aranges = NULL;
    struct section_64 *debug_info = NULL, *debug_abbrev = NULL;
    struct section_64 *debug_ranges = NULL;

    struct load_command *lc = (struct load_command *)(data + sizeof(struct mach_header_64));
    for (size_t i = 0; i < hdr->ncmds; ++i, lc = (struct load_command *)((uintptr_t)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *sc = (struct segment_command_64 *)lc;

            if (sc->initprot == 0 && sc->maxprot == 0 && sc->vmaddr == 0)
                continue;

            sc->vmaddr += bias;

            struct section_64 *sect = (struct section_64 *)((uintptr_t)sc + sizeof(struct segment_command_64));
            for (uint32_t j = 0; j < sc->nsects; ++j, ++sect) {
                sect->addr += bias;

                if (0 == strncmp(sect->segname, "__DWARF", 16)) {
                    if (0 == strncmp(sect->sectname, "__debug_line", 16))
                        debug_line = sect;
                    else if (0 == strncmp(sect->sectname, "__debug_aranges", 16))
                        debug_aranges = sect;
                    else if (0 == strncmp(sect->sectname, "__debug_info", 16))
                        debug_info = sect;
                    else if (0 == strncmp(sect->sectname, "__debug_abbrev", 16))
                        debug_abbrev = sect;
                    else if (0 == strncmp(sect->sectname, "__debug_ranges", 16))
                        debug_ranges = sect;
                }
            }
        }

        if (lc->cmd == LC_SYMTAB) {
            struct symtab_command *symc = (struct symtab_command *)lc;
            struct nlist_64 *syms = (struct nlist_64 *)(data + symc->symoff);
            for (uint32_t j = 0; j < symc->nsyms; ++j) {
                if (syms[j].n_sect != 0)
                    syms[j].n_value += bias;
            }
        }
    }

    if (debug_line)
        dwarf_fixup_debug_line((uint8_t *)(data + debug_line->offset),
                               debug_line->size, bias);
    if (debug_aranges)
        dwarf_fixup_debug_aranges((uint8_t *)(data + debug_aranges->offset),
                                  debug_aranges->size, bias);
    if (debug_info && debug_abbrev)
        dwarf_fixup_debug_info((uint8_t *)(data + debug_info->offset),
                               debug_info->size,
                               (uint8_t *)(data + debug_abbrev->offset),
                               debug_abbrev->size, bias);
    if (debug_ranges)
        dwarf_fixup_debug_ranges((uint8_t *)(data + debug_ranges->offset),
                                 debug_ranges->size, bias, 8);

    dl->dlm64_jit_entry.symfile_addr = data;
    dl->dlm64_jit_entry.symfile_size = statbuf.st_size;
    ret = 0;

out:
    if (ret) {
        if (data) {
            uo.uo_free(DLPRIVATE(dl), data);
            data = NULL;
        }
    }
    if (fd != -1) {
        uo.uo_close(DLPRIVATE(dl), fd);
        fd = -1;
    }
    return ret;
}

static struct dlinfo *find_library(struct ulinker *ulinker, const char *name, int refcnt)
{
    struct dlinfo *dl = NULL;

    DLINFO_LOCK(ulinker);
    for (struct dlinfo *curr = ulinker->ul_dlinfo; curr != NULL; curr = curr->dl_next) {
        if (0 == strcmp(curr->dl_name, name)) {
            curr->dl_refcnt += refcnt;
            dl = curr;
            break;
        }
    }
    DLINFO_UNLOCK(ulinker);

    return dl;
}

static const Elf64_Phdr *elf64_find_phdr(struct dlinfo_elf64 *dl, long ptype) {
    for (const Elf64_Phdr *curr_phdr = dl->dle64_phdr; curr_phdr < (dl->dle64_phdr + dl->dle64_phdr_count); curr_phdr++) {
        if (curr_phdr->p_type == ptype)
            return curr_phdr;
    }

    return NULL;
}

static void free_elf64_library(struct dlinfo_elf64 *dl)
{
    if (dl->dle64_jit_entry.symfile_addr) {
        gdb_jit_unregister(&dl->dle64_jit_entry);
        uo.uo_free(DLPRIVATE(dl), (void *)dl->dle64_jit_entry.symfile_addr);
        dl->dle64_jit_entry.symfile_addr = NULL;
        dl->dle64_jit_entry.symfile_size = 0;
    }
    
    if (dl->dle64_shstrtab_map && dl->dle64_shstrtab_map_size) {
        uo.uo_munmap(DLPRIVATE(dl), dl->dle64_shstrtab_map, dl->dle64_shstrtab_map_size);
        dl->dle64_shstrtab_map = NULL;
        dl->dle64_shstrtab = NULL;
        dl->dle64_shstrtab_map_size = dl->dle64_shstrtab_size = 0;
    }

    if (dl->dle64_shsymtab_map && dl->dle64_shsymtab_map_size) {
        uo.uo_munmap(DLPRIVATE(dl), dl->dle64_shsymtab_map, dl->dle64_shsymtab_map_size);
        dl->dle64_shsymtab_map = NULL;
        dl->dle64_shsymtab = NULL;
        dl->dle64_shsymtab_map_size = dl->dle64_shsymtab_size = 0;
    }

    if (DLBASE(dl)->dl_base) {
        uo.uo_munmap(DLPRIVATE(dl), DLBASE(dl)->dl_base, DLBASE(dl)->dl_size);
        DLBASE(dl)->dl_base = NULL;
        DLBASE(dl)->dl_size = 0;
    }
    uo.uo_free(DLPRIVATE(dl), dl);
}

static char *string_elf64(struct dlinfo_elf64 *dl, Elf64_Word idx)
{
    if (idx >= dl->dle64_strtab_size)
        return NULL;
    return dl->dle64_strtab + idx;
}

static void *lookup_symbol_external(struct dlinfo_external *dl, const char *s, struct dlinfo *caller)
{
    return dl->dlext_dlsym(DLPRIVATE(dl), s, caller ? caller->dl_emu : 0);
}

static void *lookup_symbol_elf64_dyn(struct dlinfo_elf64 *dl, const char *s)
{
    uint32_t hash = 5381;
    const uint8_t* name = (const uint8_t *)(s);
    while (*name != 0) {
        hash += (hash << 5) + *name++; // h*33 + c = h + h * 32 + c = h + h << 5 + c
    }

    uint32_t h2 = hash >> dl->dle64_gnu_shift2;

    uint32_t bloom_mask_bits = sizeof(Elf64_Addr)*8;
    uint32_t word_num = (hash / bloom_mask_bits) & dl->dle64_gnu_maskwords;
    Elf64_Addr bloom_word = dl->dle64_gnu_bloom_filter[word_num];

    // test against bloom filter
    if ((1 & (bloom_word >> (hash % bloom_mask_bits)) & (bloom_word >> (h2 % bloom_mask_bits))) == 0) {
        return NULL;
    }

    // bloom test says "probably yes"...
    uint32_t n = dl->dle64_gnu_bucket[hash % dl->dle64_gnu_nbucket];

    if (n == 0) {
        return NULL;
    }

    do {
        Elf64_Sym *sym = dl->dle64_symtab + n;
        if (((dl->dle64_gnu_chain[n] ^ hash) >> 1) == 0 &&
            strcmp(string_elf64(dl, sym->st_name), s) == 0 &&
            (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL || ELF64_ST_BIND(sym->st_info) == STB_WEAK) &&
            sym->st_shndx != SHN_UNDEF) {
            Elf64_Sym *sym = &dl->dle64_symtab[n];
            if (ELF64_ST_TYPE(sym->st_info) == STT_GNU_IFUNC) {
                typedef Elf64_Addr (*ifunc_resolver_t)(void);
                ifunc_resolver_t ifunc_resolver = (ifunc_resolver_t)(dl->dle64_load_bias + sym->st_value);
                Elf64_Addr ifunc_addr = ifunc_resolver();
                
                return (void *)ifunc_addr;
            }

            return (void *)(dl->dle64_load_bias + sym->st_value);
        }
    } while ((dl->dle64_gnu_chain[n++] & 1) == 0);

    return NULL;
}

static void *lookup_symbol_elf64_all(struct dlinfo_elf64 *dl, const char *s)
{
    if (!dl->dle64_shsymtab || !dl->dle64_shstrtab)
        goto dyn;
    
    for (int i = 0; i < dl->dle64_shsymtab_size; i++) {
        // out-of-bounds in string table?
        if (dl->dle64_shsymtab[i].st_name >= dl->dle64_shstrtab_size)
            continue;
        
        const char *name = dl->dle64_shstrtab + dl->dle64_shsymtab[i].st_name;
        if (strcmp(name, s) == 0) {
            return (void *)(dl->dle64_load_bias + dl->dle64_shsymtab[i].st_value);
        }
    }

dyn:
    return lookup_symbol_elf64_dyn(dl, s);
}

static void *lookup_symbol_elf64(struct dlinfo_elf64 *dl, const char *s, int all)
{
    if (!all)
        return lookup_symbol_elf64_dyn(dl, s);
    return lookup_symbol_elf64_all(dl, s);
}

static void *lookup_symbol_macho64(struct dlinfo_macho64 *dl, const char *s)
{
    if (!dl->dlm64_symtab)
        return NULL;

    for (size_t i = dl->dlm64_dysymtab_iextdefsym; i < (dl->dlm64_dysymtab_iextdefsym + dl->dlm64_dysymtab_nextdefsym) && i < dl->dlm64_symtabsize; i++) {
        struct nlist_64 *n = &dl->dlm64_symtab[i];
        if (!n->n_un.n_strx)
            continue;
        const char *sym_name = &dl->dlm64_strtab[n->n_un.n_strx];
        // skip the leading underscore
        if (0 == strcmp(s, sym_name+1))
            return (void *)(dl->dlm64_load_bias + n->n_value);
    }

    return NULL;
}

static void *lookup_symbol(struct dlinfo *dl, const char *s, int all, struct dlinfo *caller)
{
    if (dl->dl_type == DLT_EXTERNAL)
        return lookup_symbol_external((struct dlinfo_external *)dl, s, caller);
    if (dl->dl_type == DLT_ELF64)
        return lookup_symbol_elf64((struct dlinfo_elf64 *)dl, s, all);
    if (dl->dl_type == DLT_MACHO64)
        return lookup_symbol_macho64((struct dlinfo_macho64 *)dl, s);
    ERROR("symbol \"%s\" not found: unknown dl_type %d", s, dl->dl_type);
    return NULL;
}

static void reverse_lookup_symbol_elf64(struct dlinfo_elf64 *dl, const void *addr,
                                        const char **out_name, void **out_addr)
{
    uintptr_t target = (uintptr_t)addr;
    uintptr_t best = 0;
    const char *best_name = NULL;

    if (dl->dle64_shsymtab && dl->dle64_shstrtab) {
        for (size_t i = 0; i < dl->dle64_shsymtab_size; i++) {
            Elf64_Sym *sym = &dl->dle64_shsymtab[i];
            if (sym->st_shndx == SHN_UNDEF || sym->st_value == 0)
                continue;
            unsigned char type = ELF64_ST_TYPE(sym->st_info);
            if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE)
                continue;
            if (sym->st_name >= dl->dle64_shstrtab_size)
                continue;
            uintptr_t saddr = dl->dle64_load_bias + sym->st_value;
            if (saddr <= target && saddr > best) {
                best = saddr;
                best_name = dl->dle64_shstrtab + sym->st_name;
            }
        }
    } else if (dl->dle64_gnu_bucket && dl->dle64_gnu_chain) {
        for (uint32_t b = 0; b < dl->dle64_gnu_nbucket; b++) {
            uint32_t idx = dl->dle64_gnu_bucket[b];
            if (idx == 0)
                continue;
            do {
                Elf64_Sym *sym = &dl->dle64_symtab[idx];
                if (sym->st_shndx != SHN_UNDEF && sym->st_value != 0) {
                    uintptr_t saddr = dl->dle64_load_bias + sym->st_value;
                    if (saddr <= target && saddr > best) {
                        const char *name = string_elf64(dl, sym->st_name);
                        if (!name)
                            continue;
                        best = saddr;
                        best_name = name;
                    }
                }
            } while ((dl->dle64_gnu_chain[idx++] & 1) == 0);
        }
    }

    *out_name = best_name;
    *out_addr = best_name ? (void *)best : NULL;
}

static void reverse_lookup_symbol_macho64(struct dlinfo_macho64 *dl, const void *addr,
                                          const char **out_name, void **out_addr)
{
    uintptr_t target = (uintptr_t)addr;
    uintptr_t best = 0;
    const char *best_name = NULL;

    if (!dl->dlm64_symtab || !dl->dlm64_strtab)
        goto out;

    for (size_t i = 0; i < dl->dlm64_symtabsize; i++) {
        struct nlist_64 *n = &dl->dlm64_symtab[i];
        if ((n->n_type & 0xe0) != 0)
            continue;
        if ((n->n_type & 0x0e) != 0x0e)
            continue;
        if (n->n_value == 0 || !n->n_un.n_strx)
            continue;
        if (n->n_un.n_strx >= dl->dlm64_strtabsize)
            continue;
        uintptr_t saddr = dl->dlm64_load_bias + n->n_value;
        if (saddr <= target && saddr > best) {
            best = saddr;
            const char *name = &dl->dlm64_strtab[n->n_un.n_strx];
            best_name = (name[0] == '_') ? name + 1 : name;
        }
    }

out:
    *out_name = best_name;
    *out_addr = best_name ? (void *)best : NULL;
}

static size_t address_in_library(struct dlinfo *dl, const void *addr) {
    if ((uintptr_t)addr >= (uintptr_t)dl->dl_base &&
        (uintptr_t)addr < (uintptr_t)dl->dl_base + dl->dl_size) {
        return (uintptr_t)addr - (uintptr_t)dl->dl_base;
    }

    return SIZE_MAX;
}

static struct dlinfo *library_for_address_locked(struct ulinker *ulinker, const void *addr) {
    struct dlinfo *dl = ulinker->ul_dlinfo;
    while (dl && dl->dl_type != DLT_EXTERNAL) {
        if (address_in_library(dl, addr) != SIZE_MAX)
            return dl;
        dl = dl->dl_next;
    }
    return NULL;
}

static void *dlsym_internal(struct ulinker *ulinker, void *handle, const char *symbol, void *lr, int external, int all, struct dlinfo *caller)
{
    DLINFO_LOCK(ulinker);
    struct dlinfo *dl = (struct dlinfo *)handle;
    if (RTLD_DEFAULT == handle) {
        dl = ulinker->ul_dlinfo;
    }
    else if (RTLD_NEXT == handle) {
        dl = library_for_address_locked(ulinker, lr);
        if (dl)
            dl = dl->dl_next;
    }

    while (dl) {
        if (!external && dl->dl_type == DLT_EXTERNAL)
            break;
        void *a = lookup_symbol(dl, symbol, all, caller);
        if (a) {
            DLINFO_UNLOCK(ulinker);
            return a;
        }
        dl = dl->dl_next;
    }
    DLINFO_UNLOCK(ulinker);
    return NULL;
}

static struct dlinfo *load_elf64_library(struct ulinker *ulinker, const char *filename, int fd, ssize_t file_size, unsigned char *ident, int flags)
{
    struct dlinfo *ret = NULL;
    struct dlinfo_elf64 *dl = NULL;
    void  *phdr_mmap = MAP_FAILED;
    size_t phdr_size = 0;
    void  *shdr_mmap = MAP_FAILED;
    size_t shdr_size = 0;

    if (!(dl = uo.uo_malloc(ulinker->ul_private, sizeof(struct dlinfo_elf64)))) {
        ERROR("failed to open \"%s\": -%d (%s)", filename, ENOMEM, strerror(ENOMEM));
        goto out;
    }

    memset(dl, 0, sizeof(*dl));
    DLBASE(dl)->dl_type = DLT_ELF64;
    DLBASE(dl)->dl_flags = flags;
    DLBASE(dl)->dl_emu = !(flags & RTLD_NOEMU) && !noemu;
    strncpy(DLBASE(dl)->dl_pathname, filename, sizeof(DLBASE(dl)->dl_pathname)-1);
    DLPRIVATE(dl) = ulinker->ul_private;

    memcpy(dl->dle64_ehdr.e_ident, ident, EI_NIDENT);

    if ((sizeof(Elf64_Ehdr) - EI_NIDENT) != uo.uo_read(ulinker->ul_private, fd, ((uint8_t *)&dl->dle64_ehdr) + EI_NIDENT, sizeof(Elf64_Ehdr) - EI_NIDENT)) {
        ERROR("failed to read ELF header \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }

    if (dl->dle64_ehdr.e_type == ET_EXEC) {
        ERROR("failed to load ELF \"%s\": executable files are only supported as PIE!", filename);
        goto out;
    }

    if (dl->dle64_ehdr.e_type != ET_DYN) {
        ERROR("failed to load ELF \"%s\": unexpected e_type: %d", filename, dl->dle64_ehdr.e_type);
        goto out;
    }

    dl->dle64_phdr_count = dl->dle64_ehdr.e_phnum;

    if (dl->dle64_ehdr.e_machine == EM_X86_64)
        DLBASE(dl)->dl_arch = ULARCH_X86_64;
    else if (dl->dle64_ehdr.e_machine == EM_AARCH64)
        DLBASE(dl)->dl_arch = ULARCH_ARM64;
    DLBASE(dl)->dl_abi = ULABI_LINUX;

    // map the program header

    Elf64_Addr pg_min = page_start(dl->dle64_ehdr.e_phoff);
    Elf64_Addr pg_max = page_end(dl->dle64_ehdr.e_phoff + (dl->dle64_phdr_count * sizeof(Elf64_Phdr)));
    Elf64_Addr pg_offset = page_offset(dl->dle64_ehdr.e_phoff);

    phdr_size = pg_max - pg_min;

    if (MAP_FAILED == (phdr_mmap = uo.uo_mmap(ulinker->ul_private, NULL, phdr_size, PROT_READ, MAP_PRIVATE, fd, pg_min))) {
        ERROR("failed to map ELF phdr \"%s\" (size 0x%zx): -%d (%s)", filename, phdr_size, errno, strerror(errno));
        goto out;
    }

    Elf64_Phdr *phdr = (Elf64_Phdr *)((uintptr_t)phdr_mmap + pg_offset);

    // reserve virtual range for library

    Elf64_Addr min_vaddr = UINTPTR_MAX;
    Elf64_Addr max_vaddr = 0;

    int found_pt_load = 0;
    for (size_t i = 0; i < dl->dle64_phdr_count; ++i) {
        const Elf64_Phdr* curr_phdr = &phdr[i];

        if (curr_phdr->p_type != PT_LOAD) {
            continue;
        }
        found_pt_load = 1;

        if (curr_phdr->p_vaddr < min_vaddr) {
            min_vaddr = curr_phdr->p_vaddr;
        }

        if (curr_phdr->p_vaddr + curr_phdr->p_memsz > max_vaddr) {
            max_vaddr = curr_phdr->p_vaddr + curr_phdr->p_memsz;
        }
    }
    if (!found_pt_load) {
        min_vaddr = 0;
    }

    min_vaddr = page_start(min_vaddr);
    max_vaddr = page_end(max_vaddr);

    size_t load_size = max_vaddr - min_vaddr;

    void *base = uo.uo_mmap(ulinker->ul_private, NULL, load_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (MAP_FAILED == base) {
        ERROR("failed to map ELF \"%s\" (size 0x%zx): -%d (%s)", filename, load_size, errno, strerror(errno));
        goto out;
    }

    DLBASE(dl)->dl_base = base;
    DLBASE(dl)->dl_size = load_size;

    dl->dle64_load_bias = (uintptr_t)base - min_vaddr;

    // load segments

    for (size_t i = 0; i < dl->dle64_phdr_count; ++i) {
        const Elf64_Phdr* curr_phdr = &phdr[i];

        if (curr_phdr->p_type != PT_LOAD) {
            continue;
        }

        // Segment addresses in memory.
        Elf64_Addr seg_start = curr_phdr->p_vaddr + dl->dle64_load_bias;
        Elf64_Addr seg_end   = seg_start + curr_phdr->p_memsz;

        Elf64_Addr seg_page_start = page_start(seg_start);
        Elf64_Addr seg_page_end   = page_end(seg_end);

        Elf64_Addr seg_file_end   = seg_start + curr_phdr->p_filesz;

        // File offsets.
        Elf64_Addr file_start = curr_phdr->p_offset;
        Elf64_Addr file_end   = file_start + curr_phdr->p_filesz;

        Elf64_Addr file_page_start = page_start(file_start);
        Elf64_Addr file_length = file_end - file_page_start;

        if (file_size <= 0) {
            ERROR("failed to load ELF \"%s\": invalid size 0x%zx", filename, file_size);
            goto out;
        }

        if (file_end > (size_t)file_size) {
            ERROR("failed to load ELF \"%s\": load segment[%zd]:"
                " p_offset (%p) + p_filesz (%p) ( = %p) past end of file (0x%zx)",
                filename, i, (void *)curr_phdr->p_offset,
                (void *)curr_phdr->p_filesz,
                (void *)file_end, file_size);
            goto out;
        }

        if (file_length != 0) {
            void* seg_addr = uo.uo_mmap(ulinker->ul_private, (void *)seg_page_start,
                                file_length,
                                PFLAGS_TO_PROT(curr_phdr->p_flags) & ~(DLBASE(dl)->dl_emu ? PROT_EXEC : 0), // strip away PROT_EXEC if run on emulator
                                MAP_FIXED|MAP_PRIVATE,
                                fd,
                                file_page_start);
            if (seg_addr == MAP_FAILED) {
                ERROR("failed to map ELF \"%s\" segment %zd: -%d (%s)", filename, i, errno, strerror(errno));
                goto out;
            }
        }

        // if the segment is writable, and does not end on a page boundary,
        // zero-fill it until the page limit.
        if ((curr_phdr->p_flags & PF_W) != 0 && page_offset(seg_file_end) > 0) {
            memset((void *)seg_file_end, 0, page_size() - page_offset(seg_file_end));
        }

        seg_file_end = page_end(seg_file_end);

        // seg_file_end is now the first page address after the file
        // content. If seg_end is larger, we need to zero anything
        // between them. This is done by using a private anonymous
        // map for all extra pages.
        if (seg_page_end > seg_file_end) {
            void* zeromap = uo.uo_mmap(ulinker->ul_private, (void *)seg_file_end,
                                seg_page_end - seg_file_end,
                                PFLAGS_TO_PROT(curr_phdr->p_flags) & ~(DLBASE(dl)->dl_emu ? PROT_EXEC : 0), // strip away PROT_EXEC if run on emulator
                                MAP_FIXED|MAP_ANONYMOUS|MAP_PRIVATE,
                                -1,
                                0);
            if (zeromap == MAP_FAILED) {
                ERROR("failed to zero fill ELF \"%s\" segment %zd gap: -%d (%s)", filename, i, errno, strerror(errno));
                goto out;
            }
        }

    }

    // find phdr as it appears in the loaded segments

    const Elf64_Phdr *phdr_limit = phdr + dl->dle64_phdr_count;
    Elf64_Phdr *loaded_phdr = NULL;

    // If there is a PT_PHDR, use it directly.
    for (const Elf64_Phdr *curr_phdr = phdr; curr_phdr < phdr_limit; ++curr_phdr) {
        if (curr_phdr->p_type == PT_PHDR) {
            loaded_phdr = (Elf64_Phdr *)(dl->dle64_load_bias + curr_phdr->p_vaddr);
            break;
        }
    }

    // Otherwise, check the first loadable segment. If its file offset
    // is 0, it starts with the ELF header, and we can trivially find the
    // loaded program header from it.
    if (!loaded_phdr) {
        for (const Elf64_Phdr *curr_phdr = phdr; curr_phdr < phdr_limit; ++curr_phdr) {
            if (curr_phdr->p_type == PT_LOAD) {
                if (curr_phdr->p_offset == 0) {
                    Elf64_Addr elf_addr = dl->dle64_load_bias + curr_phdr->p_vaddr;
                    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr *)elf_addr;
                    Elf64_Addr offset = ehdr->e_phoff;
                    loaded_phdr = (Elf64_Phdr *)(elf_addr + offset);
                }
                break;
            }
        }
    }

    if (!loaded_phdr) {
        ERROR("failed to find loaded phdr for ELF \"%s\"", filename);
        goto out;
    }

    // Ensures that our program header is actually within a loadable
    // segment. This should help catch badly-formed ELF files that
    // would cause the linker to crash later when trying to access it.
    Elf64_Phdr *loaded_end = loaded_phdr + dl->dle64_phdr_count;
    for (Elf64_Phdr *curr_phdr = phdr; curr_phdr < phdr_limit; ++curr_phdr) {
        if (curr_phdr->p_type != PT_LOAD) {
            continue;
        }
        Elf64_Addr seg_start = curr_phdr->p_vaddr + dl->dle64_load_bias;
        Elf64_Addr seg_end = curr_phdr->p_filesz + seg_start;
        if (seg_start <= (Elf64_Addr)loaded_end && (Elf64_Addr)loaded_end <= seg_end) {
            dl->dle64_phdr = (const Elf64_Phdr *)loaded_phdr;
            break;
        }
    }

    if (!dl->dle64_phdr) {
        ERROR("failed to load ELF\"%s\": loaded phdr %p not in loadable segment", filename, loaded_phdr);
        goto out;
    }

    ret = (struct dlinfo *)dl;

    // parse dynamic table (optional)

    const Elf64_Phdr *dyn_phdr = elf64_find_phdr(dl, PT_DYNAMIC);
    if (!dyn_phdr) {
        goto out;
    }

    const Elf64_Dyn *dyn = (Elf64_Dyn *) (dl->dle64_load_bias + dyn_phdr->p_vaddr);
    for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB:
                dl->dle64_strtab = (char *)(dl->dle64_load_bias + d->d_un.d_ptr);
                break;
            
            case DT_STRSZ:
                dl->dle64_strtab_size = d->d_un.d_val;
                break;
            
            case DT_SYMTAB:
                dl->dle64_symtab = (Elf64_Sym *)(dl->dle64_load_bias + d->d_un.d_ptr);
                break;
            
            case DT_GNU_HASH:
                dl->dle64_gnu_nbucket = ((uint32_t *)(dl->dle64_load_bias + d->d_un.d_ptr))[0];
                // skip symndx
                dl->dle64_gnu_maskwords = ((uint32_t *)(dl->dle64_load_bias + d->d_un.d_ptr))[2];
                dl->dle64_gnu_shift2 = ((uint32_t *)(dl->dle64_load_bias + d->d_un.d_ptr))[3];

                dl->dle64_gnu_bloom_filter = (Elf64_Addr *)(dl->dle64_load_bias + d->d_un.d_ptr + 16);
                dl->dle64_gnu_bucket = (uint32_t *)(dl->dle64_gnu_bloom_filter + dl->dle64_gnu_maskwords);
                // amend chain for symndx = header[1]
                dl->dle64_gnu_chain = dl->dle64_gnu_bucket + dl->dle64_gnu_nbucket -
                    ((uint32_t *)(dl->dle64_load_bias + d->d_un.d_ptr))[1];

                // if (!powerof2(gnu_maskwords_)) {
                dl->dle64_gnu_maskwords--;
                break;
            
            case DT_PLTREL:
                dl->dle64_plt_rel_type = d->d_un.d_val;
                break;
            
            case DT_JMPREL:
                dl->dle64_plt_rel_opaque = (void *)(dl->dle64_load_bias + d->d_un.d_ptr);
                break;
            
            case DT_PLTRELSZ:
                dl->dle64_plt_rel_size = d->d_un.d_val;
                break;
            
            case DT_PLTGOT:
                // Ignore because RTLD_LAZY is not supported
                break;
            
            case DT_RELA:
                dl->dle64_rela = (Elf64_Rela *)(dl->dle64_load_bias + d->d_un.d_ptr);
                break;
            
            case DT_RELASZ:
                dl->dle64_rela_size = d->d_un.d_val;
                break;
            
            case DT_RELAENT:
                dl->dle64_rela_ent = d->d_un.d_val;
                break;

            case DT_INIT:
                dl->dle64_init_func = (void *)(dl->dle64_load_bias + d->d_un.d_ptr);
                break;

            case DT_INIT_ARRAY:
                dl->dle64_init_array = (void *)(dl->dle64_load_bias + d->d_un.d_ptr);
                break;

            case DT_INIT_ARRAYSZ:
                dl->dle64_init_array_size = d->d_un.d_val;
                break;
        }
    }

    // second pass - parse entries relying on strtab
    for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SONAME) {
            strncpy(DLBASE(dl)->dl_name, string_elf64(dl, d->d_un.d_val), DLINFO_NAME_LEN - 1);
            break;
        }
    }

    // parse symbol section (optional)

    if (dl->dle64_ehdr.e_shnum && dl->dle64_ehdr.e_shoff) {
        off_t shdr_off  = page_start(dl->dle64_ehdr.e_shoff);
        shdr_size = page_end(dl->dle64_ehdr.e_shoff + dl->dle64_ehdr.e_shnum * sizeof(Elf64_Shdr)) - shdr_off;
        shdr_mmap  = uo.uo_mmap(ulinker->ul_private, NULL, shdr_size, PROT_READ, MAP_PRIVATE, fd, shdr_off);
        if (shdr_mmap == MAP_FAILED) {
            ERROR("failed to allocate ELF \"%s\" section headers: -%d (%s)", filename, errno, strerror(errno));
            goto out;
        }

        Elf64_Shdr *shdr0 = (Elf64_Shdr *)((uint64_t)shdr_mmap + (dl->dle64_ehdr.e_shoff & (page_size() - 1)));

        int i = 0;
        for (Elf64_Shdr *shdr = shdr0; shdr < (shdr0 + dl->dle64_ehdr.e_shnum); shdr++, i++) {
            if (shdr->sh_type != SHT_SYMTAB)
                continue;

            // no symbols or no linked string table
            if (!shdr->sh_size || !shdr->sh_link)
                continue;

            // string table empty?
            Elf64_Shdr *link_shdr = &shdr0[shdr->sh_link];
            if (!link_shdr->sh_size)
                continue;

            off_t  shsymtab_off  = page_start(shdr->sh_offset);
            size_t shsymtab_size = page_end(shdr->sh_offset + shdr->sh_size) - shsymtab_off;
            void  *shsymtab_mmap = uo.uo_mmap(ulinker->ul_private, NULL, shsymtab_size, PROT_READ, MAP_PRIVATE, fd, shsymtab_off);
            if (shsymtab_mmap == MAP_FAILED) {
                ERROR("failed to allocate ELF \"%s\" .symtab section: -%d (%s)", filename, errno, strerror(errno));
                goto out;
            }

            dl->dle64_shsymtab_map = shsymtab_mmap;
            dl->dle64_shsymtab_map_size = shsymtab_size;
            dl->dle64_shsymtab = (Elf64_Sym *)((uint64_t)shsymtab_mmap + (shdr->sh_offset & (page_size() - 1)));
            dl->dle64_shsymtab_size = shdr->sh_size / sizeof(Elf64_Sym);

            off_t  shstrtab_off  = page_start(link_shdr->sh_offset);
            size_t shstrtab_size = page_end(link_shdr->sh_offset + link_shdr->sh_size) - shstrtab_off;
            void  *shstrtab_mmap = uo.uo_mmap(ulinker->ul_private, NULL, shstrtab_size, PROT_READ, MAP_PRIVATE, fd, shstrtab_off);
            if (shstrtab_mmap == MAP_FAILED) {
                ERROR("failed to allocate ELF \"%s\" .strtab section: -%d (%s)", filename, errno, strerror(errno));
                goto out;
            }

            dl->dle64_shstrtab_map = shstrtab_mmap;
            dl->dle64_shstrtab_map_size = shstrtab_size;
            dl->dle64_shstrtab = (const char *)((uint64_t)shstrtab_mmap + (link_shdr->sh_offset & (page_size() - 1)));
            dl->dle64_shstrtab_size = link_shdr->sh_size;

            break;
        }
    }

    if (0 == create_elf64_jit_entry(dl, filename))
        gdb_jit_register(&dl->dle64_jit_entry);

out:
    if (shdr_mmap != MAP_FAILED) {
        uo.uo_munmap(ulinker->ul_private, shdr_mmap, shdr_size);
        shdr_mmap = MAP_FAILED;
        shdr_size = 0;
    }
    if (phdr_mmap != MAP_FAILED) {
        uo.uo_munmap(ulinker->ul_private, phdr_mmap, phdr_size);
        phdr_mmap = MAP_FAILED;
        phdr_size = 0;
    }
    if (!ret) {
        if (dl) {
            free_elf64_library(dl);
            dl = NULL;
        }
    }
    return ret;
}

static struct dlinfo *load_elf_library(struct ulinker *ulinker, const char *filename, int fd, ssize_t file_size, int flags)
{
    struct dlinfo *ret = NULL;
    unsigned char ident[EI_NIDENT] = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3};

    if ((EI_NIDENT - 4) != uo.uo_read(ulinker->ul_private, fd, &ident[4], EI_NIDENT - 4)) {
        ERROR("failed to read rest of ELF e_ident \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }

    if (ident[EI_CLASS] == ELFCLASS64) {
        ret = load_elf64_library(ulinker, filename, fd, file_size, ident, flags);
    }
    else {
        ERROR("failed to load \"%s\": unsupported ELFCLASS %hhu", filename, ident[EI_CLASS]);
        goto out;
    }

out:
    return ret;
}

/*
 * Parked Mach-O image slots (Apple only).
 *
 * On iOS the kernel never frees an executable mapping of signed code: once
 * vm_map_entry_cs_associate() succeeds it marks the entry permanent
 * (vm_map_executable_immutable, always on in release kernels), and
 * vm_map_delete() then keeps a permanent entry in place with its protections
 * stripped to none instead of removing it ("A permanent entry can not be
 * removed, so leave it in place but remove all access permissions",
 * osfmk/vm/vm_map.c). So munmap() of a native image left its __TEXT behind as
 * dead 0/0 address space. TVM maps every native image once per exec and again
 * in every fork child, so each exec leaked its text for good: ~9 MB and two
 * map entries per `git` run, until iOS SIGKILLed the whole app (reproduced:
 * killed after ~2800 `git --version` runs, 6400 regions; the device test
 * suite died a little after its git tests).
 *
 * Instead of unmapping, an unloaded native image keeps its whole range, with
 * the executable segments still mapped, and is parked here keyed by the file's
 * identity. The next load of the same file takes the slot: its executable
 * segments are left as they are (same file, same bytes, still validly signed)
 * and every other segment is mapped afresh over the stale one with MAP_FIXED.
 * Address space is then bounded by the peak number of simultaneously loaded
 * copies of each image rather than by the number of loads ever made. The
 * parked slots' stale data pages stay resident until reuse; they are small.
 */
#if defined(__APPLE__)
#define MACHO_SLOTS_MAX 256

struct macho_slot {
    int in_use;
    struct macho_slot_key key;
    void *base;
};

static pthread_mutex_t g_macho_slots_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct macho_slot g_macho_slots[MACHO_SLOTS_MAX];

static int macho_slot_key_eq(const struct macho_slot_key *a, const struct macho_slot_key *b)
{
    return a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
           a->mtime_sec == b->mtime_sec && a->mtime_nsec == b->mtime_nsec &&
           a->load_size == b->load_size;
}

static void *macho_slot_take(const struct macho_slot_key *key)
{
    void *base = NULL;
    pthread_mutex_lock(&g_macho_slots_mtx);
    for (int i = 0; i < MACHO_SLOTS_MAX; i++) {
        if (g_macho_slots[i].in_use && macho_slot_key_eq(&g_macho_slots[i].key, key)) {
            base = g_macho_slots[i].base;
            g_macho_slots[i].in_use = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_macho_slots_mtx);
    return base;
}

static int macho_slot_put(const struct macho_slot_key *key, void *base)
{
    int ok = 0;
    pthread_mutex_lock(&g_macho_slots_mtx);
    for (int i = 0; i < MACHO_SLOTS_MAX; i++) {
        if (!g_macho_slots[i].in_use) {
            g_macho_slots[i].in_use = 1;
            g_macho_slots[i].key = *key;
            g_macho_slots[i].base = base;
            ok = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_macho_slots_mtx);
    return ok;
}
#endif

static void free_macho64_library(struct dlinfo_macho64 *dl)
{
    if (dl->dlm64_jit_entry.symfile_addr) {
        gdb_jit_unregister(&dl->dlm64_jit_entry);
        uo.uo_free(DLPRIVATE(dl), (void *)dl->dlm64_jit_entry.symfile_addr);
        dl->dlm64_jit_entry.symfile_addr = NULL;
        dl->dlm64_jit_entry.symfile_size = 0;
    }
    if (DLBASE(dl)->dl_base) {
#if defined(__APPLE__)
        if (!(dl->dlm64_slot_cacheable && macho_slot_put(&dl->dlm64_slot_key, DLBASE(dl)->dl_base)))
#endif
        uo.uo_munmap(DLPRIVATE(dl), DLBASE(dl)->dl_base, DLBASE(dl)->dl_size);
        DLBASE(dl)->dl_base = NULL;
        DLBASE(dl)->dl_size = 0;
    }
    uo.uo_free(DLPRIVATE(dl), dl);
}

static uintptr_t vaddr_macho64(struct dlinfo_macho64 *dl, uint32_t fileoff)
{
    const struct load_command *curr_lc = dl->dlm64_lc;
    for (size_t i = 0; i < dl->dlm64_hdr.ncmds; ++i, curr_lc = (const struct load_command *)((uintptr_t)curr_lc + curr_lc->cmdsize)) {
        if (curr_lc->cmd != LC_SEGMENT_64)
            continue;
        
        const struct segment_command_64 *curr_sc = (const struct segment_command_64 *)curr_lc;

        // skip page zero
        if (curr_sc->initprot == 0 && curr_sc->maxprot == 0 && curr_sc->vmaddr == 0) {
            // PAGEZERO
            continue;
        }

        if (fileoff < curr_sc->fileoff || fileoff >= (curr_sc->fileoff + curr_sc->filesize))
            continue;

        return dl->dlm64_load_bias + curr_sc->vmaddr + fileoff - curr_sc->fileoff;
    }

    return 0;
}

static struct dlinfo *load_macho64_library(struct ulinker *ulinker, const char *filename, int fd, ssize_t file_size, int flags)
{
    struct dlinfo *ret = NULL;
    struct dlinfo_macho64 *dl = NULL;
    void* hdr_mmap = MAP_FAILED;
    size_t hdr_size = 0;

    if (!(dl = uo.uo_malloc(ulinker->ul_private, sizeof(struct dlinfo_macho64)))) {
        ERROR("failed to open \"%s\": -%d (%s)", filename, ENOMEM, strerror(ENOMEM));
        goto out;
    }

    memset(dl, 0, sizeof(*dl));
    DLBASE(dl)->dl_type = DLT_MACHO64;
    DLBASE(dl)->dl_flags = flags;
    DLBASE(dl)->dl_emu = !(flags & RTLD_NOEMU) && !noemu;
    strncpy(DLBASE(dl)->dl_pathname, filename, sizeof(DLBASE(dl)->dl_pathname)-1);
    DLPRIVATE(dl) = ulinker->ul_private;

    dl->dlm64_hdr.magic = MH_MAGIC_64;

    if ((sizeof(struct mach_header_64) - 4) != uo.uo_read(ulinker->ul_private, fd, ((uint8_t *)&dl->dlm64_hdr) + 4, sizeof(struct mach_header_64) - 4)) {
        ERROR("failed to read Mach-O header \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }

    if (dl->dlm64_hdr.filetype != MH_DYLIB && dl->dlm64_hdr.filetype != MH_EXECUTE) {
        ERROR("failed to load Mach-O \"%s\": unexpected filetype: %d", filename, dl->dlm64_hdr.filetype);
        goto out;
    }

    if (dl->dlm64_hdr.filetype == MH_EXECUTE && !(dl->dlm64_hdr.flags & MH_PIE)) {
        ERROR("failed to load Mach-O \"%s\": executable files are only supported as PIE!", filename);
        goto out;
    }

    if (dl->dlm64_hdr.cputype == CPU_TYPE_X86_64)
        DLBASE(dl)->dl_arch = ULARCH_X86_64;
    else if (dl->dlm64_hdr.cputype == CPU_TYPE_ARM64)
        DLBASE(dl)->dl_arch = ULARCH_ARM64;
    DLBASE(dl)->dl_abi = ULABI_DARWIN;

    // map the header and load commands

    dl->dlm64_sizeoflc = dl->dlm64_hdr.sizeofcmds;
    hdr_size = page_end(sizeof(struct mach_header_64) + dl->dlm64_sizeoflc);

    if (MAP_FAILED == (hdr_mmap = uo.uo_mmap(ulinker->ul_private, NULL, hdr_size, PROT_READ, MAP_PRIVATE, fd, 0))) {
        ERROR("failed to map ELF phdr \"%s\" (size 0x%zx): -%d (%s)", filename, hdr_size, errno, strerror(errno));
        goto out;
    }

    struct load_command *lc = (struct load_command *)((uintptr_t)hdr_mmap + sizeof(struct mach_header_64));
    const struct load_command *curr_lc = NULL;
    // reserve virtual range for library

    Elf64_Addr min_vaddr = UINTPTR_MAX;
    Elf64_Addr max_vaddr = 0;

    uint32_t codesig_off = 0, codesig_size = 0;
    int found_lc_segment = 0;
    curr_lc = lc;
    for (size_t i = 0; i < dl->dlm64_hdr.ncmds; ++i, curr_lc = (const struct load_command *)((uintptr_t)curr_lc + curr_lc->cmdsize)) {
        if (curr_lc->cmd == LC_CODE_SIGNATURE) {
            const struct linkedit_data_command *sig = (const struct linkedit_data_command *)curr_lc;
            codesig_off = sig->dataoff;
            codesig_size = sig->datasize;
        }
        if (curr_lc->cmd != LC_SEGMENT_64) {
            continue;
        }

        const struct segment_command_64 *curr_sc = (const struct segment_command_64 *)curr_lc;

        // skip page zero
        if (curr_sc->initprot == 0 && curr_sc->maxprot == 0 && curr_sc->vmaddr == 0) {
            dl->dlm64_pagezero += curr_sc->vmsize;
            continue;
        }

        found_lc_segment = 1;

        if (curr_sc->vmaddr < min_vaddr) {
            min_vaddr = curr_sc->vmaddr;
        }

        if (curr_sc->vmaddr + curr_sc->vmsize > max_vaddr) {
            max_vaddr = curr_sc->vmaddr + curr_sc->vmsize;
        }
    }
    if (!found_lc_segment) {
        min_vaddr = 0;
    }

    min_vaddr = page_start(min_vaddr);
    max_vaddr = page_end(max_vaddr);

    size_t load_size = max_vaddr - min_vaddr;

    void *base = MAP_FAILED;
#if defined(__APPLE__)
    // Native images with executable segments go through the slot table (see
    // macho_slot_take); emulated images map their text without PROT_EXEC,
    // which the kernel frees normally.
    if (!DLBASE(dl)->dl_emu) {
        int has_exec = 0;
        curr_lc = lc;
        for (size_t i = 0; i < dl->dlm64_hdr.ncmds; ++i, curr_lc = (const struct load_command *)((uintptr_t)curr_lc + curr_lc->cmdsize)) {
            if (curr_lc->cmd == LC_SEGMENT_64 && (((const struct segment_command_64 *)curr_lc)->initprot & VM_PROT_EXECUTE))
                has_exec = 1;
        }
        struct stat st;
        if (has_exec && 0 == uo.uo_fstat(ulinker->ul_private, fd, &st)) {
            dl->dlm64_slot_key.dev = st.st_dev;
            dl->dlm64_slot_key.ino = st.st_ino;
            dl->dlm64_slot_key.size = st.st_size;
            dl->dlm64_slot_key.mtime_sec = (long)st.st_mtimespec.tv_sec;
            dl->dlm64_slot_key.mtime_nsec = (long)st.st_mtimespec.tv_nsec;
            dl->dlm64_slot_key.load_size = load_size;
            void *slot = macho_slot_take(&dl->dlm64_slot_key);
            if (slot) {
                base = slot;
                dl->dlm64_slot_reused = 1;
                // its executable segments are intact whatever happens below
                dl->dlm64_slot_cacheable = 1;
            }
        }
    }
#endif
    if (MAP_FAILED == base)
        base = uo.uo_mmap(ulinker->ul_private, NULL, load_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (MAP_FAILED == base) {
        ERROR("failed to map Mach-O \"%s\" (size 0x%zx): -%d (%s)", filename, load_size, errno, strerror(errno));
        goto out;
    }

    DLBASE(dl)->dl_base = base;
    DLBASE(dl)->dl_size = load_size;

    dl->dlm64_load_bias = (uintptr_t)base - min_vaddr;

    // Register code signature with kernel so mmap(PROT_EXEC) is allowed.
    // Required on iOS; best-effort on macOS.
#if defined(__APPLE__)
    if (codesig_off && codesig_size && !DLBASE(dl)->dl_emu && !dl->dlm64_slot_reused) {
        fsignatures_t siginfo;
        siginfo.fs_file_start = 0;
        siginfo.fs_blob_start = (void *)(long)codesig_off;
        siginfo.fs_blob_size  = codesig_size;
        int sigresult = fcntl(fd, F_ADDFILESIGS_RETURN, &siginfo);
        if (sigresult == -1) {
#if TARGET_OS_IOS
            ERROR("failed to register code signature for \"%s\": -%d (%s)", filename, errno, strerror(errno));
            goto out;
#else
            PRINT("ulinker: code signature registration failed for \"%s\" (non-fatal): %s", filename, strerror(errno));
#endif
        }
    }
#endif

    // load segments

    curr_lc = lc;
    for (size_t i = 0; i < dl->dlm64_hdr.ncmds; ++i, curr_lc = (const struct load_command *)((uintptr_t)curr_lc + curr_lc->cmdsize)) {
        if (curr_lc->cmd != LC_SEGMENT_64) {
            continue;
        }

        const struct segment_command_64 *curr_sc = (const struct segment_command_64 *)curr_lc;

        // skip page zero
        if (curr_sc->initprot == 0 && curr_sc->maxprot == 0 && curr_sc->vmaddr == 0) {
            // PAGEZERO
            continue;
        }

        // Segment addresses in memory.
        uintptr_t seg_start = curr_sc->vmaddr + dl->dlm64_load_bias;
        uintptr_t seg_end   = seg_start + curr_sc->vmsize;

        uintptr_t seg_page_start = page_start(seg_start);
        uintptr_t seg_page_end   = page_end(seg_end);

        uintptr_t seg_file_end   = seg_start + curr_sc->filesize;

        // File offsets.
        uintptr_t file_start = curr_sc->fileoff;
        uintptr_t file_end   = file_start + curr_sc->filesize;

        uintptr_t file_page_start = page_start(file_start);
        uintptr_t file_length = file_end - file_page_start;

        if (file_size <= 0) {
            ERROR("failed to load Mach-O \"%s\": invalid size 0x%zx", filename, file_size);
            goto out;
        }

        if (file_end > (size_t)file_size) {
            ERROR("failed to load Mach-O \"%s\": segment_command[%zd]:"
                " fileoff (%p) + filesize (%p) ( = %p) past end of file (0x%zx)",
                filename, i, (void *)curr_sc->fileoff,
                (void *)curr_sc->filesize,
                (void *)file_end, file_size);
            goto out;
        }

#if defined(__APPLE__)
        // a reused slot's executable segment is still mapped from this file
        if (dl->dlm64_slot_reused && (curr_sc->initprot & VM_PROT_EXECUTE))
            continue;
#endif

        if (file_length != 0) {
            void* seg_addr = uo.uo_mmap(ulinker->ul_private, (void *)seg_page_start,
                                file_length,
                                VMPROT_TO_PROT(curr_sc->initprot) & ~(DLBASE(dl)->dl_emu ? PROT_EXEC : 0), // strip away PROT_EXEC if run on emulator,
                                MAP_FIXED|MAP_PRIVATE,
                                fd,
                                file_page_start);
            if (seg_addr == MAP_FAILED) {
                ERROR("failed to map Mach-O \"%s\" segment_command[%zd]: -%d (%s)", filename, i, errno, strerror(errno));
                goto out;
            }
        }

        // if the segment is writable, and does not end on a page boundary,
        // zero-fill it until the page limit.
        if ((curr_sc->initprot & VM_PROT_WRITE) != 0 && page_offset(seg_file_end) > 0) {
            memset((void *)seg_file_end, 0, page_size() - page_offset(seg_file_end));
        }

        seg_file_end = page_end(seg_file_end);

        // seg_file_end is now the first page address after the file
        // content. If seg_end is larger, we need to zero anything
        // between them. This is done by using a private anonymous
        // map for all extra pages.
        if (seg_page_end > seg_file_end) {
            void* zeromap = uo.uo_mmap(ulinker->ul_private, (void *)seg_file_end,
                                seg_page_end - seg_file_end,
                                VMPROT_TO_PROT(curr_sc->initprot) & ~(DLBASE(dl)->dl_emu ? PROT_EXEC : 0), // strip away PROT_EXEC if run on emulator
                                MAP_FIXED|MAP_ANONYMOUS|MAP_PRIVATE,
                                -1,
                                0);
            if (zeromap == MAP_FAILED) {
                ERROR("failed to zero fill Mach-O \"%s\" segment_command[%zd] gap: -%d (%s)", filename, i, errno, strerror(errno));
                goto out;
            }
        }

    }

#if defined(__APPLE__)
    // every segment is mapped now; the image may be parked on unload
    if (!DLBASE(dl)->dl_emu && dl->dlm64_slot_key.load_size)
        dl->dlm64_slot_cacheable = 1;
#endif

    // find phdr as it appears in the loaded segments

    curr_lc = lc;
    for (size_t i = 0; i < dl->dlm64_hdr.ncmds; ++i, curr_lc = (const struct load_command *)((uintptr_t)curr_lc + curr_lc->cmdsize)) {
        if (curr_lc->cmd != LC_SEGMENT_64) {
            continue;
        }

        const struct segment_command_64 *curr_sc = (const struct segment_command_64 *)curr_lc;

        // skip page zero
        if (curr_sc->initprot == 0 && curr_sc->maxprot == 0 && curr_sc->vmaddr == 0) {
            // PAGEZERO
            continue;
        }

        if (curr_sc->fileoff <= sizeof(struct mach_header_64) && (curr_sc->fileoff + curr_sc->filesize) >= (sizeof(struct mach_header_64) + dl->dlm64_hdr.sizeofcmds)) {
            uintptr_t loaded_hdr = (uintptr_t)dl->dlm64_load_bias + curr_sc->vmaddr;
            dl->dlm64_lc = (const struct load_command *)(loaded_hdr + sizeof(struct mach_header_64));
            break;
        }
    }

    if (!dl->dlm64_lc) {
        ERROR("failed to find loaded lc for Mach-O \"%s\"", filename);
        goto out;
    }

    ret = (struct dlinfo *)dl;

    // parse additional information from load commands

    curr_lc = dl->dlm64_lc;
    for (size_t i = 0; i < dl->dlm64_hdr.ncmds; ++i, curr_lc = (const struct load_command *)((uintptr_t)curr_lc + curr_lc->cmdsize)) {
        if (curr_lc->cmd == LC_ID_DYLIB) {
            const struct dylib_command *dc = (const struct dylib_command *)curr_lc;
            const char *dylib_name = (const char *)dc + dc->dylib.name.offset;
            strncpy(DLBASE(dl)->dl_name, dylib_name, DLINFO_NAME_LEN - 1);
        }

        if (curr_lc->cmd == LC_SYMTAB) {
            const struct symtab_command *symc = (const struct symtab_command *)curr_lc;
            dl->dlm64_symtab = (struct nlist_64 *)vaddr_macho64(dl, symc->symoff);
            dl->dlm64_symtabsize = symc->nsyms;
            dl->dlm64_strtab = (const char *)vaddr_macho64(dl, symc->stroff);
            dl->dlm64_strtabsize = symc->strsize;
        }

        if (curr_lc->cmd == LC_DYSYMTAB) {
            const struct dysymtab_command *dysymc = (const struct dysymtab_command *)curr_lc;
            dl->dlm64_dysymtab_iextdefsym = dysymc->iextdefsym;
            dl->dlm64_dysymtab_nextdefsym = dysymc->nextdefsym;
        }

        if (curr_lc->cmd == LC_DYLD_CHAINED_FIXUPS) {
            const struct linkedit_data_command *dcfc = (const struct linkedit_data_command *)curr_lc;
            const struct dyld_chained_fixups_header *dcfhdr = (const struct dyld_chained_fixups_header *)vaddr_macho64(dl, dcfc->dataoff);
            dl->dlm64_chainedfixups_starts = (const struct dyld_chained_starts_in_image *)(((uintptr_t)dcfhdr) + dcfhdr->starts_offset);
            dl->dlm64_chainedfixups_strtab = (const char *)(((uintptr_t)dcfhdr) + dcfhdr->symbols_offset);
            dl->dlm64_chainedfixups_imports = (void *)(((uintptr_t)dcfhdr) + dcfhdr->imports_offset);
            dl->dlm64_chainedfixups_imports_count = dcfhdr->imports_count;
            dl->dlm64_chainedfixups_imports_format = dcfhdr->imports_format;
        }
    }

    if (0 == create_macho64_jit_entry(dl, filename))
        gdb_jit_register(&dl->dlm64_jit_entry);

out:
    if (hdr_mmap != MAP_FAILED) {
        uo.uo_munmap(ulinker->ul_private, hdr_mmap, hdr_size);
        hdr_mmap = MAP_FAILED;
        hdr_size = 0;
    }
    if (!ret) {
        if (dl) {
            free_macho64_library(dl);
            dl = NULL;
        }
    }
    return ret;
}

static void free_library(struct dlinfo *dl)
{
    if (dl->dl_type == DLT_EXTERNAL) {
        // external shouldn't ever be free'd (linked by multiple linkers)
        return;
    }

    if (dl->dl_type == DLT_ELF64)
        free_elf64_library((struct dlinfo_elf64 *)dl);
    else if (dl->dl_type == DLT_MACHO64)
        free_macho64_library((struct dlinfo_macho64 *)dl);
    else {
        // TODO abort
        abort();
    }
}

static int link_library_elf64_relocate(struct dlinfo_elf64 *dl, void *rel_opaque, size_t size, size_t rel_type)
{
    if (rel_type != DT_RELA) {
        ERROR("failed to relocate ELF \"%s\": unknown rel_type %zu", DLBASE(dl)->dl_name, rel_type);
        return 1;
    }

    if (!size)
        return 0;

    if (!dl->dle64_symtab) {
        ERROR("failed to relocate ELF \"%s\": symbol table not found", DLBASE(dl)->dl_name);
        return 1;
    }

    for (Elf64_Rela *rel = (Elf64_Rela *)rel_opaque; (uintptr_t)(rel + 1) <= (uintptr_t)rel_opaque + size; rel++)
    {
        Elf64_Word type = ELF64_R_TYPE(rel->r_info);
        if (!type)
            continue;

        Elf64_Word sym = ELF64_R_SYM(rel->r_info);
        const char *sym_name = NULL;
        size_t     sym_size = 0;
        Elf64_Addr sym_addr = 0;
        Elf64_Addr reloc = dl->dle64_load_bias + rel->r_offset;
        Elf64_Addr addend = rel->r_addend;

        if (sym) {
            sym_name = string_elf64(dl, dl->dle64_symtab[sym].st_name);
            sym_size = dl->dle64_symtab[sym].st_size;
            // For COPY relocations, skip the current library to avoid
            // resolving the symbol to its own copy target.
            if (type == R_X86_64_COPY || type == R_AARCH64_COPY) {
                sym_addr = (Elf64_Addr)dlsym_internal(DLBASE(dl)->dl_linker, (void *)DLBASE(dl)->dl_next, sym_name, NULL, 1, 0, DLBASE(dl));
            } else {
                // lookup symbol with external libraries as this is for internal use
                sym_addr = (Elf64_Addr)dlsym_internal(DLBASE(dl)->dl_linker, RTLD_DEFAULT, sym_name, NULL, 1, 0, DLBASE(dl));
            }
            if (!sym_addr) {
                if (ELF64_ST_BIND(dl->dle64_symtab[sym].st_info) != STB_WEAK) {
                    ERROR("failed to relocate ELF \"%s\": symbol \"%s\" not found", DLBASE(dl)->dl_name, sym_name);
                    return 1;
                }
            }
        }

        switch (type) {
            case R_X86_64_64:
            case R_AARCH64_ABS64:
                //PRINT("RELO ABS64 %p %p %s", (void *)reloc, (void *)(sym_addr + addend), sym_name);
                *(Elf64_Addr *)(reloc) = (sym_addr + addend);
                break;
            case R_X86_64_COPY:
            case R_AARCH64_COPY:
                //PRINT("RELO COPY %p %p %s %zu", (void *)reloc, (void *)(sym_addr + addend), sym_name, sym_size);
                if (!sym_addr) break;
                memcpy(( void *)reloc, (void *)(sym_addr + addend), sym_size);
                break;
            case R_X86_64_JUMP_SLOT:
            case R_AARCH64_JUMP_SLOT:
                //PRINT("RELO JMP_SLOT %p %p %s", (void *)reloc, (void *)(sym_addr + addend), sym_name);
                *(Elf64_Addr *)(reloc) = (sym_addr + addend);
                break;
            
            case R_X86_64_GLOB_DAT:
            case R_AARCH64_GLOB_DAT:
                //PRINT("RELO GLOB_SLOT %p %p %s", (void *)reloc, (void *)(sym_addr + addend), sym_name);
                *(Elf64_Addr *)(reloc) = (sym_addr + addend);
                break;
            
            case R_X86_64_RELATIVE:
            case R_AARCH64_RELATIVE:
                //PRINT("RELO RELATIVE %p %p", (void *)reloc, (void *)(dl->dle64_load_bias + addend));
                *(Elf64_Addr *)(reloc) = (dl->dle64_load_bias + addend);
                break;
            
            case R_X86_64_IRELATIVE:
            case R_AARCH64_IRELATIVE:
                //PRINT("RELO IRELATIVE %p %p", (void *)reloc, (void *)(dl->dle64_load_bias + addend));
                (void)0;
                typedef Elf64_Addr (*ifunc_resolver_t)(void);
                ifunc_resolver_t ifunc_resolver = (ifunc_resolver_t)(dl->dle64_load_bias + addend);
                Elf64_Addr ifunc_addr = ifunc_resolver();
                *(Elf64_Addr *)(reloc) = ifunc_addr;
                break;
            
            default:
                ERROR("failed to relocate ELF \"%s\": unknown reloc type %u", DLBASE(dl)->dl_name, type);
                return 1;
        }
    }

    return 0;
}

static int link_library_elf64(struct dlinfo_elf64 *dl)
{
    if (dl->dle64_plt_rel_opaque) {
        if (0 != link_library_elf64_relocate(dl, dl->dle64_plt_rel_opaque, dl->dle64_plt_rel_size, dl->dle64_plt_rel_type))
            return 1;
    }

    if (dl->dle64_rela) {
        if (0 != link_library_elf64_relocate(dl, dl->dle64_rela, dl->dle64_rela_size, DT_RELA))
            return 1;
    }

    DLBASE(dl)->dl_linked = 1;
    return 0;
}

static int link_library_macho64(struct dlinfo_macho64 *dl)
{
    if (!dl->dlm64_chainedfixups_imports)
        return 0;

    if (dl->dlm64_chainedfixups_imports_format != DYLD_CHAINED_IMPORT) {
        ERROR("failed to link Mach-O \"%s\": unknown chained fixups imports format %d", DLBASE(dl)->dl_name, dl->dlm64_chainedfixups_imports_format);
        return 1;
    }

    struct dyld_chained_import *imports = dl->dlm64_chainedfixups_imports;

    uintptr_t starts_base = (uintptr_t)dl->dlm64_chainedfixups_starts;
    for (uint32_t sg = 0; sg < dl->dlm64_chainedfixups_starts->seg_count; sg++) {
        uint32_t seg_offset = dl->dlm64_chainedfixups_starts->seg_info_offset[sg];
        if (seg_offset == 0) {// the mighty undocumented skip {
            continue;
        }

        const struct dyld_chained_starts_in_segment *seg_starts = (const struct dyld_chained_starts_in_segment *)(starts_base + seg_offset);
        uintptr_t segment_address = dl->dlm64_load_bias + seg_starts->segment_offset + dl->dlm64_pagezero;

        switch (seg_starts->pointer_format) {
            case DYLD_CHAINED_PTR_64:
            case DYLD_CHAINED_PTR_64_OFFSET:
                break;
            default:
                ERROR("failed to link Mach-O \"%s\": unknown pointer format %d for segment %p", DLBASE(dl)->dl_name, seg_starts->pointer_format, (void *)seg_starts->segment_offset);
                return 1;
        }

        for (uint32_t pg = 0; pg < seg_starts->page_count; pg++) {
            if (seg_starts->page_start[pg] == DYLD_CHAINED_PTR_START_NONE)
                continue;

            union {
                uintptr_t *ptr8;
                uint8_t   *ptr1;

                struct dyld_chained_ptr_64_rebase *ptr_64_rebase;
                struct dyld_chained_ptr_64_bind   *ptr_64_bind;
            } p;
            p.ptr8 = (uintptr_t *)(segment_address + (uintptr_t)pg * seg_starts->page_size + seg_starts->page_start[pg]);
            
            uintptr_t *prev_ptr8 = 0;
            while (p.ptr8 != prev_ptr8) {
                prev_ptr8 = p.ptr8;
                unsigned next = 0;
                switch (seg_starts->pointer_format) {
                    case DYLD_CHAINED_PTR_64:
                    case DYLD_CHAINED_PTR_64_OFFSET:
                        if (p.ptr_64_rebase->bind) {
                            next = p.ptr_64_bind->next * 4;
                            struct dyld_chained_import *import = &imports[p.ptr_64_bind->ordinal];
                            // skip the leading underscore
                            const char *s = dl->dlm64_chainedfixups_strtab + import->name_offset + 1;
                            uintptr_t sym_addr = (uintptr_t)dlsym_internal(DLBASE(dl)->dl_linker, RTLD_DEFAULT, s, NULL, 1, 0, DLBASE(dl));
                            if (!sym_addr && !import->weak_import) {
                                ERROR("failed to link Mach-O \"%s\": symbol \"%s\" not found", DLBASE(dl)->dl_name, s);
                                return 1;
                            }
                            *p.ptr8 = sym_addr;
                        }
                        else {
                            next = p.ptr_64_rebase->next * 4;
                            *p.ptr8 = (uintptr_t)dl->dlm64_load_bias + p.ptr_64_rebase->target + dl->dlm64_pagezero;
                        }
                        break;
                }
                p.ptr1 += next;
            }
        }
    }

    DLBASE(dl)->dl_linked = 1;
    return 0;
}

static int link_library(struct dlinfo *dl)
{
    // already linked
    if (dl->dl_linked)
        return 0;

    if (dl->dl_type == DLT_ELF64)
        return link_library_elf64((struct dlinfo_elf64 *)dl);

    if (dl->dl_type == DLT_MACHO64)
        return link_library_macho64((struct dlinfo_macho64 *)dl);

    ERROR("failed to link ELF \"%s\": unknown type %d", dl->dl_name, dl->dl_type);
    return 1;
}

static struct dlinfo *load_library_internal(struct ulinker *ulinker, const char *filename, int flags)
{
    struct dlinfo *ret = NULL;
    struct dlinfo *dl = NULL;
    int fd = -1;

    // first see if this library is already loaded and return it
    if ((ret = find_library(ulinker, filename, 1)))
        return ret;

    fd = uo.uo_open(ulinker->ul_private, filename);
    if (fd == -1) {
        ERROR("failed to open \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }
    
    struct stat file_stat;
    if (uo.uo_fstat(ulinker->ul_private, fd, &file_stat) != 0) {
        ERROR("failed to stat file \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }
    ssize_t file_size = file_stat.st_size;

    uint32_t magic = 0;
    if (4 != uo.uo_read(ulinker->ul_private, fd, &magic, 4)) {
        ERROR("failed to read file header \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }

    if (magic == *(uint32_t *)elf_magic) {  // \x7fELF
        dl = load_elf_library(ulinker, filename, fd, file_size, flags);
    }
    else if (magic == MH_MAGIC_64) { // 0xfeedfacf
        dl = load_macho64_library(ulinker, filename, fd, file_size, flags);
    }
    else {
        ERROR("failed to load \"%s\": unknown magic %hhx,%hhx,%hhx,%hhx", filename, (magic) & 0xff, (magic >> 8) & 0xff, (magic >> 16) & 0xff, (magic >> 24) & 0xff);
        goto out;
    }

    // fd is no longer needed
    uo.uo_close(ulinker->ul_private, fd);
    fd = -1;

    if (!dl)
        goto out;

    // link library to linker loaded library list
    DLINFO_LOCK(ulinker);
    for (struct dlinfo *curr = ulinker->ul_dlinfo; curr != NULL; curr = curr->dl_next) {
        // some other thread managed to load this library before we did, abort
        if (0 == strcmp(curr->dl_name, dl->dl_name)) {
            free_library(dl);
            dl = NULL;

            curr->dl_refcnt++;
            DLINFO_UNLOCK(ulinker);
            return curr;
        }
    }

    dl->dl_next = ulinker->ul_dlinfo;
    ulinker->ul_dlinfo = dl;

    dl->dl_linker = ulinker;
    dl->dl_refcnt++;
    
    DLINFO_UNLOCK(ulinker);

    ULINKER_EVENT_LOAD(ULEVENT_LOAD, dl);

    ret = dl;

out:
    if (!ret) {
        if (dl) {
            free_library(dl);
            dl = NULL;
        }
    }
    if (fd != -1) {
        uo.uo_close(ulinker->ul_private, fd);
        fd = -1;
    }
    return ret;
}

static void call_function(struct ulinker *linker, struct dlinfo *dl, void *func) {
    uo.uo_call(linker->ul_private, func);
}

static void call_macho64_constructors(struct dlinfo_macho64 *dl) {
    const struct load_command *curr_lc = dl->dlm64_lc;
    for (size_t i = 0; i < dl->dlm64_hdr.ncmds; ++i, curr_lc = (const struct load_command *)((uintptr_t)curr_lc + curr_lc->cmdsize)) {
        if (curr_lc->cmd != LC_SEGMENT_64)
            continue;
        
        const struct segment_command_64 *curr_sc = (const struct segment_command_64 *)curr_lc;

        const struct section_64 *curr_sect = (const struct section_64 *)((uintptr_t)curr_sc + sizeof(struct segment_command_64));
        for (size_t j = 0; j < curr_sc->nsects; ++j, curr_sect++) {
            
            if ( (curr_sect->flags & SECTION_TYPE) != S_INIT_FUNC_OFFSETS )
                continue;
            
            // MachOAnalyzer::forEachInitializer implements various sanity checks here (prot, alignment)

            const uint32_t *initsStart = (uint32_t *)(dl->dlm64_load_bias + curr_sect->addr);
            const uint32_t *initsEnd = (uint32_t *)((uintptr_t)initsStart + curr_sect->size);
            for (const uint32_t *p = initsStart; p < initsEnd; ++p) {
                uint32_t initOffset = *p;
                uintptr_t initAddr = vaddr_macho64(dl, initOffset);
                if (initAddr)
                    call_function(DLBASE(dl)->dl_linker, (struct dlinfo *)dl, (void *)initAddr);
            }
        }
    }
}

static void call_elf64_constructors(struct dlinfo_elf64 *dl)
{
    if (dl->dle64_init_func)
        call_function(DLBASE(dl)->dl_linker, (struct dlinfo *)dl, (void *)dl->dle64_init_func);

    if (dl->dle64_init_array && dl->dle64_init_array_size) {
        size_t count = dl->dle64_init_array_size / sizeof(void *);
        void **funcs = (void **)dl->dle64_init_array;
        for (size_t i = 0; i < count; i++) {
            if (funcs[i])
                call_function(DLBASE(dl)->dl_linker, (struct dlinfo *)dl, funcs[i]);
        }
    }
}

static void call_constructors(struct dlinfo *dl) {
    if (dl->dl_constructors_called)
        return;

    // We set constructors_called before actually calling the constructors, otherwise it doesn't
    // protect against recursive constructor calls. One simple example of constructor recursion

    dl->dl_constructors_called = 1;

    // TODO call children ctors here

    if (dl->dl_type == DLT_ELF64)
        call_elf64_constructors((struct dlinfo_elf64 *)dl);
    else if (dl->dl_type == DLT_MACHO64)
        call_macho64_constructors((struct dlinfo_macho64 *)dl);
}

static void notify_loaded_segments(struct ulinker *ulinker, struct dlinfo *dl)
{
    if (!ulinker->ul_cb_mmap)
        return;

    if (dl->dl_type == DLT_ELF64) {
        struct dlinfo_elf64 *dl64 = (struct dlinfo_elf64 *)dl;
        for (size_t i = 0; i < dl64->dle64_phdr_count; i++) {
            const Elf64_Phdr *phdr = &dl64->dle64_phdr[i];
            if (phdr->p_type != PT_LOAD)
                continue;
            uintptr_t seg_start = phdr->p_vaddr + dl64->dle64_load_bias;
            uintptr_t seg_page_start = page_start(seg_start);
            uintptr_t seg_page_end = page_end(seg_start + phdr->p_memsz);
            ulinker->ul_cb_mmap(ulinker, dl->dl_base, dl->dl_size,
                (void *)seg_page_start, seg_page_end - seg_page_start,
                PFLAGS_TO_PROT(phdr->p_flags) & ~(DLBASE(dl)->dl_emu ? PROT_EXEC : 0));  // strip away PROT_EXEC if run on emulator
        }
    } else if (dl->dl_type == DLT_MACHO64) {
        struct dlinfo_macho64 *dlm = (struct dlinfo_macho64 *)dl;
        const struct load_command *lc = dlm->dlm64_lc;
        for (size_t i = 0; i < dlm->dlm64_hdr.ncmds; i++, lc = (const struct load_command *)((uintptr_t)lc + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64)
                continue;
            const struct segment_command_64 *sc = (const struct segment_command_64 *)lc;
            // skip zeropage
            if (sc->initprot == 0 && sc->maxprot == 0 && sc->vmaddr == 0)
                continue;
            uintptr_t seg_start = sc->vmaddr + dlm->dlm64_load_bias;
            uintptr_t seg_page_start = page_start(seg_start);
            uintptr_t seg_page_end = page_end(seg_start + sc->vmsize);
            ulinker->ul_cb_mmap(ulinker, dl->dl_base, dl->dl_size,
                (void *)seg_page_start, seg_page_end - seg_page_start,
                VMPROT_TO_PROT(sc->initprot) & ~(DLBASE(dl)->dl_emu ? PROT_EXEC : 0));  // strip away PROT_EXEC if run on emulator
        }
    }
}

static struct dlinfo *load_library(struct ulinker *ulinker, const char *filename, int flags) {
    struct dlinfo *ret = NULL;
    struct dlinfo *dl = NULL;

    if (!(dl = load_library_internal(ulinker, filename, flags)))
        goto out;

    if (link_library(dl))
        goto out;

    call_constructors(dl);

    // calls mmap_cb, load event is reported right at mmap time.
    // this is purely for post-processing memory after initialization is done.
    notify_loaded_segments(ulinker, dl);

    ret = dl;

out:
    // TODO unload library?
    return ret;
}

static struct dlinfo_external *add_external_dlinfo(struct ulinker_external_dlinfo_args *args)
{
    struct dlinfo_external *last_dlext = NULL;
    
    pthread_mutex_lock(&g_external_dlinfos_mtx);

    for (struct dlinfo_external *dlext = g_external_dlinfos; dlext; dlext = (struct dlinfo_external *)dlext->dlext_base.dl_next) {
        if (0 == strncmp(dlext->dlext_base.dl_name, args->ue_name, DLINFO_NAME_LEN)) {
            pthread_mutex_unlock(&g_external_dlinfos_mtx);
            return dlext;
        }
        last_dlext = dlext;
    }

    struct dlinfo_external *dlext = uo.uo_malloc(args->ue_private, sizeof(struct dlinfo_external));
    if (!dlext) {
        pthread_mutex_unlock(&g_external_dlinfos_mtx);
        return NULL;
    }

    memset(dlext, 0, sizeof(*dlext));
    DLPRIVATE(dlext) = args->ue_private;
    strncpy(DLBASE(dlext)->dl_name, args->ue_name, DLINFO_NAME_LEN - 1);
    DLBASE(dlext)->dl_refcnt++;
    DLBASE(dlext)->dl_type = DLT_EXTERNAL;
    DLBASE(dlext)->dl_linked = 1;
    dlext->dlext_dlsym = args->ue_dlsym;

    if (last_dlext)
        DLBASE(last_dlext)->dl_next = (struct dlinfo *)dlext;
    else
        g_external_dlinfos = dlext;

    pthread_mutex_unlock(&g_external_dlinfos_mtx);

    return dlext;
}

/* -------------------- API -------------------- */

int ulinker_setopt(int opt, void *val)
{
    if (opt == ULINKER_OPS) {
        memcpy(&uo, val, sizeof(uo));
        return 0;
    }
    if (opt == ULINKER_PRINT_CB) {
        print_cb = (void (*)(char *, int))val;
        return 0;
    }
    if (opt == ULINKER_ADD_EXTERNAL_DLINFO) {
        return (add_external_dlinfo((struct ulinker_external_dlinfo_args *)val) ? 0 : 1);
    }
    if (opt == ULINKER_EVENT_CB) {
        event_cb = (void (*)(int, void *))val;
        return 0;
    }
    if (opt == ULINKER_NOEMU) {
        noemu = (int)(long)val;
        return 0;
    }

    return 1;
}

struct ulinker *ulinker_create(void *private) {
    struct ulinker *ret = NULL;
    struct ulinker *ulinker = NULL;

    if (!(ulinker = uo.uo_malloc(private, sizeof(*ulinker)))) {
        ERROR("failed to allocate ulinker: -%d (%s)", ENOMEM, strerror(ENOMEM));
        goto out;
    }

    memset(ulinker, 0, sizeof(*ulinker));

    pthread_mutex_init(&ulinker->ul_dlinfo_mtx, NULL);
    ulinker->ul_private = private;

    // put the external dlinfos in the end of the list
    ulinker->ul_dlinfo = (struct dlinfo *)g_external_dlinfos;

    ret = ulinker;
out:
    if (!ret) {
        if (ulinker) {
            uo.uo_free(private, ulinker);
            ulinker = NULL;
        }
    }

    return ret;
}

void ulinker_destroy(struct ulinker *ulinker) {
    // free all libraries
    while (ulinker->ul_dlinfo) {
        struct dlinfo *curr_dl = ulinker->ul_dlinfo;

        // end of linker-specific list
        if (curr_dl->dl_type == DLT_EXTERNAL) {
            ulinker->ul_dlinfo = NULL;
            break;
        }

        ULINKER_EVENT_LOAD(ULEVENT_UNLOAD, curr_dl);
        ulinker->ul_dlinfo = curr_dl->dl_next;
        free_library(curr_dl);
    }

    pthread_mutex_destroy(&ulinker->ul_dlinfo_mtx);
    uo.uo_free(ulinker->ul_private, ulinker);
}

void *ulinker_dlopen(struct ulinker *ulinker, const char *filename, int flags) {
    return (void *)load_library(ulinker, filename, flags);
}

int ulinker_dlclose(void *handle)
{
    struct dlinfo *dl = (struct dlinfo *)handle;
    struct ulinker *ulinker = dl->dl_linker;

    DLINFO_LOCK(ulinker);

    // fast exit - refcount not zero
    if (--dl->dl_refcnt) {
        DLINFO_UNLOCK(ulinker);
        return 0;
    }

    struct dlinfo **dl_prev = NULL;

    if (ulinker->ul_dlinfo == dl) {
        dl_prev = &ulinker->ul_dlinfo;
    }
    else {
        for (struct dlinfo *curr_dl = ulinker->ul_dlinfo; curr_dl; curr_dl = curr_dl->dl_next) {
            if (curr_dl->dl_next == dl) {
                dl_prev = &curr_dl->dl_next;
                break;
            }
        }
    }

    if (!dl_prev) {
        ERROR("failed to unload library \"%s\": loaded entry not found", dl->dl_name);
        dl->dl_refcnt++;
        DLINFO_UNLOCK(ulinker);
        return 1;
    }

    *dl_prev = dl->dl_next;
    dl->dl_next = NULL;

    DLINFO_UNLOCK(ulinker);

    free_library(dl);

    return 0;
}

void *ulinker_dlsym(struct ulinker *ulinker, void *handle, const char *symbol, int all) {
    void *lr = __builtin_return_address(0);
    // do NOT search in external for API calls
    return dlsym_internal(ulinker, handle, symbol, lr, 0, all, NULL);
}

char *ulinker_dlerror(void) {
    return errormsg;
}

int ulinker_dladdr(struct ulinker *ulinker, const void *addr, struct ulinker_dladdr_info *info) {
    DLINFO_LOCK(ulinker);
    struct dlinfo *dl = library_for_address_locked(ulinker, addr);
    DLINFO_UNLOCK(ulinker);

    if (!dl)
        return 0;

    info->udli_fname = dl->dl_name;
    info->udli_fbase = dl->dl_base;
    info->udli_fsize = dl->dl_size;
    info->udli_farch = dl->dl_arch;
    info->udli_fabi  = dl->dl_abi;
    info->udli_femu  = dl->dl_emu;
    info->udli_sname = NULL;
    info->udli_saddr = NULL;

    if (dl->dl_type == DLT_ELF64)
        reverse_lookup_symbol_elf64((struct dlinfo_elf64 *)dl, addr, &info->udli_sname, &info->udli_saddr);
    else if (dl->dl_type == DLT_MACHO64)
        reverse_lookup_symbol_macho64((struct dlinfo_macho64 *)dl, addr, &info->udli_sname, &info->udli_saddr);

    return 1;
}

struct dup_arg {
    void *arg;
    void *old_base;
    void (*segment_cb)(void *, void *, void *, size_t, int);
};

static void dup_mmap_cb(struct ulinker *ulinker, void *dl_base, size_t dl_size, void *map_base, size_t map_size, int map_prot) {
    struct dup_arg *dup_arg = (struct dup_arg *)ulinker->ul_cb_arg;

    uint64_t old_map_base = (uint64_t)dup_arg->old_base + ((uint64_t)map_base - (uint64_t)dl_base);

    if (dup_arg->segment_cb)
        dup_arg->segment_cb(dup_arg->arg, (void *)old_map_base, map_base, map_size, map_prot);
}

int ulinker_duplicate(struct ulinker *old, struct ulinker *new, void (*segment_cb)(void *, void *, void *, size_t, int), void *arg)
{
    int ret = 1;

    if (old == new)
        return ret;

    DLINFO_LOCK(old);

    new->ul_cb_mmap = dup_mmap_cb;

    for (struct dlinfo *dl = old->ul_dlinfo; dl; dl = dl->dl_next) {
        // end of linker-specific list
        if (dl->dl_type == DLT_EXTERNAL) {
            break;
        }

        struct dup_arg dup_arg = {
            .arg = arg,
            .old_base = dl->dl_base,
            .segment_cb = segment_cb
        };

        new->ul_cb_arg = (void *)&dup_arg;
        if (!load_library(new, dl->dl_pathname, dl->dl_flags))
            goto out;
    }

    ret = 0;

out:

    new->ul_cb_arg = NULL;
    new->ul_cb_mmap = NULL;

    if (ret) {
        while (new->ul_dlinfo) {
            struct dlinfo *dl = new->ul_dlinfo;

            // end of linker-specific list
            if (dl->dl_type == DLT_EXTERNAL) {
                break;
            }

            ULINKER_EVENT_LOAD(ULEVENT_UNLOAD, dl);
            new->ul_dlinfo = dl->dl_next;
            free_library(dl);
        }
    }

    DLINFO_UNLOCK(old);

    return ret;
}