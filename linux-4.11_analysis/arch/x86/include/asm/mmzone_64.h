/* K8 NUMA support */
/* Copyright 2002,2003 by Andi Kleen, SuSE Labs */
/* 2.5 Version loosely based on the NUMAQ Code by Pat Gaughen. */
#ifndef _ASM_X86_MMZONE_64_H
#define _ASM_X86_MMZONE_64_H

#ifdef CONFIG_NUMA

#include <linux/mmdebug.h>
#include <asm/smp.h>

extern struct pglist_data *node_data[];

// architecture specific mecro 로 NUMA 의 각 node 에 해당하는 
// pg_data 즉 pglist_data 를 불러옴
// 
#define NODE_DATA(nid)		(node_data[nid])

#endif
#endif /* _ASM_X86_MMZONE_64_H */
