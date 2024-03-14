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
	size_t size;
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

static struct block *ufs_push_block(struct block *prev) {
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

static struct block *ufs_pop_block(struct block *block) {
	struct block *tmp = block;
	block = block->prev;
	if (block) {
		block->next = NULL;
	}
	free(tmp->memory);
	free(tmp);
	return block;
}

static struct file *ufs_find_file(const char *filename) {
	struct file *file = file_list;
	while (file != NULL && (strcmp(filename, file->name) || file->is_deleted)) {
		file = file->next;
	}
	return file;
}

static struct file *ufs_new_file(const char *filename) {
	struct file *file = malloc(sizeof(struct file));
	file->name = strdup(filename);
	file->refs = 0;
	file->is_deleted = 0;
	file->size = 0;
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

static void ufs_delete_file(struct file *file) {
		if (file->next) {
			file->next->prev = file->prev;
		}
		if (file->prev) {
			file->prev->next = file->next;
		}
		if (file_list == file) {
			file_list = file->next;
		}
		while(file->last_block) {
			file->last_block = ufs_pop_block(file->last_block);
		}
		free(file->name);
		free(file);
}

int ufs_open(const char *filename, int flags) {
	struct file *file = ufs_find_file(filename);
	if (file == NULL && !(flags & UFS_CREATE)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	if (file == NULL) {
		file = ufs_new_file(filename);
	}
	if (!(flags & UFS_READ_ONLY) && !(flags & UFS_WRITE_ONLY)) {
		flags |= UFS_READ_WRITE;
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
	if (!(filedesc->flags & (UFS_WRITE_ONLY | UFS_READ_WRITE))) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
	if (filedesc->block * BLOCK_SIZE + filedesc->block_pos + size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	if (!file->block_list) {
		struct block *block = ufs_push_block(NULL);
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
			block = ufs_push_block(block);
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
		if ((size_t)(filedesc->block_pos + filedesc->block * BLOCK_SIZE) > file->size) {
			file->size = filedesc->block_pos + filedesc->block * BLOCK_SIZE;
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
	if (!(filedesc->flags & (UFS_READ_ONLY | UFS_READ_WRITE))) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
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
	struct file *file = file_descriptors[fd - 1]->file;
	file->refs--;
	if (!file->refs-- && file->is_deleted) {
		ufs_delete_file(file);
	}
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
		ufs_delete_file(file);
	}

	return 0;
}

#ifdef NEED_RESIZE
int ufs_resize(int fd, size_t new_size) {
	if (ufs_validate_fd(fd)) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;		
	}
	struct filedesc *filedesc = file_descriptors[fd - 1];
	struct file *file = filedesc->file;
	if (!(filedesc->flags & (UFS_WRITE_ONLY | UFS_READ_WRITE))) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
	if (new_size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	int new_block = (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
	int new_block_pos = new_size % BLOCK_SIZE;
	while (file->size > new_size) {
		if (file->size - file->last_block->occupied > new_size) {
			file->size -= file->last_block->occupied;
			file->last_block = ufs_pop_block(file->last_block);
		} else {
			file->last_block->occupied = new_block_pos;
			file->size = new_size;
		}
	}
	if (!new_size) {
		file->block_list = NULL;
	}
	while(file->size < (size_t)new_size) {
		if (file->size - file->last_block->occupied + BLOCK_SIZE < new_size) {
			file->size -= file->last_block->occupied;
			file->last_block->occupied = BLOCK_SIZE;
			file->size += file->last_block->occupied;
			file->last_block = ufs_push_block(file->last_block);
		} else {
			file->last_block->occupied = new_block_pos;
			file->size = new_size;
		}
	}
	for (int i = 0; i < file_descriptor_capacity; ++i) {
		struct filedesc *tmp = file_descriptors[i];
		if (tmp && tmp->file == file) {
			if ((size_t)(tmp->block * BLOCK_SIZE + tmp->block_pos) > new_size) {
				tmp->block = new_block - 1;
				tmp->block_pos = new_block_pos;
			}
		}
	}
	return 0;
}
#endif

void ufs_destroy(void) {
	struct file *file = file_list;
	while (file) {
		struct file *tmp = file;
		file = file->next;
		ufs_delete_file(tmp);
	}
	for (int i = 0; i < file_descriptor_capacity; ++i) {
		if (file_descriptors[i]) {
			free(file_descriptors[i]);
		}
	}
	free(file_descriptors);
}
