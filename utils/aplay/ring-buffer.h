#ifndef BLUEALSA_SHARED_RING_BUFFER_H_
#define BLUEALSA_SHARED_RING_BUFFER_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

typedef struct {
	unsigned char *data;
	size_t write_pos;
	size_t read_pos;
	size_t size;
	bool full;
} ring_buff_t;

int ring_buff_init(ring_buff_t *ring_buff, size_t size);
void ring_buff_free(ring_buff_t *ring_buff);

int ring_buff_resize(ring_buff_t *ring_buff, size_t new_size);

bool ring_buff_is_full(ring_buff_t *ring_buff);
bool ring_buff_is_empty(ring_buff_t *ring_buff);

size_t ring_buff_capacity(ring_buff_t *ring_buff);
size_t ring_buff_size(ring_buff_t *ring_buff);

void ring_buff_rewind(ring_buff_t *ring_buff);

ssize_t ring_buff_write(ring_buff_t *ring_buff, int fd);
size_t ring_buff_read(ring_buff_t *ring_buff, unsigned char *buf, size_t max);

void ring_buff_shift(ring_buff_t *ring_buff, size_t bytes);

#endif
