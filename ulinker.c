#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ulinker.h"


static void (*print_cb)(char *, int) = NULL;
#define PRINT0(_msg, _len) do { \
    if (print_cb) { print_cb(_msg, _len); } \
    else { write(1, _msg, _len); } \
} while (0)

static __thread char msgbuf[4096] = {0};
#define PRINT(MSG, ...) do { \
    int __len = snprintf(msgbuf, sizeof(msgbuf), "%s:%d %s: " MSG "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    PRINT0(msgbuf, __len); \
} while (0)

static __thread char errormsg[4096] = {0};
#define ERROR(_msg, ...) do { snprintf(errormsg, sizeof(errormsg), (_msg), ##__VA_ARGS__); PRINT("%s\n", errormsg); } while (0)

static void (*event_cb)(int, void *);

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
#define PFLAGS_TO_PROT(x)            (MAYBE_MAP_FLAG((x), PF_X, PROT_EXEC) | \
                                      MAYBE_MAP_FLAG((x), PF_R, PROT_READ) | \
                                      MAYBE_MAP_FLAG((x), PF_W, PROT_WRITE))

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

#define R_X86_64_JUMP_SLOT	7	/* Create PLT entry */
#define R_AARCH64_JUMP_SLOT    1026	/* Create PLT entry.  */

#define R_X86_64_GLOB_DAT	6	/* Create GOT entry */
#define R_AARCH64_GLOB_DAT     1025	/* Create GOT entry.  */

#define R_X86_64_RELATIVE	8	/* Adjust by program base */
#define R_AARCH64_RELATIVE     1027	/* Adjust by program base.  */

#define R_X86_64_IRELATIVE	37	/* Adjust indirectly by program base */
#define R_AARCH64_IRELATIVE	1032	/* STT_GNU_IFUNC relocation.  */

/* -------------------- Options -------------------- */

static void *malloc_wrapper(void *, size_t size)             { return malloc(size); }
static void  free_wrapper(void *, void *ptr)                 { return free(ptr); }
static void *realloc_wrapper(void *, void *ptr, size_t size) { return realloc(ptr, size); }

static int     open_wrapper(void *, const char *name)                { return open(name, O_RDONLY); }
static int     close_wrapper(void *, int fd)                         { return close(fd); }
static ssize_t read_wrapper(void *, int fd, void *buf, size_t count) { return read(fd, buf, count); }
static int     fstat_wrapper(void *, int fd, struct stat *statbuf)   { return fstat(fd, statbuf); }

static void *mmap_wrapper(void *, void *hint, size_t length, int prot, int flags, int fd, off_t offset) { return mmap(hint, length, prot, flags, fd, offset); }
static int   munmap_wrapper(void *, void *addr, size_t size)                                            { return munmap(addr, size); }
static int   mprotect_wrapper(void *, void *addr, size_t size, int flags)                               { return mprotect(addr, size, flags); }

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
    .uo_mprotect = mprotect_wrapper
};

/* -------------------- Linker Instance -------------------- */

struct ulinker {
    pthread_mutex_t  ul_dlinfo_mtx;
    struct dlinfo   *ul_dlinfo;
    void *ul_private;
};

#define DLINFO_LOCK(_ul)   pthread_mutex_lock(&(_ul)->ul_dlinfo_mtx)
#define DLINFO_UNLOCK(_ul) pthread_mutex_unlock(&(_ul)->ul_dlinfo_mtx)

/* -------------------- Dynamic Library Info -------------------- */

enum {
    DLT_EMULATED,
    DLT_ELF64
};

#define DLINFO_NAME_LEN 128
struct dlinfo {
    struct dlinfo *dl_next;
    struct ulinker *dl_linker; // back pointer to owning linker
    void *dl_private;
    char dl_name[DLINFO_NAME_LEN];
    uint64_t dl_refcnt;
    int dl_type; // DLT_*
    void *dl_base;
    size_t dl_size;
    int dl_linked;
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
};

struct dlinfo_emulated {
    struct dlinfo dlemu_base;
    void *(*dlemu_dlsym)(void *, const char *s);
};

static pthread_mutex_t g_emulated_dlinfos_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct dlinfo_emulated *g_emulated_dlinfos = NULL;

/* -------------------- Private functions -------------------- */

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

static void *lookup_symbol_emulated(struct dlinfo_emulated *dl, const char *s)
{
    return dl->dlemu_dlsym(DLPRIVATE(dl), s);
}

static void *lookup_symbol_elf64(struct dlinfo_elf64 *dl, const char *s)
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

static void *lookup_symbol(struct dlinfo *dl, const char *s)
{
    if (dl->dl_type == DLT_EMULATED) 
        return lookup_symbol_emulated((struct dlinfo_emulated *)dl, s);
    if (dl->dl_type == DLT_ELF64)
        return lookup_symbol_elf64((struct dlinfo_elf64 *)dl, s);
    return NULL;
}

static struct dlinfo *load_elf64_library(struct ulinker *ulinker, const char *filename, int fd, ssize_t file_size, unsigned char *ident)
{
    struct dlinfo *ret = NULL;
    struct dlinfo_elf64 *dl = NULL;
    void* phdr_mmap = MAP_FAILED;
    size_t phdr_size = 0;

    if (!(dl = uo.uo_malloc(ulinker->ul_private, sizeof(struct dlinfo_elf64)))) {
        ERROR("failed to open \"%s\": -%d (%s)", filename, ENOMEM, strerror(ENOMEM));
        goto out;
    }

    memset(dl, 0, sizeof(*dl));
    DLBASE(dl)->dl_type = DLT_ELF64;
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

        if (file_end >= (size_t)file_size) {
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
                                PFLAGS_TO_PROT(curr_phdr->p_flags),
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
                                PFLAGS_TO_PROT(curr_phdr->p_flags),
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
        }
    }

    // second pass - parse entries relying on strtab
    for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SONAME) {
            strncpy(DLBASE(dl)->dl_name, string_elf64(dl, d->d_un.d_val), DLINFO_NAME_LEN - 1);
            break;
        }
    }

out:
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

static struct dlinfo *load_elf_library(struct ulinker *ulinker, const char *filename, int fd, ssize_t file_size)
{
    struct dlinfo *ret = NULL;
    unsigned char ident[EI_NIDENT] = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3};

    if ((EI_NIDENT - 4) != uo.uo_read(ulinker->ul_private, fd, &ident[4], EI_NIDENT - 4)) {
        ERROR("failed to read rest of ELF e_ident \"%s\": -%d (%s)", filename, errno, strerror(errno));
        goto out;
    }

    if (ident[EI_CLASS] == ELFCLASS64) {
        ret = load_elf64_library(ulinker, filename, fd, file_size, ident);
    }
    else {
        ERROR("failed to load \"%s\": unsupported ELFCLASS %hhu", filename, ident[EI_CLASS]);
        goto out;
    }

out:
    return ret;
}

static void free_library(struct dlinfo *dl)
{
    if (dl->dl_type == DLT_EMULATED) {
        // emulated shouldn't ever be free'd (linked by multiple linkers)
        return;
    }
    if (dl->dl_type == DLT_ELF64) {
        free_elf64_library((struct dlinfo_elf64 *)dl);
    }
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

    PRINT("link_library_elf64: %p %lu %lu", rel_opaque, size, rel_type);
    for (Elf64_Rela *rel = (Elf64_Rela *)rel_opaque; (uintptr_t)(rel + 1) <= (uintptr_t)rel_opaque + size; rel++)
    {
        Elf64_Word type = ELF64_R_TYPE(rel->r_info);
        if (!type)
            continue;

        Elf64_Word sym = ELF64_R_SYM(rel->r_info);
        const char *sym_name = NULL;
        Elf64_Addr sym_addr = 0;
        Elf64_Addr reloc = dl->dle64_load_bias + rel->r_offset;
        Elf64_Addr addend = rel->r_addend;

        if (sym) {
            sym_name = string_elf64(dl, dl->dle64_symtab[sym].st_name);
            sym_addr = (Elf64_Addr)ulinker_dlsym(DLBASE(dl)->dl_linker, RTLD_DEFAULT, sym_name);
            if (!sym_addr) {
                if (ELF64_ST_BIND(dl->dle64_symtab[sym].st_info) != STB_WEAK) {
                    ERROR("failed to relocate ELF \"%s\": symbol \"%s\" not found", DLBASE(dl)->dl_name, sym_name);
                    return 1;
                }
            }
        }

        switch (type) {
            case R_X86_64_JUMP_SLOT:
            case R_AARCH64_JUMP_SLOT:
                PRINT("RELO JMP_SLOT %p %p %s", (void *)reloc, (void *)(sym_addr + addend), sym_name);
                *(Elf64_Addr *)(reloc) = (sym_addr + addend);
                break;
            
            case R_X86_64_GLOB_DAT:
            case R_AARCH64_GLOB_DAT:
                PRINT("RELO GLOB_SLOT %p %p %s", (void *)reloc, (void *)(sym_addr + addend), sym_name);
                *(Elf64_Addr *)(reloc) = (sym_addr + addend);
                break;
            
            case R_X86_64_RELATIVE:
            case R_AARCH64_RELATIVE:
                PRINT("RELO RELATIVE %p %p", (void *)reloc, (void *)(dl->dle64_load_bias + addend));
                *(Elf64_Addr *)(reloc) = (dl->dle64_load_bias + addend);
                break;
            
            case R_X86_64_IRELATIVE:
            case R_AARCH64_IRELATIVE:
                PRINT("RELO IRELATIVE %p %p", (void *)reloc, (void *)(dl->dle64_load_bias + addend));
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

static int link_library(struct dlinfo *dl)
{
    // already linked
    if (dl->dl_linked)
        return 0;

    if (dl->dl_type == DLT_ELF64)
        return link_library_elf64((struct dlinfo_elf64 *)dl);

    ERROR("failed to link ELF \"%s\": unknown type %d", dl->dl_name, dl->dl_type);
    return 1;
}

static struct dlinfo *load_library_internal(struct ulinker *ulinker, const char *filename)
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
        dl = load_elf_library(ulinker, filename, fd, file_size);
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

static struct dlinfo *load_library(struct ulinker *ulinker, const char *filename) {
    struct dlinfo *ret = NULL;
    struct dlinfo *dl = NULL;

    if (!(dl = load_library_internal(ulinker, filename)))
        goto out;
    
    if (link_library(dl))
        goto out;

    ret = dl;

out:
    // TODO unload library?
    return ret;
}

static void *entrypoint_elf64_library(struct dlinfo_elf64 *dl) {
    if (!dl->dle64_ehdr.e_entry)
        return NULL;
    
    return (void *)(dl->dle64_ehdr.e_entry + dl->dle64_load_bias);
}

static void *entrypoint_library(struct dlinfo *dl) {
    if (dl->dl_type == DLT_ELF64)
        return entrypoint_elf64_library((struct dlinfo_elf64 *)dl);
    
    return NULL;
}

static size_t address_in_library(struct dlinfo *dl, void *addr) {
    if ((uintptr_t)addr >= (uintptr_t)dl->dl_base &&
        (uintptr_t)addr < (uintptr_t)dl->dl_base + dl->dl_size) {
        return (uintptr_t)addr - (uintptr_t)dl->dl_base;
    }

    return SIZE_MAX;
}

static struct dlinfo *library_for_address(struct ulinker *ulinker, void *addr) {
    struct dlinfo *dl = ulinker->ul_dlinfo;
    while (dl) {
        if (address_in_library(dl, addr) != SIZE_MAX)
            return dl;
    }
    return NULL;
}

static struct dlinfo_emulated *add_emulated_dlinfo(struct ulinker_emulated_dlinfo_args *args)
{
    struct dlinfo_emulated *last_dlemu = NULL;
    
    pthread_mutex_lock(&g_emulated_dlinfos_mtx);

    for (struct dlinfo_emulated *dlemu = g_emulated_dlinfos; dlemu; dlemu = (struct dlinfo_emulated *)dlemu->dlemu_base.dl_next) {
        if (0 == strncmp(dlemu->dlemu_base.dl_name, args->ue_name, DLINFO_NAME_LEN)) {
            pthread_mutex_unlock(&g_emulated_dlinfos_mtx);
            return dlemu;
        }
        last_dlemu = dlemu;
    }

    struct dlinfo_emulated *dlemu = uo.uo_malloc(args->ue_private, sizeof(struct dlinfo_emulated));
    if (!dlemu) {
        pthread_mutex_unlock(&g_emulated_dlinfos_mtx);
        return NULL;
    }

    memset(dlemu, 0, sizeof(*dlemu));
    DLPRIVATE(dlemu) = args->ue_private;
    strncpy(DLBASE(dlemu)->dl_name, args->ue_name, DLINFO_NAME_LEN - 1);
    DLBASE(dlemu)->dl_refcnt++;
    DLBASE(dlemu)->dl_type = DLT_EMULATED;
    DLBASE(dlemu)->dl_linked = 1;
    dlemu->dlemu_dlsym = args->ue_dlsym;

    if (last_dlemu)
        DLBASE(last_dlemu)->dl_next = (struct dlinfo *)dlemu;
    else
        g_emulated_dlinfos = dlemu;

    pthread_mutex_unlock(&g_emulated_dlinfos_mtx);

    return dlemu;
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
    if (opt == ULINKER_ADD_EMULATED_DLINFO) {
        return (add_emulated_dlinfo((struct ulinker_emulated_dlinfo_args *)val) ? 0 : 1);
    }
    if (opt == ULINKER_EVENT_CB) {
        event_cb = (void (*)(int, void *))val;
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

    // put the emulated dlinfos in the end of the list
    ulinker->ul_dlinfo = (struct dlinfo *)g_emulated_dlinfos;

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
        struct dlinfo *dl = ulinker->ul_dlinfo;
        ulinker->ul_dlinfo = dl->dl_next;
        PRINT("ulinker destroy %p", dl);
        ULINKER_EVENT_LOAD(ULEVENT_UNLOAD, dl);
        free_library(dl);
    }

    pthread_mutex_destroy(&ulinker->ul_dlinfo_mtx);
    uo.uo_free(ulinker->ul_private, ulinker);
}

void *ulinker_dlopen(struct ulinker *ulinker, const char *filename, int flags) {
    return (void *)load_library(ulinker, filename);
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

void *ulinker_dlsym(struct ulinker *ulinker, void *handle, const char *symbol) {
    DLINFO_LOCK(ulinker);
    struct dlinfo *dl = (struct dlinfo *)handle;
    if (RTLD_DEFAULT == handle) {
        dl = ulinker->ul_dlinfo;
    }
    else if (RTLD_NEXT == handle) {
        void *lr = __builtin_return_address(0);
        dl = library_for_address(ulinker, lr);
        if (dl)
            dl = dl->dl_next;
    }

    while (dl) {
        void *a = lookup_symbol(dl, symbol);
        if (a) {
            DLINFO_UNLOCK(ulinker);
            return a;
        }
        dl = dl->dl_next;
    }
    DLINFO_UNLOCK(ulinker);

    return NULL;
}

char *ulinker_dlerror(void) {
    return errormsg;
}

void *ulinker_dlopen_executable(struct ulinker *ulinker, const char *filename)
{
    struct ulinker *tmp_linker = ulinker_create(ulinker->ul_private);
    if (!tmp_linker) {
        ERROR("failed to load executable library \"%s\": -%d (%s)", filename, ENOMEM, strerror(ENOMEM));
        return NULL;
    }

    struct dlinfo *dl = load_library(tmp_linker, filename);
    if (!dl) {
        ulinker_destroy(tmp_linker);
        return NULL;
    }

    void *entry = entrypoint_library(dl);
    if (!entry) {
        ERROR("failed to load executable library \"%s\": no entrypoint found", filename);
        ulinker_destroy(tmp_linker);
        return NULL;
    }

    // from here on out, no failing!

    DLINFO_LOCK(ulinker);

    // destroy all loaded libraries
    while (ulinker->ul_dlinfo) {
        struct dlinfo *curr_dl = ulinker->ul_dlinfo;
        ulinker->ul_dlinfo = curr_dl->dl_next;
        free_library(curr_dl);
    }

    // move loaded libraries from tmp_linker to ulinker (fix linker back pointer too!)
    ulinker->ul_dlinfo = tmp_linker->ul_dlinfo;
    for (struct dlinfo *curr_dl = ulinker->ul_dlinfo; curr_dl; curr_dl = curr_dl->dl_next) {
        curr_dl->dl_linker = ulinker;
    }

    DLINFO_UNLOCK(ulinker);

    return entry;
}