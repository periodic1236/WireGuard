/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Jonathan Neuschäfer
 */

/*
 * This is an implementation of a Multi-Producer/Multi-Consumer (MPMC) queue,
 * strongly inspired by ConcurrencyKit[1], and Linux's own ptr_ring.h.
 *
 *
 *              +-----------------------------------------------+
 *        index | 0| 1| 2| 3| 4| 5| 6| 7| 8| 9|10|11|12|13|14|15|
 *        state |--|--|--|**|**|**|**|**|**|**|++|++|++|--|--|--|
 *              +-----------------------------------------------+
 *                        ^                    ^        ^
 *                        consumer head        |        producer head
 *                                             producer tail
 * Possible states:
 *
 *  -- : unoccupied
 *  ++ : being written
 *  ** : occupied
 *
 * Differences between ptr_ring.h and this implementation:
 * - An additional producer tail pointer, which allows multiple enqueue
 *   operations to be in progress at the same time.
 * - No consumer tail pointer, for simplicity (although I expect it can be
 *   added later)
 * - Most importantly: No spinlocks.
 * - The head/tail pointers (or rather: indices) are stored untrimmed, i.e.
 *   without the bit mask (size - 1) applied, because that's how ConcurrencyKit
 *   does it.
 *
 * [1]: https://github.com/concurrencykit/ck/blob/master/include/ck_ring.h
 */

struct mpmc_ptr_ring {
	/* Read-mostly data */
	void **queue;
	size_t size;

	/* consumer_head: updated in dequeue; read in enqueue */
	atomic_long_t consumer_head;

	/* producer_head: read and updated in enqueue */
	atomic_long_t producer_head;

	/* producer_tail: updated in enqueue, read in dequeue */
	atomic_long_t producer_tail;
};

static inline bool mpmc_ptr_ring_empty(struct mpmc_ptr_ring *r)
{
	size_t ptail, chead;

	mb(); /* TODO: think about barriers */

	ptail = atomic_long_read(&r->producer_tail);
	chead = atomic_long_read(&r->consumer_head);

	mb(); /* TODO: think about barriers */

	return chead == ptail;
}

static inline int mpmc_ptr_ring_produce(struct mpmc_ptr_ring *r, void *ptr)
{
	size_t p, c;
	size_t mask = r->size - 1;

	p = atomic_long_read(&r->producer_head);

	for (;;) {
		rmb();	 /* TODO */
		c = atomic_long_read(&r->consumer_head);
		mb();

		if ((p - c) < mask) { /* fast path */
			if (atomic_long_cmpxchg(&r->producer_head, p, p + 1) == p)
				break;
		} else {
			size_t new_p;

			mb();
			new_p = atomic_long_read(&r->producer_head);
			mb();

			if (new_p == p)
				return -ENOSPC;

			p = new_p;
		}
	}

	mb(); /* barriers? */
	WRITE_ONCE(r->queue[p & mask], ptr);
	mb(); /* barriers? */

	/* Wait until it's our term to update the producer tail pointer */
	while(atomic_long_read(&r->producer_tail) != p)
		cpu_relax();

	smp_mb__before_atomic();
	atomic_long_set(&r->producer_tail, p + 1);
	smp_mb__after_atomic();

	return 0;
}

static inline void *mpmc_ptr_ring_consume(struct mpmc_ptr_ring *r)
{
	size_t c, p, old_c;
	void *element;
	size_t mask = r->size - 1;

	for (;;) {
		mb(); // TODO: check
		c = atomic_long_read(&r->consumer_head);
		mb(); // TODO: check
		p = atomic_long_read(&r->producer_tail);
		mb(); // TODO: check

		/* Is the ring empty? */
		if (p == c)
			return NULL;

		element = READ_ONCE(r->queue[c & mask]);

		mb(); // TODO: check

		old_c = atomic_long_cmpxchg(&r->consumer_head, c, c + 1);
		if (old_c == c)
			break;
	}
	mb(); // TODO: check

	return element;
}

static inline int mpmc_ptr_ring_init(struct mpmc_ptr_ring *r, int size, gfp_t gfp)
{
	if (WARN_ONCE(!is_power_of_2(size), "size must be a power of two"))
		return -EINVAL;

	r->size = size;
	atomic_long_set(&r->consumer_head, 0);
	atomic_long_set(&r->producer_head, 0);
	atomic_long_set(&r->producer_tail, 0);

	r->queue = kcalloc(size, sizeof(r->queue[0]), gfp);
	if (!r->queue)
		return -ENOMEM;

	mb(); /* TODO: check */

	return 0;
}

static inline void mpmc_ptr_ring_cleanup(struct mpmc_ptr_ring *r, void (*destroy)(void *))
{
	void *ptr;

	if (destroy)
		while ((ptr = mpmc_ptr_ring_consume(r)))
			destroy(ptr);
	kfree(r->queue);
}

/**
 * __mpmc_ptr_ring_peek - Read the first element in an MPMC ring buffer
 *
 * @r: The ring buffer
 *
 * Note that this function should only be called in single-consumer situations.
 */
static inline void *__mpmc_ptr_ring_peek(struct mpmc_ptr_ring *r)
{
	size_t c, p;
	size_t mask = r->size - 1;
	void *element;

	mb(); // TODO: check
	c = atomic_long_read(&r->consumer_head);
	mb(); // TODO: check
	p = atomic_long_read(&r->producer_tail);
	mb(); // TODO: check

	//pr_info("%s %px %u, at %zu/%zu\n", __func__, r, current->pid, c, p);

	if (c == p)
		return NULL;

	mb(); // TODO: check
	element = READ_ONCE(r->queue[c & mask]);
	mb(); // TODO: check

	return element;
}

/**
 * __mpmc_ptr_ring_discard_one - Discard the first element in an MPMC ring buffer
 *
 * @r: The ring buffer
 *
 * Note that this function should only be called in single-consumer situations.
 */
static inline void __mpmc_ptr_ring_discard_one(struct mpmc_ptr_ring *r)
{
	mb(); // TODO: check
	atomic_long_inc(&r->consumer_head);
	mb(); // TODO: check
}