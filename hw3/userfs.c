#include "userfs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char *memory;
	/** How many bytes are occupied. */
	int occupied;
	/** Next block in the file. */
	struct block *next;
	/** Previous block in the file. */
	struct block *prev;
};

struct file {
	/** Double-linked list of file blocks. */
	struct block *block_list;
	/**
	 * Last block in the list above for fast access to the end
	 * of file.
	 */
	struct block *last_block;
	/** How many file descriptors are opened on the file. */
	int refs;
	/** Size of file in bytes*/
	int size;
	/** File deletion status*/
	int is_deleted;
	/** File name. */
	char *name;
	/** Files are stored in a double-linked list. */
	struct file *next;
	struct file *prev;

};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
	struct file *file;
	int block;
	int block_pos;
	int flags;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;


enum ufs_error_code ufs_errno() {
	return ufs_error_code;
}

static void ufs_realloc_desc_array() {
	if (!file_descriptors) {
		file_descriptors = malloc(sizeof(struct filedesc *));
		*file_descriptors = NULL;
		file_descriptor_capacity = 1;
		return;
	}
	file_descriptors = realloc(file_descriptors, sizeof(struct filedesc *) * file_descriptor_capacity * 2);
	file_descriptor_capacity *= 2;
	for (int i = file_descriptor_count; i < file_descriptor_capacity; ++i) {
		file_descriptors[i] = NULL;
	}
}

static int ufs_get_desc(struct file *file, int flags) {
	if (file_descriptor_count == file_descriptor_capacity) {
		ufs_realloc_desc_array();
	}
	for (int i = 0; i < file_descriptor_capacity; ++i) {
		if (file_descriptors[i] == NULL) {
			struct filedesc *fd = malloc(sizeof(struct filedesc));
			file_descriptors[i] = fd;
			file_descriptor_count++;
			fd->file = file;
			file->refs++;
			fd->block = 0;
			fd->block_pos = 0;
			fd->flags = flags;
			return i + 1;
		}
	}
	return -1;
}

static int ufs_validate_fd(int fd) {
	return fd < 1 || fd > file_descriptor_capacity + 1 || file_descriptors[fd - 1] == NULL;
}

static struct file *ufs_find_file(const char *filename) {
	struct file *file = file_list;
	while (file != NULL && (strcmp(filename, file->name) || file->is_deleted)) {
		file = file->next;
	}
	return file;
}

static struct file *new_file(const char *filename) {
	struct file *file = malloc(sizeof(struct file));
	file->name = strdup(filename);
	file->refs = 0;
	file->is_deleted = 0;
	file->block_list = NULL;
	file->last_block = NULL;

	file->prev = NULL;			
	file->next = file_list;
	if (file_list) {
		file_list->prev = file;
	}
	file_list = file;

	return file;
}

static void delete_file(struct file *file) {
		if (file->next) {
			file->next->prev = file->prev;
		}
		if (file->prev) {
			file->prev->next = file->next;
		}
		if (file_list == file) {
			file_list = file->next;
		}
		struct block *block = file->block_list;
		while(block) {
			struct block *tmp = block;
			block = block->next;
			free(tmp->memory);
			free(tmp);
		}
		free(file->name);
		free(file);
}

static struct block *new_block(struct block *prev) {
	struct block *block = malloc(sizeof(struct block));
	block->memory = malloc(BLOCK_SIZE);
	block->occupied = 0;
	block->prev = prev;
	if (prev) {
		prev->next = block;
	}
	block->next = NULL;
	return block;
}

int ufs_open(const char *filename, int flags) {
	struct file *file = ufs_find_file(filename);
	if (file == NULL && !(flags & UFS_CREATE)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	if (file == NULL) {
		file = new_file(filename);
	}

	int fd  = ufs_get_desc(file, flags);

	return fd;
}

ssize_t ufs_write(int fd, const char *buf, size_t size) {
	if (ufs_validate_fd(fd)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;		
	}
	struct filedesc *filedesc = file_descriptors[fd - 1];
	struct file *file = filedesc->file;
	if (filedesc->block * BLOCK_SIZE + filedesc->block_pos + size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	if (!file->block_list) {
		struct block *block = new_block(NULL);
		file->block_list = block;
		file->last_block = block;
	}
	size_t done = 0;
	struct block *block = file->block_list;
	for (int i = 0; i < filedesc->block; ++i) {
		block = block->next;
	}
	while (done < size) {
		if (filedesc->block_pos == BLOCK_SIZE) {
			new_block(block);
			block = block->next;
			file->last_block = block;
			filedesc->block++;
			filedesc->block_pos = 0;
		}
		size_t cpy_size = BLOCK_SIZE - filedesc->block_pos;
		if (size - done < cpy_size) {
			cpy_size = size - done;
		}
		memcpy(block->memory + filedesc->block_pos, buf + done, cpy_size);
		filedesc->block_pos += cpy_size;
		if (filedesc->block_pos > block->occupied) {
			block->occupied = filedesc->block_pos;
		}
		done += cpy_size;
	}
	return done;
}

ssize_t ufs_read(int fd, char *buf, size_t size) {
	if (ufs_validate_fd(fd)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;		
	}
	struct filedesc *filedesc = file_descriptors[fd - 1];
	struct file *file = filedesc->file;
	size_t done = 0;
	struct block *block = file->block_list;
	for (int i = 0; i < filedesc->block; ++i) {
		block = block->next;
	}
	while (block && done < size) {
		if (filedesc->block_pos == block->occupied) {
			block = block->next;
			if (block) {
				filedesc->block++;
				filedesc->block_pos = 0;
			}
		}
		if (!block) {
			return done;
		}
		size_t cpy_size = block->occupied - filedesc->block_pos;
		if (size - done < cpy_size) {
			cpy_size = size - done;
		}
		memcpy(buf + done, block->memory + filedesc->block_pos, cpy_size);
		filedesc->block_pos += cpy_size;	
		done += cpy_size;
	}
	
	return done;
}

int ufs_close(int fd) {
	if (ufs_validate_fd(fd)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	file_descriptors[fd - 1]->file->refs--;
	free(file_descriptors[fd - 1]);
	file_descriptor_count--;
	file_descriptors[fd - 1] = NULL;
	return 0;
}

int ufs_delete(const char *filename) {
	struct file *file = ufs_find_file(filename);
	if (!file) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	file->is_deleted = 1;
	if (!file->refs) {
		delete_file(file);
	}

	return 0;
}

void ufs_destroy(void) {
	struct file *file = file_list;
	while (file) {
		struct file *tmp = file;
		file = file->next;
		delete_file(tmp);
	}
	for (int i = 0; i < file_descriptor_capacity; ++i) {
		if (file_descriptors[i]) {
			free(file_descriptors[i]);
		}
	}
	free(file_descriptors);
}
