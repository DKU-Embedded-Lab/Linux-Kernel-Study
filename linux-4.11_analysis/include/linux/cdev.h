#ifndef _LINUX_CDEV_H
#define _LINUX_CDEV_H

#include <linux/kobject.h>
#include <linux/kdev_t.h>
#include <linux/list.h>

struct file_operations;
struct inode;
struct module;

// device database 에서 character device driver 를 나타내냄 
// char_device_struct 에 포함되어 char_device_struct 내의 major, minor 정보를 
// 통해 device 를 찾아내게 되고, cdev 에 접근하여 관련 ops 에 접근한다.
//
// 즉 도서관에서  cdev 는 책이고, char_device_struct 는 책에 붙어있는 
// index 스티커 라고 생각해도 될듯
struct cdev {
	struct kobject kobj;
    // cdev 가 hash 로 관리되는 cdev_map 에서의 각 노드 
    // 이 kobj 를 찾고 container_of 로 cdev 가져옴
	struct module *owner;
    // character device driver 관련 모듈
	const struct file_operations *ops;
    // 해당 character device specific file operations
	struct list_head list;
    // inode 의 i_devices 에 연결되어 해당 character device 
    // 에 관련된 device special file 들을 연결  
    //
    // e.g.
    //             i_devices     i_devices
    //      inode-A1 <--> inode-A2 <--> inode-A3   struct file-A1
    //  i_cdev |     i_cdev  |     i_cdev  |          |  f_op
    //         -----------------------------          |
    //                     |                          | 
    //                     |                          |
    //                     | list                     |
    //               struct cdev-A                    |
    //                    |                           |
    //                    |----------------------------
    //                    |
    //         struct file_operations(device specific)
    //

	dev_t dev;
    // device number 
	unsigned int count;
    // 현재 device 의 minor number 가 몇개인지를 나타냄
};

void cdev_init(struct cdev *, const struct file_operations *);

struct cdev *cdev_alloc(void);

void cdev_put(struct cdev *p);

int cdev_add(struct cdev *, dev_t, unsigned);

void cdev_del(struct cdev *);

void cd_forget(struct inode *);

#endif
