/*
 * Intrusive red-black tree - a self balancing binary search tree
 *
 * A house-style, intrusive counterpart to the macro tree in <hpc/rb.h>. Nodes
 * carry the link fields (struct rbnode) and are embedded directly in the caller
 * payload, exactly as struct qnode is in <hpc/queue.h>; there is no separate
 * allocation for the tree and container_of() recovers the enclosing object.
 *
 * The tree is left to the caller for ordering: descend with your own comparison,
 * splice the new node with rbtree_link_node() and rebalance with
 * rbtree_insert_color(). This mirrors the Linux kernel rbtree split and keeps
 * the container free of any key/type knowledge - the same design point as the
 * policy-free primitives in <hpc/queue.h>.
 *
 *	- every search path from the root to a leaf holds the same number of
 *	  black nodes,
 *	- a red node never has a red child,
 *	- the root is black.
 *
 * Every operation is bounded O(lg n); the height is at most 2lg(n + 1).
 *
 * Naming follows the house convention (queue -> qnode/queue, list -> node/list)
 * with the tree spelling: node is struct rbnode, head is struct rbtree, macros
 * are lower case in the style of the kernel.
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013 - 2019                        Daniel Kubec <niel@rtfm.cz>
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
 **/

#ifndef __GENERIC_RBTREE_H__
#define __GENERIC_RBTREE_H__

#include <hpc/compiler.h>
#include <hpc/rbtree/measure.h>
#include <stddef.h>
#include <stdbool.h>

__BEGIN_DECLS

enum { RBTREE_RED = 0, RBTREE_BLACK = 1 };

/*
 * A detached node marks itself with @parent pointing at itself: the tree root
 * legitimately has a NULL parent, so NULL cannot mean "not linked". This is the
 * same sentinel the kernel rbtree uses and the tree's analogue of qnode_unhashed.
 */
struct rbnode { struct rbnode *left, *right, *parent; int color; };

/*
 * Event measurement. @measure points at a caller-owned struct rbtree_measure
 * (see <hpc/rbtree/measure.h>); attach one with rbtree_measure_attach() to
 * count inserts, erases and rotations, keep an entries gauge and derive the
 * rebalance ratio. Trees may share one struct to aggregate, hold their own, or
 * leave it NULL to not measure - a NULL target is one predicted-not-taken
 * branch per event. The member exists only under CONFIG_MEASURE; without it the
 * field, the branches and the counting all vanish.
 */
struct rbtree { struct rbnode *root; measure_member(rbtree) };

#define RBTREE_INIT           { .root = NULL }
#define DECLARE_RBTREE(name)  struct rbtree name
#define DEFINE_RBTREE(name)   struct rbtree name = RBTREE_INIT

#define init_rbtree           (struct rbtree){ .root = NULL }

#define rbtree_entry(ptr, type, member) container_of(ptr, type, member)
#define rbtree_entry_safe(ptr, type, member) container_of_safe(ptr, type, member)

/*
 * Attach (or clear, with NULL) the measurement struct events are counted
 * through. No-op without CONFIG_MEASURE. rbtree_init() clears it, so attach
 * after init - matching the slab, where the setup done before a measure is
 * attached is deliberately not counted.
 */
#ifdef CONFIG_MEASURE
#define rbtree_measure_attach(_tree, _m) \
	do { (_tree)->measure = (_m); } while (0)
#else
#define rbtree_measure_attach(_tree, _m) ((void)0)
#endif

static inline void
rbtree_init(struct rbtree *tree)
{
	tree->root = NULL;
	rbtree_measure_attach(tree, NULL);
}

static inline void
rbnode_init(struct rbnode *node)
{
	node->left = node->right = NULL;
	node->parent = node;                 /* self-parent == not linked */
	node->color = RBTREE_RED;
}

static inline bool
rbnode_linked(const struct rbnode *node)
{
	return node->parent != node;
}

static inline bool
rbnode_unlinked(const struct rbnode *node)
{
	return node->parent == node;
}

static inline bool
rbtree_empty(const struct rbtree *tree)
{
	return !tree->root;
}

/* ---- rotations ----------------------------------------------------------- *
 * A left rotation about @x lifts its right child into @x's slot; the right
 * rotation is the mirror image. Both keep the in-order sequence intact.
 */

static inline void
__rbtree_rotate_left(struct rbtree *tree, struct rbnode *x)
{
	struct rbnode *y = x->right;

	measure_inc(tree->measure, rotate);
	x->right = y->left;
	if (y->left)
		y->left->parent = x;
	y->parent = x->parent;
	if (!x->parent)
		tree->root = y;
	else if (x == x->parent->left)
		x->parent->left = y;
	else
		x->parent->right = y;
	y->left = x;
	x->parent = y;
}

static inline void
__rbtree_rotate_right(struct rbtree *tree, struct rbnode *x)
{
	struct rbnode *y = x->left;

	measure_inc(tree->measure, rotate);
	x->left = y->right;
	if (y->right)
		y->right->parent = x;
	y->parent = x->parent;
	if (!x->parent)
		tree->root = y;
	else if (x == x->parent->right)
		x->parent->right = y;
	else
		x->parent->left = y;
	y->right = x;
	x->parent = y;
}

/**
 * rbtree_link_node - splice a fresh node into a found slot
 *
 * @node:       the node to insert, coloured red
 * @parent:     the node that will become @node's parent (NULL for the root)
 * @link:       address of the child slot to fill (&parent->left/right, or
 *              &tree->root when the tree is empty)
 *
 * The caller has already walked the tree and knows where @node belongs. After
 * linking, call rbtree_insert_color() to restore the red-black invariants.
 */
static inline void
rbtree_link_node(struct rbnode *node, struct rbnode *parent,
                 struct rbnode **link)
{
	node->parent = parent;
	node->left = node->right = NULL;
	node->color = RBTREE_RED;
	*link = node;
}

/**
 * rbtree_insert_color - rebalance after a red node was linked in
 *
 * @tree:       the tree.
 * @node:       the freshly linked red node.
 */
static inline void
rbtree_insert_color(struct rbtree *tree, struct rbnode *node)
{
	struct rbnode *parent, *gparent;

	measure_inc(tree->measure, insert);
	measure_inc(tree->measure, entries);           /* gauge up */

	while ((parent = node->parent) && parent->color == RBTREE_RED) {
		gparent = parent->parent;      /* a red parent is never the root */
		if (parent == gparent->left) {
			struct rbnode *uncle = gparent->right;
			if (uncle && uncle->color == RBTREE_RED) {
				parent->color = RBTREE_BLACK;
				uncle->color = RBTREE_BLACK;
				gparent->color = RBTREE_RED;
				node = gparent;
				continue;
			}
			if (node == parent->right) {
				node = parent;
				__rbtree_rotate_left(tree, node);
				parent = node->parent;
			}
			parent->color = RBTREE_BLACK;
			gparent->color = RBTREE_RED;
			__rbtree_rotate_right(tree, gparent);
		} else {
			struct rbnode *uncle = gparent->left;
			if (uncle && uncle->color == RBTREE_RED) {
				parent->color = RBTREE_BLACK;
				uncle->color = RBTREE_BLACK;
				gparent->color = RBTREE_RED;
				node = gparent;
				continue;
			}
			if (node == parent->left) {
				node = parent;
				__rbtree_rotate_right(tree, node);
				parent = node->parent;
			}
			parent->color = RBTREE_BLACK;
			gparent->color = RBTREE_RED;
			__rbtree_rotate_left(tree, gparent);
		}
	}
	tree->root->color = RBTREE_BLACK;
}

static inline void
__rbtree_erase_color(struct rbtree *tree, struct rbnode *node,
                     struct rbnode *parent)
{
	struct rbnode *sib;

	while ((!node || node->color == RBTREE_BLACK) && node != tree->root) {
		if (parent->left == node) {
			sib = parent->right;
			if (sib->color == RBTREE_RED) {
				sib->color = RBTREE_BLACK;
				parent->color = RBTREE_RED;
				__rbtree_rotate_left(tree, parent);
				sib = parent->right;
			}
			if ((!sib->left || sib->left->color == RBTREE_BLACK) &&
			    (!sib->right || sib->right->color == RBTREE_BLACK)) {
				sib->color = RBTREE_RED;
				node = parent;
				parent = node->parent;
			} else {
				if (!sib->right ||
				    sib->right->color == RBTREE_BLACK) {
					if (sib->left)
						sib->left->color = RBTREE_BLACK;
					sib->color = RBTREE_RED;
					__rbtree_rotate_right(tree, sib);
					sib = parent->right;
				}
				sib->color = parent->color;
				parent->color = RBTREE_BLACK;
				if (sib->right)
					sib->right->color = RBTREE_BLACK;
				__rbtree_rotate_left(tree, parent);
				node = tree->root;
				break;
			}
		} else {
			sib = parent->left;
			if (sib->color == RBTREE_RED) {
				sib->color = RBTREE_BLACK;
				parent->color = RBTREE_RED;
				__rbtree_rotate_right(tree, parent);
				sib = parent->left;
			}
			if ((!sib->left || sib->left->color == RBTREE_BLACK) &&
			    (!sib->right || sib->right->color == RBTREE_BLACK)) {
				sib->color = RBTREE_RED;
				node = parent;
				parent = node->parent;
			} else {
				if (!sib->left ||
				    sib->left->color == RBTREE_BLACK) {
					if (sib->right)
						sib->right->color = RBTREE_BLACK;
					sib->color = RBTREE_RED;
					__rbtree_rotate_left(tree, sib);
					sib = parent->left;
				}
				sib->color = parent->color;
				parent->color = RBTREE_BLACK;
				if (sib->left)
					sib->left->color = RBTREE_BLACK;
				__rbtree_rotate_right(tree, parent);
				node = tree->root;
				break;
			}
		}
	}
	if (node)
		node->color = RBTREE_BLACK;
}

/**
 * rbtree_erase - remove @node from @tree
 *
 * @tree:       the tree.
 * @node:       a node currently linked in @tree.
 *
 * O(lg n). Like queue_del, this leaves @node's link fields dangling; run
 * rbnode_init() on it before reuse (or use rbtree_erase_init).
 */
static inline void
rbtree_erase(struct rbtree *tree, struct rbnode *node)
{
	struct rbnode *child, *parent;
	int color;

	measure_inc(tree->measure, erase);
	measure_dec(tree->measure, entries);           /* gauge down */

	if (!node->left) {
		child = node->right;
	} else if (!node->right) {
		child = node->left;
	} else {
		/* two children: splice in the in-order successor */
		struct rbnode *old = node, *left;

		node = node->right;
		while ((left = node->left) != NULL)
			node = left;

		if (old->parent) {
			if (old->parent->left == old)
				old->parent->left = node;
			else
				old->parent->right = node;
		} else
			tree->root = node;

		child = node->right;
		parent = node->parent;
		color = node->color;

		if (parent == old) {
			parent = node;
		} else {
			if (child)
				child->parent = parent;
			parent->left = child;

			node->right = old->right;
			old->right->parent = node;
		}

		node->parent = old->parent;
		node->color = old->color;
		node->left = old->left;
		old->left->parent = node;

		goto color;
	}

	parent = node->parent;
	color = node->color;

	if (child)
		child->parent = parent;
	if (parent) {
		if (parent->left == node)
			parent->left = child;
		else
			parent->right = child;
	} else
		tree->root = child;

color:
	if (color == RBTREE_BLACK)
		__rbtree_erase_color(tree, child, parent);
}

static inline void
rbtree_erase_init(struct rbtree *tree, struct rbnode *node)
{
	rbtree_erase(tree, node);
	rbnode_init(node);
}

/**
 * rbtree_replace - swap @victim for @new, which must sort to the same slot
 *
 * @tree:       the tree.
 * @victim:     a node currently linked in @tree.
 * @new:        the replacement node (its link fields are overwritten).
 */
static inline void
rbtree_replace(struct rbtree *tree, struct rbnode *victim, struct rbnode *new)
{
	struct rbnode *parent = victim->parent;

	*new = *victim;
	if (victim->left)
		victim->left->parent = new;
	if (victim->right)
		victim->right->parent = new;
	if (parent) {
		if (parent->left == victim)
			parent->left = new;
		else
			parent->right = new;
	} else
		tree->root = new;
}

/* ---- ordered traversal --------------------------------------------------- */

static inline struct rbnode *
rbtree_first(const struct rbtree *tree)
{
	struct rbnode *n = tree->root;

	if (!n)
		return NULL;
	while (n->left)
		n = n->left;
	return n;
}

static inline struct rbnode *
rbtree_last(const struct rbtree *tree)
{
	struct rbnode *n = tree->root;

	if (!n)
		return NULL;
	while (n->right)
		n = n->right;
	return n;
}

static inline struct rbnode *
rbtree_next(const struct rbnode *node)
{
	struct rbnode *n = (struct rbnode *)node;
	struct rbnode *parent;

	if (n->right) {
		n = n->right;
		while (n->left)
			n = n->left;
		return n;
	}
	while ((parent = n->parent) && n == parent->right)
		n = parent;
	return parent;
}

static inline struct rbnode *
rbtree_prev(const struct rbnode *node)
{
	struct rbnode *n = (struct rbnode *)node;
	struct rbnode *parent;

	if (n->left) {
		n = n->left;
		while (n->right)
			n = n->right;
		return n;
	}
	while ((parent = n->parent) && n == parent->left)
		n = parent;
	return parent;
}

/**
 * rbtree_walk - iterate a tree in order, node by node
 *
 * @self:       the tree.
 * @it:         struct rbnode * iterator
 */

#define rbtree_walk(self, it) \
	for ((it) = rbtree_first(self); (it); (it) = rbtree_next(it))

/**
 * rbtree_walk_delsafe - in-order iteration safe against removal of @it
 *
 * @self:       the tree.
 * @it:         struct rbnode * iterator
 * @tmp:        struct rbnode * scratch
 */

#define rbtree_walk_delsafe(self, it, tmp) \
	for ((it) = rbtree_first(self); \
	     (it) && ({ (tmp) = rbtree_next(it); 1; }); (it) = (tmp))

/**
 * rbtree_walk_reverse - reverse in-order iteration, node by node
 *
 * @self:       the tree.
 * @it:         struct rbnode * iterator
 */

#define rbtree_walk_reverse(self, it) \
	for ((it) = rbtree_last(self); (it); (it) = rbtree_prev(it))

/**
 * rbtree_for_each - iterate in order, resolving the enclosing struct
 *
 * @self:       the tree.
 * @it:         type * iterator
 * @type:       the enclosing structure type
 * @member:     the name of the rbnode within @type
 */

#define rbtree_for_each(self, it, type, member) \
	for (type *(it) = rbtree_entry_safe(rbtree_first(self), type, member); \
	     (it); \
	     (it) = rbtree_entry_safe(rbtree_next(&(it)->member), type, member))

/**
 * rbtree_for_each_delsafe - typed in-order iteration, safe against removal
 *
 * @self:       the tree.
 * @it:         type * iterator
 * @type:       the enclosing structure type
 * @member:     the name of the rbnode within @type
 */

#define rbtree_for_each_delsafe(self, it, type, member) \
	for (type *__it, \
	     *(it) = rbtree_entry_safe(rbtree_first(self), type, member); \
	     (it) && ({ __it = rbtree_entry_safe( \
	         rbtree_next(&(it)->member), type, member); 1; }); \
	     (it) = __it)

/**
 * rbtree_for_each_reverse - reverse in-order typed iteration
 *
 * @self:       the tree.
 * @it:         type * iterator
 * @type:       the enclosing structure type
 * @member:     the name of the rbnode within @type
 */

#define rbtree_for_each_reverse(self, it, type, member) \
	for (type *(it) = rbtree_entry_safe(rbtree_last(self), type, member); \
	     (it); \
	     (it) = rbtree_entry_safe(rbtree_prev(&(it)->member), type, member))

/* ---- RCU variants -------------------------------------------------------- *
 * Gated on CONFIG_RCU (which depends on CONFIG_THREADS). Writers still
 * serialise against each other; readers run lockless inside an rcu read-side
 * section they open themselves. Publishing a node uses liburcu's
 * rcu_assign_pointer() (a store-release), traversal its rcu_dereference()
 * (dependency-ordered loads), in the flavour this build selected - see
 * <hpc/rcu.h>. A removed node may be freed only after a grace period,
 * synchronize_rcu() or call_rcu().
 *
 * As with the Linux kernel rbtree, only linking and replacement are published
 * atomically for readers. rbtree_insert_color() and rbtree_erase() rebalance by
 * mutating several pointers of existing nodes, so a reader racing a rebalance
 * may transiently miss a node that is in the tree; it never dereferences freed
 * memory or leaves the read-side section corrupt. Serialise writers and defer
 * freeing to a grace period.
 */

#ifdef CONFIG_RCU

#include <hpc/rcu.h>

/**
 * rbtree_link_node_rcu - splice a fresh node in, publishing it to rcu readers
 *
 * @node:       the node to insert, coloured red
 * @parent:     the node that will become @node's parent (NULL for the root)
 * @link:       address of the child slot to fill
 *
 * The node's own fields are initialised before the slot is published with a
 * store-release, so a reader observing @node through @link also observes a fully
 * formed node. Follow with rbtree_insert_color() to rebalance.
 */
static inline void
rbtree_link_node_rcu(struct rbnode *node, struct rbnode *parent,
                     struct rbnode **link)
{
	node->parent = parent;
	node->left = node->right = NULL;
	node->color = RBTREE_RED;
	rcu_assign_pointer(*link, node);
}

/**
 * rbtree_replace_rcu - swap @victim for @new, published to rcu readers
 *
 * @tree:       the tree.
 * @victim:     a node currently linked in @tree.
 * @new:        the replacement node, sorting to the same slot.
 *
 * @new is filled in completely before it is spliced in with a store-release, so
 * a reader sees either @victim or a fully formed @new, never a torn node. The
 * child links still point at @victim's subtrees for a parked reader; @victim may
 * be freed after a grace period.
 */
static inline void
rbtree_replace_rcu(struct rbtree *tree, struct rbnode *victim,
                   struct rbnode *new)
{
	struct rbnode *parent = victim->parent;

	*new = *victim;
	if (victim->left)
		victim->left->parent = new;
	if (victim->right)
		victim->right->parent = new;
	if (parent) {
		if (parent->left == victim)
			rcu_assign_pointer(parent->left, new);
		else
			rcu_assign_pointer(parent->right, new);
	} else
		rcu_assign_pointer(tree->root, new);
}

static inline struct rbnode *
rbtree_first_rcu(const struct rbtree *tree)
{
	struct rbnode *n = rcu_dereference(tree->root), *l;

	if (!n)
		return NULL;
	while ((l = rcu_dereference(n->left)))
		n = l;
	return n;
}

static inline struct rbnode *
rbtree_next_rcu(const struct rbnode *node)
{
	struct rbnode *n = (struct rbnode *)node, *r, *l, *parent;

	if ((r = rcu_dereference(n->right))) {
		n = r;
		while ((l = rcu_dereference(n->left)))
			n = l;
		return n;
	}
	while ((parent = rcu_dereference(n->parent)) &&
	       n == rcu_dereference(parent->right))
		n = parent;
	return parent;
}

/**
 * rbtree_walk_rcu - lockless in-order iteration, node by node
 *
 * @self:       the tree.
 * @it:         struct rbnode * iterator
 *
 * The caller owns the read-side section: rcu_read_lock() before, and
 * rcu_read_unlock() once it is done with the nodes it saw.
 */

#define rbtree_walk_rcu(self, it) \
	for ((it) = rbtree_first_rcu(self); (it); (it) = rbtree_next_rcu(it))

/**
 * rbtree_for_each_rcu - lockless in-order typed iteration
 *
 * @self:       the tree.
 * @it:         type * iterator
 * @type:       the enclosing structure type
 * @member:     the name of the rbnode within @type
 */

#define rbtree_for_each_rcu(self, it, type, member) \
	for (type *(it) = \
	         rbtree_entry_safe(rbtree_first_rcu(self), type, member); \
	     (it); \
	     (it) = rbtree_entry_safe(rbtree_next_rcu(&(it)->member), \
	                              type, member))

#endif/*CONFIG_RCU*/

__END_DECLS

#endif/*__GENERIC_RBTREE_H__*/
