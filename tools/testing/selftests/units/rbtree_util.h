/*
 * Shared scaffolding for the <hpc/rbtree.h> units - rbtree.c (plain spelling)
 * and rbtree_rcu.c (lockless spelling).
 *
 * The tree is policy-free: ordering and the structural invariants live with the
 * caller, so both units need the same three things - a payload type, a BST
 * descent that finds the slot a key belongs in, and the red-black audit. They are
 * here rather than duplicated so the audit in particular has one definition.
 */

#ifndef __HPC_TEST_RBTREE_UTIL_H__
#define __HPC_TEST_RBTREE_UTIL_H__

/* the audit asserts through cmocka, which wants these ahead of it */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <hpc/compiler.h>
#include <hpc/rbtree.h>

struct data { unsigned int id; struct rbnode rb; };

/*
 * Descend to the slot @id belongs in, reporting the node that will parent it.
 * Returns a pointer to the (empty) child slot, or to an occupied one when the
 * key is already present - which is how the callers reject duplicates.
 */
static struct rbnode **
find_slot(struct rbtree *t, unsigned id, struct rbnode **parent_out)
{
	struct rbnode **link = &t->root, *parent = NULL;

	while (*link) {
		struct data *d = rbtree_entry(*link, struct data, rb);
		parent = *link;
		if (id < d->id)
			link = &(*link)->left;
		else if (id > d->id)
			link = &(*link)->right;
		else
			break;                 /* duplicate key */
	}
	*parent_out = parent;
	return link;
}

static struct data *
tree_find(struct rbtree *t, unsigned id)
{
	struct rbnode *n = t->root;

	while (n) {
		struct data *d = rbtree_entry(n, struct data, rb);
		if (id < d->id)
			n = n->left;
		else if (id > d->id)
			n = n->right;
		else
			return d;
	}
	return NULL;
}

/* ---- structural audit ---------------------------------------------------- */

static int
audit(struct rbnode *n, struct rbnode *parent)
{
	int lh, rh;

	if (!n)
		return 1;                      /* NULL leaves count black */
	assert_ptr_equal(n->parent, parent);
	if (n->color == RBTREE_RED) {
		assert_true(!n->left  || n->left->color  == RBTREE_BLACK);
		assert_true(!n->right || n->right->color == RBTREE_BLACK);
	}
	if (n->left)
		assert_true(rbtree_entry(n->left, struct data, rb)->id <
		            rbtree_entry(n, struct data, rb)->id);
	if (n->right)
		assert_true(rbtree_entry(n->right, struct data, rb)->id >
		            rbtree_entry(n, struct data, rb)->id);
	lh = audit(n->left, n);
	rh = audit(n->right, n);
	assert_int_equal(lh, rh);           /* equal black-height on both sides */
	return lh + (n->color == RBTREE_BLACK ? 1 : 0);
}

static void
validate(struct rbtree *t)
{
	if (t->root) {
		assert_int_equal(t->root->color, RBTREE_BLACK);
		assert_null(t->root->parent);
		audit(t->root, NULL);
	}
}

#endif/*__HPC_TEST_RBTREE_UTIL_H__*/
