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
 * RCU (read-copy-update) - liburcu, in the flavour this build selected
 *
 * There is no hpc RCU API: this header only settles which liburcu flavour the
 * translation unit gets, and callers use liburcu's own interface -
 * rcu_read_lock()/rcu_read_unlock(), rcu_register_thread(), synchronize_rcu(),
 * call_rcu(), rcu_dereference()/rcu_assign_pointer(). The lockless variants of
 * the intrusive containers (<hpc/queue.h>, <hpc/rbtree.h>, <hpc/hash/table.h>)
 * publish and traverse with those same primitives.
 *
 * The flavour is a compile-time contract: the reader inlines and the
 * grace-period implementation have to be the same one, so CONFIG_RCU_* picks
 * both the header included here and the archive that gets linked (see
 * vendor/Kbuild.urcu). What differs is the read side's cost and what it owes
 * the implementation:
 *
 *   CONFIG_RCU_MEMB    default. Readers do plain loads and stores; the writer
 *                      forces the barrier with sys_membarrier(). Every thread
 *                      that takes a read-side section calls
 *                      rcu_register_thread() first, and
 *                      rcu_unregister_thread() before it exits.
 *   CONFIG_RCU_QSBR    the cheapest read side there is - read-side sections
 *                      cost nothing - in exchange for an obligation: nothing
 *                      marks where a reader is done, so a read-side thread must
 *                      announce a quiescent state itself, with
 *                      rcu_quiescent_state(), often enough that writers make
 *                      progress. It also registers.
 *   CONFIG_RCU_BP      bulletproof: registers a thread on its first read-side
 *                      section and needs no unregistration, at the price of a
 *                      slower read side. For code loaded into a process it does
 *                      not own.
 *
 * Read-side sections and grace periods are the caller's business - the container
 * traversals deliberately do not open one, so several of them can share a single
 * section:
 *
 *   rcu_register_thread();
 *   ...
 *   rcu_read_lock();
 *   queue_for_each_rcu(&q, it, struct entry, qnode) { ... }
 *   rcu_read_unlock();
 *   ...
 *   queue_del_rcu(&victim->qnode);   / * writers serialise among themselves * /
 *   synchronize_rcu();               / * or call_rcu() to defer the free * /
 *   free(victim);
 *
 * There is no non-RCU fallback on purpose: a plain access is not an RCU one, and
 * a no-op stand-in would hide the difference exactly where it matters. Code that
 * has to work either way keys off CONFIG_RCU itself, as the container headers do.
 */

#ifndef __GENERIC_RCU_H__
#define __GENERIC_RCU_H__

#ifdef CONFIG_RCU

/*
 * liburcu is LGPLv2.1 and hands out its read-side and pointer primitives as
 * inline code to LGPL-compatible callers (hpc is MIT) when _LGPL_SOURCE is
 * defined, instead of the out-of-line wrappers in the shared object. The build
 * defines it for the whole tree (see vendor/Kbuild.urcu) so every translation
 * unit agrees; repeat it here for a hand-rolled compile. It has to be in place
 * before the first urcu header is pulled in.
 */
#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE
#endif

#if defined(CONFIG_RCU_QSBR)
#include <urcu-qsbr.h>
#elif defined(CONFIG_RCU_BP)
#include <urcu-bp.h>
#else
#include <urcu.h>
#endif
#include <urcu-pointer.h>
#include <urcu-call-rcu.h>

#endif/*CONFIG_RCU*/

/*
 * Without CONFIG_RCU this header contributes nothing - and deliberately no
 * substitute API either. rcu_read_lock(), synchronize_rcu() and friends are then
 * simply undeclared, so a caller that needs them fails at the line that needs
 * them, in a build where they could not have worked anyway.
 */

#endif/*__GENERIC_RCU_H__*/
