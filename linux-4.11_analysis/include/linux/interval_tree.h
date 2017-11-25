#ifndef _LINUX_INTERVAL_TREE_H
#define _LINUX_INTERVAL_TREE_H

#include <linux/rbtree.h>

struct interval_tree_node {
	struct rb_node rb;
    // red black tree node
	unsigned long start;	/* Start of interval */ 
    // 현재 node range 의 start
	unsigned long last;	/* Last location _in_ interval */
    // 현재 node range 의 end
	unsigned long __subtree_last;
    // red black tree 에서 child subtree 의 가장 큰 last 값
};

extern void
interval_tree_insert(struct interval_tree_node *node, struct rb_root *root);

extern void
interval_tree_remove(struct interval_tree_node *node, struct rb_root *root);

extern struct interval_tree_node *
interval_tree_iter_first(struct rb_root *root,
			 unsigned long start, unsigned long last);

extern struct interval_tree_node *
interval_tree_iter_next(struct interval_tree_node *node,
			unsigned long start, unsigned long last);

#endif	/* _LINUX_INTERVAL_TREE_H */ 


