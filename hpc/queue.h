/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2013 - 2026                        Daniel Kubec <niel@rtfm.cz>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"),to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Intrusive hashable queue - a doubly linked list with a single head pointer
 *
 * Modelled on the Linux kernel hlist: nodes carry a forward @next pointer and
 * a @prev pointer-to-pointer that addresses the slot referring back to them
 * (either the head's @first or a predecessor's @next). This lets a node be
 * removed in O(1) without walking the queue and without a distinct head node,
 * so an array of heads costs one pointer per bucket - ideal for hash tables.
 
 **/

#ifndef __GENERIC_QUEUE_H__
#define __GENERIC_QUEUE_H__

#include <hpc/compiler.h>
#include <stdbool.h>

__BEGIN_DECLS

struct qnode { struct qnode *next, **prev; };
struct queue { struct qnode *first; };

#define QUEUE_INIT           { .first = NULL }
#define DECLARE_QUEUE(name)  struct queue name
#define DEFINE_QUEUE(name)   struct queue name = QUEUE_INIT

#ifndef init_qnode
#define init_qnode           (struct qnode){ .next = NULL, .prev = NULL }
#endif
#define init_queue           (struct queue){ .first = NULL }

#define queue_entry(ptr, type, member) container_of(ptr, type, member)
#define queue_entry_safe(ptr, type, member) container_of_safe(ptr, type, member)

static inline void
qnode_init(struct qnode *qnode)
{
	qnode->next = NULL;
	qnode->prev = NULL;
}

static inline void
queue_init(struct queue *queue)
{
	queue->first = NULL;
}

static inline bool
qnode_unhashed(const struct qnode *qnode)
{
	return !qnode->prev;
}

static inline bool
qnode_hashed(const struct qnode *qnode)
{
	return !!qnode->prev;
}

static inline int
queue_empty(const struct queue *queue)
{
	return !queue->first;
}

static inline struct qnode *
queue_first(const struct queue *queue)
{
	return queue->first;
}

static inline void
queue_add_head(struct queue *queue, struct qnode *qnode)
{
	struct qnode *first = queue->first;
	qnode->next = first;
	if (first)
		first->prev = &qnode->next;
	queue->first = qnode;
	qnode->prev = &queue->first;
}

static inline void
queue_add(struct queue *queue, struct qnode *qnode)
{
	queue_add_head(queue, qnode);
}

/* insert @qnode before @next, which must already be in a queue */
static inline void
queue_add_before(struct qnode *qnode, struct qnode *next)
{
	qnode->prev = next->prev;
	qnode->next = next;
	next->prev = &qnode->next;
	*(qnode->prev) = qnode;
}

/* insert @qnode after @prev, which must already be in a queue */
static inline void
queue_add_behind(struct qnode *qnode, struct qnode *prev)
{
	qnode->next = prev->next;
	prev->next = qnode;
	qnode->prev = &prev->next;
	if (qnode->next)
		qnode->next->prev = &qnode->next;
}

static inline void
__queue_del(struct qnode *qnode)
{
	struct qnode *next = qnode->next;
	struct qnode **prev = qnode->prev;
	*prev = next;
	if (next)
		next->prev = prev;
}

static inline void
queue_del(struct qnode *qnode)
{
	__queue_del(qnode);
}

static inline void
queue_del_init(struct qnode *qnode)
{
	if (qnode_hashed(qnode)) {
		__queue_del(qnode);
		qnode_init(qnode);
	}
}

/* move every node of @from onto the (assumed empty) head @to */
static inline void
queue_move(struct queue *to, struct queue *from)
{
	to->first = from->first;
	if (to->first)
		to->first->prev = &to->first;
	from->first = NULL;
}

/* ---- RCU variants ------------------------------------------------------- *
 * Gated on CONFIG_RCU (which depends on CONFIG_THREADS). Writers still
 * serialise against each other; readers run lockless under an rcu read-side
 * section they open themselves. del_rcu keeps @next intact so a reader parked on
 * the victim can still advance; the node must be freed only after a grace period
 * - synchronize_rcu() or call_rcu().
 *
 * Publication is liburcu's rcu_assign_pointer() (a store-release), traversal its
 * rcu_dereference() (a dependency-ordered load), in the flavour this build
 * selected - so the ordering is the one the grace periods were built for. See
 * <hpc/rcu.h>.
 */

#ifdef CONFIG_RCU

#include <hpc/rcu.h>

static inline void
queue_add_head_rcu(struct queue *queue, struct qnode *qnode)
{
	struct qnode *first = queue->first;
	qnode->next = first;
	qnode->prev = &queue->first;
	rcu_assign_pointer(queue->first, qnode);
	if (first)
		first->prev = &qnode->next;
}

static inline void
queue_add_before_rcu(struct qnode *qnode, struct qnode *next)
{
	qnode->prev = next->prev;
	qnode->next = next;
	rcu_assign_pointer(*(qnode->prev), qnode);
	next->prev = &qnode->next;
}

static inline void
queue_add_behind_rcu(struct qnode *qnode, struct qnode *prev)
{
	qnode->next = prev->next;
	qnode->prev = &prev->next;
	rcu_assign_pointer(prev->next, qnode);
	if (qnode->next)
		qnode->next->prev = &qnode->next;
}

static inline void
queue_del_rcu(struct qnode *qnode)
{
	__queue_del(qnode);
	qnode->prev = NULL;
}

#endif/*CONFIG_RCU*/

/**
 * queue_walk - iterate over a queue node by node
 *
 * @self:       the queue.
 * @it:         struct qnode * iterator
 */

#define queue_walk(self, it) \
	for ((it) = (self)->first; (it); (it) = (it)->next)

/**
 * queue_walk_delsafe - iterate with safety against removal of @it
 *
 * @self:       the queue.
 * @it:         struct qnode * iterator
 * @tmp:        struct qnode * scratch
 */

#define queue_walk_delsafe(self, it, tmp) \
	for ((it) = (self)->first; (it) && ({ (tmp) = (it)->next; 1; }); \
	     (it) = (tmp))

/**
 * queue_for_each - iterate over a queue, resolving the enclosing struct
 *
 * @self:       the queue.
 * @it:         type * iterator
 * @type:       the enclosing structure type
 * @member:     the name of the qnode within @type
 */

#define queue_for_each(self, it, type, member) \
	for (type *(it) = queue_entry_safe((self)->first, type, member); (it); \
	     (it) = queue_entry_safe((it)->member.next, type, member))

/**
 * queue_for_each_delsafe - typed iteration, safe against removal of @it
 *
 * @self:       the queue.
 * @it:         type * iterator
 * @type:       the enclosing structure type
 * @member:     the name of the qnode within @type
 */

#define queue_for_each_delsafe(self, it, type, member) \
	for (type *__it, *(it) = queue_entry_safe((self)->first, type, member); \
	     (it) && ({ __it = queue_entry_safe((it)->member.next, type, member); \
	                1; }); \
	     (it) = __it)

#ifdef CONFIG_RCU

/**
 * queue_walk_rcu - lockless iteration over a queue node by node
 *
 * @self:       the queue.
 * @it:         struct qnode * iterator
 *
 * The caller owns the read-side section: rcu_read_lock() before, and
 * rcu_read_unlock() once it is done with the nodes it saw.
 */

#define queue_walk_rcu(self, it) \
	for ((it) = rcu_dereference((self)->first); (it); \
	     (it) = rcu_dereference((it)->next))

/**
 * queue_for_each_rcu - lockless typed iteration over a queue
 *
 * @self:       the queue.
 * @it:         type * iterator
 * @type:       the enclosing structure type
 * @member:     the name of the qnode within @type
 */

#define queue_for_each_rcu(self, it, type, member) \
	for (type *(it) = \
	         queue_entry_safe(rcu_dereference((self)->first), type, member); \
	     (it); \
	     (it) = queue_entry_safe(rcu_dereference((it)->member.next), \
	                             type, member))

#endif/*CONFIG_RCU*/

__END_DECLS

#endif/*__GENERIC_QUEUE_H__*/
