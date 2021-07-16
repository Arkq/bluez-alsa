#include "ring-buffer.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>

int ring_buff_init(ring_buff_t *ring_buff, size_t size) {
	void *ptr;
	if ((ptr = (unsigned char*)realloc(ring_buff->data, size)) == NULL)
		return -1;

	ring_buff->data = ptr;
	ring_buff->write_pos = 0;
	ring_buff->read_pos = 0;
	ring_buff->size = size;
	ring_buff->full = false;

	return 0;
}

void ring_buff_free(ring_buff_t *ring_buff) {
	if (ring_buff->data == NULL)
		return;
	free(ring_buff->data);
	ring_buff->data = NULL;
	ring_buff->write_pos = 0;
	ring_buff->read_pos = 0;
}

int ring_buff_resize(ring_buff_t *ring_buff, size_t new_size) {
	if (ring_buff->size == new_size)
		return 0;

	printf("Resizing Ring Buffer from %u to %u bytes\n", ring_buff->size, new_size);

	unsigned char *new_buff = (unsigned char*)malloc(new_size);
	size_t new_write_pos;
	bool new_full;

	if (new_size >= ring_buff_size(ring_buff)) {
		// copy all

		if (ring_buff->write_pos >= ring_buff->read_pos && !ring_buff->full) {
			// 1 copy
			size_t copy_len = ring_buff->write_pos - ring_buff->read_pos;
			memcpy(new_buff, &ring_buff->data[ring_buff->read_pos], copy_len);
		} else {
			// 2 copies
			size_t copy_len1 = ring_buff->size - ring_buff->read_pos;
			size_t copy_len2 = ring_buff->write_pos;

			memcpy(new_buff, &ring_buff->data[ring_buff->read_pos], copy_len1);
			memcpy(&new_buff[copy_len1], ring_buff->data, copy_len2);
		}

		new_write_pos = ring_buff_size(ring_buff);
		new_full = false;
	} else {
		// copy most recent new_size bytes

		if ((ring_buff->write_pos >= ring_buff->read_pos && !ring_buff->full) || ring_buff->write_pos >= new_size) {
			// 1 copy
			size_t copy_len = new_size; // bc new_size < write_pos - read_pos
			size_t copy_from = ring_buff->write_pos - copy_len; // most recent bytes
			memcpy(new_buff, &ring_buff->data[copy_from], copy_len);
		} else {
			// 2 copies
			size_t copy_len2 = ring_buff->write_pos;
			size_t copy_len1 = new_size - copy_len2;

			size_t copy_from = ring_buff->size - copy_len1;

			memcpy(new_buff, &ring_buff->data[copy_from], copy_len1);
			memcpy(&new_buff[copy_len1], ring_buff->data, copy_len2);
		}

		new_write_pos = 0;
		new_full = true;
	}

	unsigned char *old_buff = ring_buff->data;
	ring_buff->data = new_buff;
	ring_buff->write_pos = new_write_pos;
	ring_buff->read_pos = 0;
	ring_buff->size = new_size;
	ring_buff->full = new_full;

	free(old_buff);

	return 1;
}


bool ring_buff_is_full(ring_buff_t *ring_buff) {
	return ring_buff->full;
}

bool ring_buff_is_empty(ring_buff_t *ring_buff) {
	return !ring_buff->full && ring_buff->write_pos == ring_buff->read_pos;
}


size_t ring_buff_capacity(ring_buff_t *ring_buff) {
	return ring_buff->size;
}

size_t ring_buff_size(ring_buff_t *ring_buff) {
	size_t size = ring_buff->size;

	if (!ring_buff->full) {
		if (ring_buff->write_pos >= ring_buff->read_pos) {
			size = ring_buff->write_pos - ring_buff->read_pos;
		} else {
			size = ring_buff->size + ring_buff->write_pos - ring_buff->read_pos;
		}
	}

	return size;
}

void ring_buff_rewind(ring_buff_t *ring_buff) {
	ring_buff->write_pos = 0;
	ring_buff->read_pos = 0;
	ring_buff->full = false;
}

ssize_t ring_buff_write(ring_buff_t *ring_buff, int fd) {
	size_t len = ring_buff->size - ring_buff->write_pos;
	size_t readable = ring_buff_size(ring_buff);

	// set filedescriptor to non blocking mode
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	size_t totalRead = 0;
	int s;

	while((s = read(fd, &ring_buff->data[ring_buff->write_pos], len)) > 0) {
		ring_buff->write_pos = (ring_buff->write_pos + s) % ring_buff->size;
		len = ring_buff->size - ring_buff->write_pos;
		totalRead += s;
	}

	if (s == -1 && errno != EAGAIN) {
		return -1;
	}

	ring_buff->full = ring_buff->full || (readable + totalRead >= ring_buff->size);

	if (ring_buff->full) {
		ring_buff->read_pos = ring_buff->write_pos;
	}

	return totalRead;
}

size_t ring_buff_read(ring_buff_t *ring_buff, unsigned char *buf, size_t max) {
	if (max <= 0)
		return 0;

	if (max > ring_buff_size(ring_buff)) {
		max = ring_buff_size(ring_buff);
	}

	if (ring_buff->read_pos + max <= ring_buff->size) { // read all at once
		memcpy(buf, &ring_buff->data[ring_buff->read_pos], max);
	} else { // read 2 times
		size_t read = ring_buff->size - ring_buff->read_pos;

		memcpy(buf, &ring_buff->data[ring_buff->read_pos], read);
		memcpy(&buf[read], ring_buff->data, max - read);
	}

	return max;
}

void ring_buff_shift(ring_buff_t *ring_buff, size_t bytes) {
	if (bytes == 0)
		return;

	ring_buff->read_pos = (ring_buff->read_pos + bytes) % ring_buff->size;
	ring_buff->full = false;
}
