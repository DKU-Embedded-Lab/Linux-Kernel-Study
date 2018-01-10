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
	struct module *owner;
	const struct file_operations *ops;
    // 해당 device driver 관련 file operations
	struct list_head list;
	dev_t dev;
	unsigned int count;
};

void cdev_init(struct cdev *, const struct file_operations *);

struct cdev *cdev_alloc(void);

void cdev_put(struct cdev *p);

int cdev_add(struct cdev *, dev_t, unsigned);

void cdev_del(struct cdev *);

void cd_forget(struct inode *);

#endif
