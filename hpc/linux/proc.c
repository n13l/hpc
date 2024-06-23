#define _GNU_SOURCE
#include <link.h>
#include <stdlib.h>
#include <stdio.h>

#include <dirent.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <elf.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/reg.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#include <inttypes.h>

#include "proc.h"

#include <hpc/compiler.h>
#include <hpc/log.h>

#define CHECK_ERR(cond) \
	if (cond) die("%s (%d:%s)", #cond, errno, strerror(errno));

void
proc_status(int id, int status)
{
	if (WIFEXITED(status))
		info("process pid=%d exited, status=%d", id, WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		info("process pid=%d killed by signal %d (%s)", id, WTERMSIG(status), 
		     strsignal(WTERMSIG(status)));
	else if (WIFSTOPPED(status))
		info("process pid=%d stopped by signal %d (%s)", id,
		     WSTOPSIG(status), strsignal(WSTOPSIG(status)));
	else if (WIFCONTINUED(status))
		info("process pid=%d continued", id);
}

int
proc_fullpath(pid_t pid, char *path, size_t size) {
	char file[PATH_MAX];
	sprintf(file, "/proc/%d/exe", pid);
	return readlink(file, path, size);
}

long
proc_lookup_base(pid_t pid)
{
	FILE *fp;
	char filename[30];
	char line[850];
	long addr = 0;
	char perms[5];
	char* modulePath;
	sprintf(filename, "/proc/%d/maps", pid);
	fp = fopen(filename, "r");
	if(fp == NULL)
		exit(1);
	while(fgets(line, 850, fp) != NULL) {
		if (!strstr(line, "r--p"))
			continue;
		sscanf(line, "%lx-", &addr);
		break;
	}
	fclose(fp);
	return addr;
}

long
libc_lookup_fn(const char *path, const char *fn)
{
	struct link_map *map = dlopen(path, RTLD_LAZY);
	if (!map)
		return 0;
	long addr = (long)dlsym((void *)map, fn);
	if (!addr)
		return 0;
	long offset = addr - map->l_addr;
	return offset;
}

int
libc_lookup_path(pid_t pid, char *path, unsigned int max_size)
{
	char filename[30];
	char line[850];
	long base = 0, start = 0, len = 0, pgoff = 0;
	char prot[5] = {0};
	char execname[PATH_MAX] = {0};
	char* modulePath;

	path[0] = 0;
	sprintf(filename, "/proc/%d/maps", pid);
	FILE *fp = fopen(filename, "r");
	if(fp == NULL)
		exit(1);
	while(fgets(line, 850, fp) != NULL) {
		if (!strstr(line, "r-xp"))
			continue;
		sscanf(line, "%"PRIx64"-%"PRIx64" %s %"PRIx64" %*x:%*x %*u %s\n",
		             &start, &len, prot, &pgoff, execname);
		if(strstr(line, "/libc.so") != NULL) {
			strcpy(path, execname);
			break;
		}
	
	}
	fclose(fp);
	return strlen(path);
}

long
libc_lookup_base(pid_t pid, const char *file)
{
	FILE *fp;
	char filename[30];
	char line[850];
	long base = 0, start = 0, len = 0, pgoff = 0;
	char prot[5] = {0};
	char execname[PATH_MAX] = {0};
	char* modulePath;
	sprintf(filename, "/proc/%d/maps", pid);
	fp = fopen(filename, "r");
	if(fp == NULL)
		exit(1);
	while(fgets(line, 850, fp) != NULL) {
		if (!strstr(line, "r-xp"))
			continue;
		sscanf(line, "%"PRIx64"-%"PRIx64" %s %"PRIx64" %*x:%*x %*u %s\n",
		             &start, &len, prot, &pgoff, execname);
		if(strstr(line, file) != NULL) {
			base = start - pgoff;
			break;
		}
	}
	fclose(fp);
	return base;
}

int
proc_is_mem_mapped(pid_t pid, void* addr)
{
	char path[255];
	uintptr_t address = (uintptr_t) addr;
	snprintf(path, sizeof(path), "/proc/%d/maps", pid);

	FILE* maps = fopen(path, "r");
	if (!maps) {
		perror("fopen");
		return 0;
	}

	uintptr_t start, end;
	while (fscanf(maps, "%lx-%lx", &start, &end) != EOF) {
		if (address >= start && address < end) {
			fclose(maps);
			return 1;
		}
		while (fgetc(maps) != '\n' && !feof(maps)) {}
	}

	fclose(maps);
	return 0;
}

unsigned long
proc_lookup_main_offset(const char *path)
{
	char cmd[512];
	unsigned long addr = 0;
	snprintf(cmd, sizeof(cmd),
	        "readelf -s %s | grep ' main' | awk '{print $2}'", path);
	FILE *pipe = popen(cmd, "r");
	if (!pipe) {
		perror("popen");
		exit(EXIT_FAILURE);
	}

	char buf[32] = {0};
	if (fgets(buf, sizeof(buf), pipe) != NULL) {
		char *ptr;
		addr = strtoul(buf, &ptr, 10);
		sscanf(buf, "%lx", &addr);
	}

	pclose(pipe);
	return addr;
}

long
proc_lookup_mem_cave(pid_t pid)
{
	char path[30], line[85], str[20];
	long addr;
	char name[255];

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	FILE *file = fopen(path, "r");
	if(file == NULL)
		return 0;

	while(fgets(line, 85, file) != NULL) {
		if (!strstr(line, "r-xp"))
			continue;

		sscanf(line, "%lx-%s %s %s %s", &addr, str, str, str, name);
		if(strcmp(str, "00:00") == 0)
			break;
		
	}
	fclose(file);
	return addr;
}

/*
int
process_prelinked(const char *path)
{
	Elf_Ehdr ehdr;
	Elf_Phdr phdr;
	int fd = -1, found = 0;

	if ((fd = open(path, O_RDONLY)) < 0)
		goto failed;
	if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr))
		goto failed;
	if (memcmp(ehdr.e_ident, "\177ELF",  4) != 0)
		goto failed;
	if (lseek(fd, ehdr.e_phoff, SEEK_SET) != ehdr.e_phoff)
		goto failed;

	for (int i = 0; i < ehdr.e_phnum; ++i) {
		if (read(fd, &phdr, sizeof(phdr)) != sizeof(phdr))
			goto failed;
		if (phdr.p_type != PT_LOAD)
			continue;
		found = 1;
		break;
	}

	close(fd);
	return !found ? -1: phdr.p_vaddr != 0;

failed:
	if (fd != -1)
		close(fd);
	return -1;
}
*/
int
process_write(pid_t pid, size_t addr, void *buf, size_t blen)
{
	size_t alen = blen + sizeof(size_t) - (blen % sizeof(size_t));
	u8 ptr[alen];
	memset(ptr, 0, alen);
	memcpy(ptr, buf, blen);

	for (int i = 0; i < blen; i += sizeof(size_t)) {
		void *p = (void *)*(size_t *)&ptr[i];
		ptrace(PTRACE_POKETEXT, pid, (void *)(addr + i), p);
	}

	return 0;
}

int
process_read(pid_t pid, size_t addr, char *buf, size_t blen)
{
	size_t word = 0;
	for (int i = 0; i < blen; i += sizeof(size_t)) {
		word = ptrace(PTRACE_PEEKTEXT, pid, (void *)(addr + i), NULL);
		memcpy(&buf[i], &word, sizeof(word));
	}
	return 0;
}

static int
callback(struct dl_phdr_info *info, size_t size, void *data)
{
	int j;
	const char *cb = (const char *)&callback;
	const char *base = (const char *)info->dlpi_addr;
	const ElfW(Phdr) *first_load = NULL;

	for (j = 0; j < info->dlpi_phnum; j++) {
		const ElfW(Phdr) *phdr = &info->dlpi_phdr[j];

	if (phdr->p_type == PT_LOAD) {
		const char *beg = base + phdr->p_vaddr;
		const char *end = beg + phdr->p_memsz;

		if (first_load == NULL) first_load = phdr;
		if (beg <= cb && cb < end) {
			// Found PT_LOAD that "covers" callback().
			printf("ELF header is at %p, image linked at 0x%zx, relocation: 0x%zx\n",
			base + first_load->p_vaddr, first_load->p_vaddr, info->dlpi_addr);
			return 1;
		}
		}
	}
	return 0;
}

void
dl_phdr_lookup_image(void)
{
	dl_iterate_phdr(callback, NULL);
}

int32_t
calc_jmp(void *from, void *to) {
	/* 5 bytes in a JMP/CALL rel32 instruction. */
	int64_t delta = (int64_t)to - (int64_t)from - 5;
	if (delta < INT_MIN || delta > INT_MAX)
		exit(1);
	return (int32_t)delta;
}
