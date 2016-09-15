/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include "queue.h"

static void bus1_queue_node_set_timestamp(struct bus1_queue_node *node, u64 ts)
{
	WARN_ON(ts & BUS1_QUEUE_TYPE_MASK);
	node->timestamp_and_type &= BUS1_QUEUE_TYPE_MASK;
	node->timestamp_and_type |= ts;
}

static void bus1_queue_node_no_free(struct kref *ref)
{
	/* no-op kref_put() callback that is used if we hold >1 reference */
	WARN(1, "Queue node freed unexpectedly");
}

static int bus1_queue_node_compare(struct bus1_queue_node *node,
				   u64 timestamp,
				   unsigned long sender)
{
	return bus1_queue_compare(bus1_queue_node_get_timestamp(node),
				  node->sender, timestamp, sender);
}

/**
 * bus1_queue_init() - initialize queue
 * @queue:	queue to initialize
 * @waitq:	wait queue to use for wake-ups
 *
 * This initializes a new queue. The queue memory is considered uninitialized,
 * any previous content is unrecoverable.
 */
void bus1_queue_init(struct bus1_queue *queue, wait_queue_head_t *waitq)
{
	queue->clock = 0;
	rcu_assign_pointer(queue->front, NULL);
	queue->waitq = waitq;
	queue->messages = RB_ROOT;
	mutex_init(&queue->lock);
}

/**
 * bus1_queue_destroy() - destroy queue
 * @queue:	queue to destroy
 *
 * This destroys a queue that was previously initialized via bus1_queue_init().
 * The caller must make sure the queue is empty before calling this.
 *
 * This function is a no-op, and only does safety checks on the queue. It is
 * safe to call this function multiple times on the same queue.
 *
 * The caller must guarantee that the backing memory of @queue is freed in an
 * rcu-delayed manner.
 */
void bus1_queue_destroy(struct bus1_queue *queue)
{
	if (!queue)
		return;

	mutex_destroy(&queue->lock);
	WARN_ON(!RB_EMPTY_ROOT(&queue->messages));
	WARN_ON(rcu_access_pointer(queue->front));
}

/**
 * bus1_queue_flush() - flush message queue
 * @queue:		queue to flush
 * @list:		list to put flushed messages to
 *
 * This flushes all entries from @queue and puts them into @list (no particular
 * order) for the caller to clean up. All the entries returned in @list must be
 * unref'ed by the caller.
 */
void bus1_queue_flush(struct bus1_queue *queue, struct list_head *list)
{
	struct bus1_queue_node *node, *t;

	mutex_lock(&queue->lock);

	/*
	 * A queue contains staging and committed nodes. A committed node is
	 * fully owned by the queue, including a ref-count, but a staging entry
	 * is always still owned by a transaction (with an additional
	 * ref-count). Any state transition is synchronized by the queue lock.
	 *
	 * On flush, we push all committed (i.e., queue-owned) nodes into a
	 * list and transfer them (including a ref-count) to the caller, as if
	 * they dequeued them manually. But any staging node is simply marked
	 * as removed and the queue-refcount is dropped. The owning transaction
	 * will thus notice the removal and will be unable to commit it. Hence,
	 * it will be responsible of the cleanup.
	 * Note that we *know* that our node-refcount cannot be the last, since
	 * otherwise the owning transaction would be unable to commit the node.
	 */
	rbtree_postorder_for_each_entry_safe(node, t, &queue->messages, rb) {
		if (bus1_queue_node_is_staging(node)) {
			RB_CLEAR_NODE(&node->rb);
			kref_put(&node->ref, bus1_queue_node_no_free);
		} else {
			list_add(&node->link, list);
		}
	}

	queue->messages = RB_ROOT;
	rcu_assign_pointer(queue->front, NULL);

	mutex_unlock(&queue->lock);
}

static void bus1_queue_add(struct bus1_queue *queue,
			   struct bus1_queue_node *node,
			   u64 timestamp)
{
	struct rb_node *front, *n, **slot;
	struct bus1_queue_node *iter;
	bool is_leftmost, readable;
	u64 ts;
	int r;

	lockdep_assert_held(&queue->lock);
	ts = bus1_queue_node_get_timestamp(node);
	readable = bus1_queue_is_readable(queue);

	/* provided timestamp must be valid */
	if (WARN_ON(timestamp == 0 || timestamp > queue->clock + 1))
		return;
	/* if unstamped, it must be unlinked, and vice versa */
	if (WARN_ON(!ts == !RB_EMPTY_NODE(&node->rb)))
		return;
	/* if stamped, it must be a valid staging timestamp from earlier */
	if (ts != 0 && WARN_ON(!(ts & 1) || timestamp < ts))
		return;
	/* nothing to do? */
	if (ts == timestamp)
		return;

	/*
	 * On updates, we remove our entry and re-insert it with a higher
	 * timestamp. Hence, _iff_ we were the first entry, we might uncover
	 * some new front entry. Make sure we mark it as front entry then. Note
	 * that we know that our entry must be marked staging, so it cannot be
	 * set as front, yet. If there is a front, it is some other node.
	 */
	front = rcu_dereference_protected(queue->front,
					  lockdep_is_held(&queue->lock));
	if (front) {
		/*
		 * If there already is a front entry, just verify that we will
		 * not order *before* it. We *must not* replace it as front.
		 */
		iter = container_of(front, struct bus1_queue_node, rb);
		WARN_ON(node == iter);
		WARN_ON(timestamp <= bus1_queue_node_get_timestamp(iter));
	} else if (!RB_EMPTY_NODE(&node->rb) && !rb_prev(&node->rb)) {
		/*
		 * We are linked into the queue as staging entry *and* we are
		 * the first entry. Now look at the following entry. If it is
		 * already committed *and* has a lower timestamp than we do, it
		 * will become the new front, so mark it as such!
		 */
		n = rb_next(&node->rb);
		if (n) {
			iter = container_of(n, struct bus1_queue_node, rb);
			if (!bus1_queue_node_is_staging(iter) &&
			    bus1_queue_node_compare(iter, timestamp,
						    node->sender) < 0)
				rcu_assign_pointer(queue->front, n);
		}
	}

	/* must be staging, so it cannot be pointed to by queue->front */
	if (RB_EMPTY_NODE(&node->rb))
		kref_get(&node->ref);
	else
		rb_erase(&node->rb, &queue->messages);

	bus1_queue_node_set_timestamp(node, timestamp);

	/* re-insert into sorted rb-tree with new timestamp */
	slot = &queue->messages.rb_node;
	n = NULL;
	is_leftmost = true;
	while (*slot) {
		n = *slot;
		iter = container_of(n, struct bus1_queue_node, rb);
		r = bus1_queue_node_compare(node,
					    bus1_queue_node_get_timestamp(iter),
					    iter->sender);
		if (r < 0) {
			slot = &n->rb_left;
		} else {
			slot = &n->rb_right;
			is_leftmost = false;
		}
	}

	rb_link_node(&node->rb, n, slot);
	rb_insert_color(&node->rb, &queue->messages);

	if (!(timestamp & 1) && is_leftmost)
		rcu_assign_pointer(queue->front, &node->rb);

	if (!readable && bus1_queue_is_readable(queue))
		wake_up_interruptible(queue->waitq);
}

/**
 * bus1_queue_stage() - stage queue entry with fresh timestamp
 * @queue:		queue to operate on
 * @node:		queue entry to stage
 * @timestamp:		minimum timestamp for @node
 *
 * Link a queue entry with a new timestamp. The staging entry blocks all
 * messages with timestamps synced on this queue in the future, as well as any
 * messages with a timestamp greater than @timestamp. However, it does not block
 * any messages already committed to this queue.
 *
 * The caller must provide an even timestamp and the entry may not already have
 * been committed.
 *
 * The queue owns its own reference to the node, so the caller retains their
 * reference after this call returns.
 *
 * Return: The timestamp used.
 */
u64 bus1_queue_stage(struct bus1_queue *queue,
		     struct bus1_queue_node *node,
		     u64 timestamp)
{
	WARN_ON(!RB_EMPTY_NODE(&node->rb));
	WARN_ON(timestamp & 1);

	mutex_lock(&queue->lock);
	timestamp = bus1_queue_sync(queue, timestamp);
	bus1_queue_add(queue, node, timestamp + 1);
	mutex_unlock(&queue->lock);

	return timestamp;
}

/**
 * bus1_queue_commit_staged() - commit staged queue entry with new timestamp
 * @queue:		queue to operate on
 * @node:		queue entry to commit
 * @timestamp:		new timestamp for @node
 *
 * Update a queue entry according to @timestamp. If the queue entry is already
 * staged on the queue, it is updated and sorted according to @timestamp.
 * Otherwise, nothing is done.
 *
 * The caller must provide an even timestamp and the entry may not already have
 * been committed.
 *
 * Furthermore, the queue clock must be synced with the new timestamp *before*
 * staging an entry. Similarly, the timestamp of an entry can only be
 * increased, never decreased.
 *
 * The queue owns its own reference to the node, so the caller retains their
 * reference after this call returns.
 *
 * Returns: True if the entry was committed, false otherwise.
 */
bool bus1_queue_commit_staged(struct bus1_queue *queue,
			      struct bus1_queue_node *node,
			      u64 timestamp)
{
	bool committed;

	WARN_ON(timestamp & 1);

	mutex_lock(&queue->lock);
	committed = bus1_queue_node_is_queued(node);
	if (committed)
		bus1_queue_add(queue, node, timestamp);
	mutex_unlock(&queue->lock);

	return committed;
}

/**
 * bus1_queue_commit_unstaged() - commit unstaged queue entry with new timestamp
 * @queue:		queue to operate on
 * @node:		queue entry to commit
 *
 * Directly commit an unstaged queue entry to the destination queue. If the
 * queue entry is already, nothing is done.
 *
 * The destination queue is ticked and the resulting timestamp is used to commit
 * the queue entry.
 *
 * The queue owns its own reference to the node, so the caller retains their
 * reference after this call returns.
 */
void bus1_queue_commit_unstaged(struct bus1_queue *queue,
				struct bus1_queue_node *node)
{
	mutex_lock(&queue->lock);
	if (!bus1_queue_node_is_queued(node))
		bus1_queue_add(queue, node, bus1_queue_tick(queue));
	mutex_unlock(&queue->lock);
}

/**
 * bus1_queue_remove() - remove entry from queue
 * @queue:		queue to operate on
 * @node:		queue entry to remove
 *
 * This unlinks @node and fully removes it from the queue @queue. You must
 * never reuse that node again, once removed.
 *
 * If @node was still in staging, this call might uncover a new front entry and
 * as such turn the queue readable. Hence, the caller *must* handle its return
 * value.
 *
 * The queue will drop its reference to the node, but rely on the caller to
 * have a reference on their own (which is implied by passing @node as argument
 * to this function). The caller retains their reference after this call
 * returns.
 *
 * Returns: True if removed by this call, false if already removed.
 */
bool bus1_queue_remove(struct bus1_queue *queue, struct bus1_queue_node *node)
{
	struct bus1_queue_node *iter;
	struct rb_node *front, *n;
	bool readable, queued = false;

	if (!node)
		return false;

	mutex_lock(&queue->lock);

	if (RB_EMPTY_NODE(&node->rb))
		goto exit;

	readable = bus1_queue_is_readable(queue);
	front = rcu_dereference_protected(queue->front,
					  lockdep_is_held(&queue->lock));

	if (!rb_prev(&node->rb)) {
		/*
		 * We are the first entry in the queue. Regardless whether we
		 * are marked as front or not, our removal might uncover a new
		 * front. Hence, always look at the next following entry and
		 * see whether it is fully committed. If it is, mark it as
		 * front, but otherwise reset the front to NULL.
		 */
		n = rb_next(&node->rb);
		if (n) {
			iter = container_of(n, struct bus1_queue_node, rb);
			if (bus1_queue_node_is_staging(iter))
				n = NULL;
		}
		rcu_assign_pointer(queue->front, n);
	}

	rb_erase(&node->rb, &queue->messages);
	RB_CLEAR_NODE(&node->rb);
	kref_put(&node->ref, bus1_queue_node_no_free);
	queued = true;

	if (!readable && bus1_queue_is_readable(queue))
		wake_up_interruptible(queue->waitq);

exit:
	mutex_unlock(&queue->lock);
	return queued;
}

/**
 * bus1_queue_peek_locked() - peek first available entry
 * @queue:	queue to operate on
 * @continuep:	output variable to store continue-state of peeked node
 *
 * This returns a reference to the first available entry in the given queue, or
 * NULL if there is none. The queue stays unmodified and the returned entry
 * remains on the queue.
 *
 * This only returns entries that are ready to be dequeued. Entries that are
 * still in staging mode will not be considered.
 *
 * If a node is returned, the continue-state of that node is stored in
 * @continuep. The continue-state describes whether there are more nodes queued
 * that are part of the same transaction.
 *
 * The caller must hold @queue.lock.
 *
 * Return: Reference to first available entry, NULL if none available.
 */
struct bus1_queue_node *bus1_queue_peek_locked(struct bus1_queue *queue,
					       bool *continuep)
{
	struct bus1_queue_node *node, *next;
	struct rb_node *n;

	lockdep_assert_held(&queue->lock);

	n = rcu_dereference_protected(queue->front,
				      lockdep_is_held(&queue->lock));
	if (!n)
		return NULL;

	node = container_of(n, struct bus1_queue_node, rb);
	kref_get(&node->ref);

	n = rb_next(n);
	if (n) {
		next = container_of(n, struct bus1_queue_node, rb);
		*continuep = !bus1_queue_node_compare(node,
				bus1_queue_node_get_timestamp(next),
				next->sender);
	} else {
		*continuep = false;
	}

	return node;
}