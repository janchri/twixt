/*
 * Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux side of the native_sim multi-master I2C bus: the shared wires.
 * See i2c_mm_native_bottom.h. Runs in the native_simulator runner (host libc).
 *
 * The state lives in POSIX shared memory (/dev/shm/twixt_i2c_mm_<bus-id>),
 * guarded by a process-shared robust mutex. Slots of processes that died are
 * reclaimed, so a crashed run does not block the next one.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i2c_mm_native_bottom.h"

#define MAX_SLOTS  8
#define WIRE_MAGIC 0x33634d4dU

struct slot {
	pid_t pid;           /* 0: slot is free */
	int32_t target_addr; /* -1: not a target */
	uint32_t ev_seq;     /* last event posted to this target */
	uint32_t ans_seq;    /* last event this target has answered */
	int32_t ev;
	int32_t result;
	uint8_t ev_byte;
	uint8_t ans_byte;
	uint8_t done; /* test bench: i2c_mm_native_bottom_set_done() */
};

struct wire {
	pthread_mutex_t lock;
	volatile uint32_t magic;
	int32_t owner; /* slot of the master that owns the bus, -1: free */
	int32_t stuck; /* a target holds SDA low: nobody can START until a recovery */
	int32_t key_len;
	uint8_t key[I2C_MM_NATIVE_BOTTOM_KEY_MAX];
	int64_t start_ns; /* when the owner's START began */
	int64_t epoch_ns; /* when the first process of this run joined */
	struct slot slots[MAX_SLOTS];
};

static struct wire *wire;
static int my_slot = -1;
static char shm_name[48];

static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t i2c_mm_native_bottom_wall_us(void)
{
	return mono_ns() / 1000;
}

int64_t i2c_mm_native_bottom_epoch_us(void)
{
	return wire->epoch_ns / 1000;
}

static void lock(void)
{
	if (pthread_mutex_lock(&wire->lock) == EOWNERDEAD) {
		pthread_mutex_consistent(&wire->lock);
	}
}

static void unlock(void)
{
	pthread_mutex_unlock(&wire->lock);
}

static int alive(pid_t pid)
{
	return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

/* Call with the lock held */
static void free_slot(int i)
{
	wire->slots[i].pid = 0;
	wire->slots[i].target_addr = -1;
	if (wire->owner == i) {
		wire->owner = -1;
	}
}

/* Call with the lock held */
static int bus_empty(void)
{
	for (int i = 0; i < MAX_SLOTS; i++) {
		if (wire->slots[i].pid != 0) {
			return 0;
		}
	}
	return 1;
}

/* Free the slots of dead processes. Call with the lock held. */
static void reap(void)
{
	for (int i = 0; i < MAX_SLOTS; i++) {
		if (wire->slots[i].pid != 0 && !alive(wire->slots[i].pid)) {
			free_slot(i);
		}
	}
}

static void detach(void)
{
	int empty;

	if (!wire) {
		return;
	}
	lock();
	free_slot(my_slot);
	empty = bus_empty();
	unlock();
	if (empty) {
		shm_unlink(shm_name);
	}
}

static struct wire *map_wire(int *created)
{
	for (int attempt = 0; attempt < 3; attempt++) {
		int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
		struct wire *w;

		*created = fd >= 0;
		if (fd < 0 && errno == EEXIST) {
			fd = shm_open(shm_name, O_RDWR, 0600);
		}
		if (fd < 0) {
			return NULL;
		}
		if (*created) {
			if (ftruncate(fd, sizeof(struct wire)) < 0) {
				close(fd);
				return NULL;
			}
		} else {
			struct stat st = {0};

			/* The creator may still be sizing it */
			for (int i = 0; i < 100 && st.st_size != sizeof(struct wire); i++) {
				fstat(fd, &st);
				usleep(1000);
			}
			if (st.st_size != sizeof(struct wire)) {
				close(fd); /* left over from another build */
				shm_unlink(shm_name);
				continue;
			}
		}
		w = mmap(NULL, sizeof(struct wire), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		return w == MAP_FAILED ? NULL : w;
	}
	return NULL;
}

int i2c_mm_native_bottom_attach(int bus_id)
{
	int created = 0;

	if (wire) {
		return -EBUSY; /* one port per process */
	}
	snprintf(shm_name, sizeof(shm_name), "/twixt_i2c_mm_%d", bus_id);

	wire = map_wire(&created);
	if (!wire) {
		return -errno ? -errno : -EIO;
	}

	if (created) {
		pthread_mutexattr_t attr;

		pthread_mutexattr_init(&attr);
		pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
		pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
		pthread_mutex_init(&wire->lock, &attr);
		wire->owner = -1;
		for (int i = 0; i < MAX_SLOTS; i++) {
			wire->slots[i].target_addr = -1;
		}
		__atomic_store_n(&wire->magic, WIRE_MAGIC, __ATOMIC_RELEASE);
	} else {
		for (int i = 0;
		     i < 1000 && __atomic_load_n(&wire->magic, __ATOMIC_ACQUIRE) != WIRE_MAGIC;
		     i++) {
			usleep(1000);
		}
		if (wire->magic != WIRE_MAGIC) {
			wire = NULL;
			return -EIO;
		}
	}

	lock();
	reap();
	if (bus_empty()) {
		wire->epoch_ns = mono_ns(); /* nobody else is on the bus: a new run */
	}
	for (int i = 0; i < MAX_SLOTS; i++) {
		if (wire->slots[i].pid == 0) {
			struct slot *s = &wire->slots[i];

			memset(s, 0, sizeof(*s));
			s->pid = getpid();
			s->target_addr = -1;
			my_slot = i;
			break;
		}
	}
	unlock();

	if (my_slot < 0) {
		wire = NULL;
		return -ENOMEM;
	}
	atexit(detach);
	return 0;
}

int i2c_mm_native_bottom_targets(void)
{
	int n = 0;

	lock();
	reap();
	for (int i = 0; i < MAX_SLOTS; i++) {
		n += wire->slots[i].pid != 0 && wire->slots[i].target_addr >= 0;
	}
	unlock();
	return n;
}

void i2c_mm_native_bottom_set_done(void)
{
	lock();
	wire->slots[my_slot].done = 1;
	unlock();
}

int i2c_mm_native_bottom_done_count(void)
{
	int n = 0;

	lock();
	reap();
	for (int i = 0; i < MAX_SLOTS; i++) {
		n += wire->slots[i].pid != 0 && wire->slots[i].done;
	}
	unlock();
	return n;
}

static int find_target(int addr, int with_mine)
{
	for (int i = 0; i < MAX_SLOTS; i++) {
		const struct slot *s = &wire->slots[i];

		if ((i != my_slot || with_mine) && s->pid != 0 && s->target_addr == addr) {
			return i;
		}
	}
	return -1;
}

int i2c_mm_native_bottom_set_target(int addr)
{
	int ret = 0;

	lock();
	if (addr >= 0 && find_target(addr, 0) >= 0) {
		ret = -EADDRINUSE;
	} else {
		wire->slots[my_slot].target_addr = addr;
	}
	unlock();
	return ret;
}

int i2c_mm_native_bottom_find_target(int addr, int with_mine)
{
	int found;

	lock();
	found = find_target(addr, with_mine);
	unlock();
	return found;
}

int i2c_mm_native_bottom_start(const uint8_t *key, int key_len, int join_us)
{
	int64_t now = mono_ns();
	int ret = I2C_MM_NATIVE_BOTTOM_OWNER;

	lock();
	reap();
	if (wire->stuck) {
		ret = I2C_MM_NATIVE_BOTTOM_STUCK;
	} else if (wire->owner < 0) {
		wire->start_ns = now;
	} else if (now - wire->start_ns < (int64_t)join_us * 1000) {
		/* The owner's START is just happening: both drive the bus */
		int n = key_len < wire->key_len ? key_len : wire->key_len;

		if (memcmp(key, wire->key, n) >= 0) {
			ret = I2C_MM_NATIVE_BOTTOM_LOST;
		}
	} else {
		ret = I2C_MM_NATIVE_BOTTOM_BUSY;
	}
	if (ret == I2C_MM_NATIVE_BOTTOM_OWNER) {
		wire->owner = my_slot;
		wire->key_len = key_len;
		memcpy(wire->key, key, key_len);
	}
	unlock();
	return ret;
}

int i2c_mm_native_bottom_confirm(int join_us)
{
	int ret;

	lock();
	if (wire->owner != my_slot) {
		ret = 0;
	} else if (mono_ns() - wire->start_ns >= (int64_t)join_us * 1000) {
		ret = 1;
	} else {
		ret = -1;
	}
	unlock();
	return ret;
}

int i2c_mm_native_bottom_active(void)
{
	return __atomic_load_n(&wire->owner, __ATOMIC_ACQUIRE) >= 0;
}

/* Unlocked: has the target answered the last event posted to it? Recheck under the lock. */
static int seq_equal(struct slot *s)
{
	return __atomic_load_n(&s->ans_seq, __ATOMIC_ACQUIRE) ==
	       __atomic_load_n(&s->ev_seq, __ATOMIC_ACQUIRE);
}

void i2c_mm_native_bottom_release(void)
{
	lock();
	if (wire->owner == my_slot) {
		wire->owner = -1;
	}
	unlock();
}

void i2c_mm_native_bottom_set_stuck(int stuck)
{
	lock();
	wire->stuck = stuck;
	unlock();
}

void i2c_mm_native_bottom_post(int target_slot, int ev, uint8_t byte)
{
	struct slot *s = &wire->slots[target_slot];

	lock();
	s->ev = ev;
	s->ev_byte = byte;
	s->ev_seq++;
	unlock();
}

int i2c_mm_native_bottom_answered(int target_slot, int *result, uint8_t *byte)
{
	struct slot *s = &wire->slots[target_slot];
	int done;

	if (!seq_equal(s)) {
		return 0; /* cheap look first: polled while the target works */
	}
	lock();
	done = s->ans_seq == s->ev_seq;
	if (done) {
		*result = s->result;
		*byte = s->ans_byte;
	}
	unlock();
	return done;
}

void i2c_mm_native_bottom_cancel(int target_slot)
{
	struct slot *s = &wire->slots[target_slot];

	lock();
	s->ans_seq = s->ev_seq;
	unlock();
}

int i2c_mm_native_bottom_fetch(int *ev, uint8_t *byte)
{
	struct slot *s = &wire->slots[my_slot];
	int pending;

	if (seq_equal(s)) {
		return 0; /* cheap look first: polled all the time */
	}
	lock();
	pending = s->ans_seq != s->ev_seq;
	if (pending) {
		*ev = s->ev;
		*byte = s->ev_byte;
	}
	unlock();
	return pending;
}

void i2c_mm_native_bottom_answer(int result, uint8_t byte)
{
	struct slot *s = &wire->slots[my_slot];

	lock();
	s->result = result;
	s->ans_byte = byte;
	s->ans_seq = s->ev_seq;
	unlock();
}
