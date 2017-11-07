#ifndef _UAPI_LINUX_UTSNAME_H
#define _UAPI_LINUX_UTSNAME_H

#define __OLD_UTS_LEN 8

struct oldold_utsname {
	char sysname[9];
	char nodename[9];
	char release[9];
	char version[9];
	char machine[9];
};

#define __NEW_UTS_LEN 64

struct old_utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
};

// ute_namespace 에 초기화 될 새로운 namespace 가 담길 값들로
// 변하지 않는 속성, 변하는 속성등이 있다.
struct new_utsname {
	char sysname[__NEW_UTS_LEN + 1];    // e.g. linux -> /proc/sys/kernel/ostype
	char nodename[__NEW_UTS_LEN + 1];   // e.g. son -> /proc/sys/kernel/hostname 
	char release[__NEW_UTS_LEN + 1];    // e.g. 4.11.0 -> /proc/sys/kernel/osrelease
	char version[__NEW_UTS_LEN + 1];    // e.g. #8 SMP Sun Jul 16 23:31:38 KST 2017 -> /proc/sys/kernel/version
	char machine[__NEW_UTS_LEN + 1];    // e.g. x86_64
    char domainname[__NEW_UTS_LEN + 1]; // e.g. none -> /proc/sys/kernel/domainname
};


#endif /* _UAPI_LINUX_UTSNAME_H */
