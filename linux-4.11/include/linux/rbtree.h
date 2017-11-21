/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

  linux/include/linux/rbtree.h

  To use rbtrees you'll have to implement your own insert and search cores.
  This will avoid us to use callbacks and to drop drammatically performances.
  I know it's not the cleaner way,  but in C (not in C++) to get
  performances and genericity...

  See Documentation/rbtree.txt for documentation and samples.
*/

#ifndef	_LINUX_RBTREE_H
#define	_LINUX_RBTREE_H

#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/rcupdate.h>

struct rb_node {
	unsigned long  __rb_parent_color;
    // 현재 node 의 색과 부모로의 parent pointer 를 모두 가지는 변수
    // aligne 되어 있기 때문에 LSB 를 color 를 표현하도록 사용하고 
    // 나머지를 부모로의 pointer 로 사용
	struct rb_node *rb_right;
	struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));
    /* The alignment might seem pointless, but allegedly CRIS needs it */

struct rb_root {
	struct rb_node *rb_node;
};


#define rb_parent(r)   ((struct rb_node *)((r)->__rb_parent_color & ~3))
// rb_node 의 __rb_parent_color 에서 하위 2 bit 가져옴
#define RB_ROOT	(struct rb_root) { NULL, }
// rb_root 생성 및 초기화
#define	rb_entry(ptr, type, member) container_of(ptr, type, member)
// member 라는 이름으로 rb_node 를 가진 type 형 struct 의 주소를 ptr 에 가져옴
#define RB_EMPTY_ROOT(root)  (READ_ONCE((root)->rb_node) == NULL)
// rbtree 가 현재 비어있는지 검사
/* 'empty' nodes are nodes that are known not to be inserted in an rbtree */
#define RB_EMPTY_NODE(node)  \
	((node)->__rb_parent_color == (unsigned long)(node)) 
// rb_node 가 현재 rbtree 에 추가되어 있는 상태인지 검사
#define RB_CLEAR_NODE(node)  \
	((node)->__rb_parent_color = (unsigned long)(node))
// rb_node 생성 시, parent 를 가리키는 변수가 자기 자신을 
// 가리키도록 초기화

extern void rb_insert_color(struct rb_node *, struct rb_root *);
extern void rb_erase(struct rb_node *, struct rb_root *);


/* Find logical next and previous nodes in a tree */
extern struct rb_node *rb_next(const struct rb_node *);
extern struct rb_node *rb_prev(const struct rb_node *);
extern struct rb_node *rb_first(const struct rb_root *);
extern struct rb_node *rb_last(const struct rb_root *);

/* Postorder iteration - always visit the parent after its children */
extern struct rb_node *rb_first_postorder(const struct rb_root *);
extern struct rb_node *rb_next_postorder(const struct rb_node *);

/* Fast replacement of a single node without remove/rebalance/add/rebalance */
extern void rb_replace_node(struct rb_node *victim, struct rb_node *new,
			    struct rb_root *root);
extern void rb_replace_node_rcu(struct rb_node *victim, struct rb_node *new,
				struct rb_root *root);

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
				struct rb_node **rb_link)
{
	node->__rb_parent_color = (unsigned long)parent;
    // 처음 node 를 삽임할때 불려지기 위한 함수로 __rb_parent_color 를 
    // parent 로 설정한다는 것은 node 의 color 을 red 로 설정한다는 
    // 의미이기도 함    
	node->rb_left = node->rb_right = NULL;

	*rb_link = node;
    // 이 node 가 추가될 위치를 의미 rb_link_node 함수의 호출 전 left,right 의 
    // 위치가 결정되고 결정된 위치의 parent->rb_left 또는 parent->rb_right 를
    // 넘겨주어 tree 연결 수행 
    // 
    // 이항수 호출 후,  rb_insert_color 또는 __rb_insert 호출을 통해 
    // balance 수행
}

static inline void rb_link_node_rcu(struct rb_node *node, struct rb_node *parent,
				    struct rb_node **rb_link)
{
	node->__rb_parent_color = (unsigned long)parent;
	node->rb_left = node->rb_right = NULL;

	rcu_assign_pointer(*rb_link, node);
}

#define rb_entry_safe(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); \
	   ____ptr ? rb_entry(____ptr, type, member) : NULL; \
	})

/**
 * rbtree_postorder_for_each_entry_safe - iterate in post-order over rb_root of
 * given type allowing the backing memory of @pos to be invalidated
 *
 * @pos:	the 'type *' to use as a loop cursor.
 * @n:		another 'type *' to use as temporary storage
 * @root:	'rb_root *' of the rbtree.
 * @field:	the name of the rb_node field within 'type'.
 *
 * rbtree_postorder_for_each_entry_safe() provides a similar guarantee as
 * list_for_each_entry_safe() and allows the iteration to continue independent
 * of changes to @pos by the body of the loop.
 *
 * Note, however, that it cannot handle other modifications that re-order the
 * rbtree it is iterating over. This includes calling rb_erase() on @pos, as
 * rb_erase() may rebalance the tree, causing us to miss some nodes.
 */
#define rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
	for (pos = rb_entry_safe(rb_first_postorder(root), typeof(*pos), field); \
	     pos && ({ n = rb_entry_safe(rb_next_postorder(&pos->field), \
			typeof(*pos), field); 1; }); \
	     pos = n)

#endif	/* _LINUX_RBTREE_H */
