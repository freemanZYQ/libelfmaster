#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <elf.h>
#include <sys/types.h>
#include <search.h>
#include <sys/time.h>
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <search.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include "../include/libelfmaster.h"
#include "../include/internal.h"

/* includes
bool elf_create_object(const char *path, struct elfobj *obj, size_t size, uint64_t load_flags, elf_error_t *); 
bool elf_inject_code(struct elfobj_t *host, struct elfobj_t *target, void *payload, uint64_t injection_flags, elf_error_t *error);
bool elf_commit_object(struct elfobj *obj, size_t size, int offset, elf_error_t *error);
*/

// macros
#define ELF_INJECT_DATA 		0
#define ELF_INJECT_REVERSE_CODE		1
#define ELF_ET_STUB  			-1
#define PAGE_SIZE 0x1000
#define PAGE_ALIGN(x) (x & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x) (PAGE_ALIGN(x) + PAGE_SIZE)

// -----------------------------------ulexec

/*
 * Creates an elf object. initial content can be specified to contain a given ELF file.
 */
bool
elf_create_object(const char *path, struct elfobj *obj, struct elfobj *copy, size_t size, uint64_t load_flags, elf_error_t *error) 
{
	int fd;
	unsigned int open_flags = O_RDWR|O_CREAT|O_APPEND;
	unsigned int mmap_perms = PROT_READ|PROT_WRITE;
	unsigned int mmap_flags = MAP_PRIVATE;
	uint8_t *mem;

	/*
	 * we count on this being initialized for various sanity checks.
	 */
	memset(obj, 0, sizeof(*obj));	

	/*
	 * we check if the file wants to be created containing a particular elf object
	 */
	if (copy != NULL) {
		memcpy(obj, copy, sizeof(struct elfobj));
		memcpy(obj->mem, copy->mem, copy->size);
	}
	obj->load_flags = load_flags;
	obj->path = path;

	if (load_flags & ELF_LOAD_F_MODIFY) {
		mmap_flags = MAP_SHARED;
	}
	fd = open(path, open_flags, S_IRUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		elf_error_set(error, "open: %s", strerror(errno));
		return false;
	}
	obj->fd = fd;

	if (size != 0 && copy == NULL) {
		obj->size = size;
		mem = mmap(NULL, obj->size, mmap_perms, mmap_flags, fd, 0);
		if (mem == MAP_FAILED) {
			elf_error_set(error, "mmap: %s", strerror(errno));
			close(fd);
			return false;
		}
		obj->mem = mem;
	} else if (size == 0 && copy == NULL) {
		fprintf(stderr, "Invalid argument: size\n");
		return false;
	}
	return true;
}

/*
 * Writes a given elfobj_t instance to disk
 */
bool 
elf_commit_object(struct elfobj *obj, size_t size, int offset, elf_error_t *error) 
{
	if (pwrite(obj->fd, obj->mem, size, offset) != size) {
		elf_error_set(error, "pwrite: %s", strerror(errno));
		return false;
	}
	return true;
}

/*
 * Handles several injection techniques.
 */
bool 
elf_inject_code(struct elfobj *host, struct elfobj *target, uint64_t *payload_offset, 
		uint64_t injection_flags, elf_error_t *error) 
{
	uint64_t offset;
	uint8_t *dest_mem;
	elf_segment_iterator_t p_iter;
	struct elf_segment segment;
	bool injection_end = false;
	size_t final_size = host->size + target->size;

	if((dest_mem = (uint8_t *)calloc(1, final_size)) == NULL) {
		elf_error_set(error, "calloc: %s", strerror(errno));
		return false;
	}

	switch(injection_flags) {
		case ELF_INJECT_DATA: {
			host->data_offset = elf_data_offset(host);
			elf_segment_iterator_init(host, &p_iter);
			while (elf_segment_iterator_next(&p_iter, &segment) == ELF_ITER_OK) {
				if(segment.type == PT_LOAD && segment.offset == host->data_offset) {
					if (host->e_class == ELFCLASS32) {
						Elf32_Phdr *phdr = &host->phdr32[p_iter.index-1];
						offset = phdr->p_offset + phdr->p_memsz;
						phdr->p_memsz += PAGE_ALIGN_UP(target->size);
						phdr->p_filesz += PAGE_ALIGN_UP(target->size);
						phdr->p_flags |= PF_X;

					} else {	
						Elf64_Phdr *phdr = &host->phdr64[p_iter.index-1];
						offset = phdr->p_offset + phdr->p_memsz;
						phdr->p_memsz += PAGE_ALIGN_UP(target->size);
						phdr->p_filesz += PAGE_ALIGN_UP(target->size);
						phdr->p_flags |= PF_X;
				
					}
					memcpy(dest_mem, host->mem, host->size);
					memcpy(dest_mem + offset, target->mem, target->size);
					host->mem = dest_mem;
					host->size = final_size;
					injection_end = true;
					break;
				}
			}
			break;
		}
		case ELF_INJECT_REVERSE_CODE:
			break;	
	}
	if (injection_end == false) {
		elf_error_set(error, "Target segment was not found for injection");
		return false;
	}	
	*payload_offset = offset;
	return true;	
}

/*
 *  Returns the RVA of a given address
 */
uint64_t 
internal_address_to_rva(struct elfobj *obj, uint64_t address) 
{
	return address - obj->text_address;
}

/*
 * Returns segment virtual address delta to its disk offset. Implemented to handle segment
 * alignment properlly.
 */
uint64_t
internal_segment_offset_delta(struct elfobj *obj, struct elf_segment *segment) {
	return segment->offset - internal_address_to_rva(obj, segment->vaddr);
}

/*
 * Translates a given offset to its equivalent address for a given elfobj_t instance.
 */
bool
internal_offset_to_address(struct elfobj *obj, uint64_t offset, uint64_t *address) 
{
	elf_segment_iterator_t p_iter;
	struct elf_segment segment;

	elf_segment_iterator_init(obj, &p_iter);
	while (elf_segment_iterator_next(&p_iter, &segment) == ELF_ITER_OK) {
		if(segment.type == PT_LOAD) {
			if(segment.offset <= offset && 
					offset <= PAGE_ALIGN_UP(segment.offset + segment.filesz)) {
				*address = (offset + (segment.vaddr - segment.offset));
				return true;
			}
		}
	}
	return false;
}

/*
 * Translates a given address to its equivalent offset for a given elfobj_t instance.
 */
bool 
internal_address_to_offset(struct elfobj *obj, uint64_t address, uint64_t *offset) 
{
	elf_segment_iterator_t p_iter;
	struct elf_segment segment;

	elf_segment_iterator_init(obj, &p_iter);
	while (elf_segment_iterator_next(&p_iter, &segment) == ELF_ITER_OK) {
		if (segment.type == PT_LOAD) {
			*offset = (internal_address_to_rva(obj, address) +
					internal_segment_offset_delta(obj, &segment));
			return true;
		}
	}
	return false;
}

/*
 * Opens a given file and checks if it contains an ELF magic. (More checks can be implemented).
 */
bool
elf_has_header(const char *path, bool *has_header, elf_error_t *error) 
{
 	int fd;
        int magic;

        if((fd = open(path, O_RDONLY)) < 0) {
		elf_error_set(error, "open: %s", strerror(errno));
		return false;
	}
	
	if(pread(fd, &magic, sizeof(int), 0) != sizeof(int)) {
		elf_error_set(error, "pread: %s", strerror(errno));
		return false;
	}
	*has_header = magic == 0x464c457f ? true : false;
	close(fd);
	return true;
}

/*
 * Handles opening binary stubs that reside in disk. Saves instance as a elfobj_t.
 */
bool
elf_open_stub(const char *path, struct elfobj *obj, elf_error_t *error) 
{
	int fd;
	struct stat st;
	unsigned int open_flags = O_RDWR;
	unsigned int mmap_perms = PROT_READ|PROT_WRITE;
	unsigned int mmap_flags = MAP_PRIVATE;
	uint8_t *mem;

	memset(obj, 0, sizeof(*obj));
	if((fd = open(path, open_flags, S_IRUSR|S_IRGRP|S_IROTH)) < 0) {
		elf_error_set(error, "open: %s", strerror(errno));
		return false;
	}
	obj->fd = fd;
	obj->path = path;
		
	if (fstat(fd, &st) < 0) {
		elf_error_set(error, "fstat: %s", strerror(errno));
		close(fd);
		return false;
	}
	obj->size = st.st_size;

	if((mem = mmap(NULL, st.st_size, mmap_perms, mmap_flags, fd, 0)) == MAP_FAILED) {
		elf_error_set(error, "mmap: %s", strerror(errno));
		close(fd);
		return false;
	}
	obj->mem = mem;
	obj->type = ELF_ET_STUB;
	return true;
}

/*
 * Handles initialization of a elfobj_t for a given stub supplied as a binary array.
 */
bool
elf_init_stub(struct elfobj *obj, uint8_t *mem, int size, elf_error_t *error) 
{
	if (obj == NULL) {
		elf_error_set(error, "Invalid elfobj_t address");
		return false;
	}
	if (mem == NULL) {
		elf_error_set(error, "Invalid stub buffer address");
		return false;
	}
	obj->type = ELF_ET_STUB;
	obj->mem = mem;
	obj->size = size;
	return true;
}

int main (int argc, char **argv) 
{
	elfobj_t obj1;
	elfobj_t obj2;
        elfobj_t objdest;
	elf_error_t error;
	struct elf_symbol sym;
	uint64_t p_address, p_offset;
	bool res;

	if (argc != 3) {
		printf("Usage: %s <host> <output binary>\n", argv[0]);
		exit(EXIT_SUCCESS);
	}

	if (elf_open_object(argv[1], &obj1, ELF_LOAD_F_STRICT, &error) == false) {
		fprintf(stderr, "%s\n", elf_error_msg(&error));
		return -1;
	}

	/* 
	 * In case the binary payload is residing in memory, we can use these functions to load
	 * it, as if it was an elf or some binary blob.
	 *
	if (elf_has_header(argv[2], &res, &error) == false) {
		fprintf(stderr, "%s\n", elf_error_msg(&error));
		return -1;
	}
	if (res) {
		if (elf_open_object(argv[2], &obj2, ELF_LOAD_F_STRICT, &error) == false) {
			fprintf(stderr, "%s\n", elf_error_msg(&error));
			return -1;
		}
	} else {
		if (elf_open_stub(argv[2], &obj2, &error) == false) {
			fprintf(stderr, "%s\n", elf_error_msg(&error));
			return -1;
		}
	}
	 * Otherwise stubs can be also operated as byte-arrays.
	 */

	// http://shell-storm.org/shellcode/files/shellcode-806.php
	uint8_t stub[27] = "\x31\xc0\x48\xbb\xd1\x9d\x96\x91\xd0\x8c\x97\xff\x48" \
		         "\xf7\xdb\x53\x54\x5f\x99\x52\x57\x54\x5e\xb0\x3b\x0f\x05";
	if (elf_init_stub(&obj2, stub, sizeof(stub), &error) == false) {
		fprintf(stderr, "%s\n", elf_error_msg(&error));
		return -1;
	}
	if (elf_create_object(argv[2], &objdest, &obj1, obj1.size + obj2.size,
			       	ELF_LOAD_F_STRICT, &error) == false) {
		fprintf(stderr, "%s\n", elf_error_msg(&error));
		return -1;
	}
	if (elf_inject_code(&objdest, &obj2, &p_offset, ELF_INJECT_DATA, &error) == false) {
		fprintf(stderr, "%s\n", elf_error_msg(&error));
		return -1;
	} 
	if(internal_offset_to_address(&objdest, p_offset, &p_address) == false) {
		fprintf(stderr, "%s\n", elf_error_msg(&error));
		return -1;
	}
	printf("Payload written at offset: 0x%lx, address: 0x%lx\n", p_offset, p_address);
	
	if (elf_commit_object(&objdest, objdest.size, 0, &error) == false) {
		fprintf(stderr, "%s\n", elf_error_msg(&error));
		return -1;
	}
	
	return 0;
}
