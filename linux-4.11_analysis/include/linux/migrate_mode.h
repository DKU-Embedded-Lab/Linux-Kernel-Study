#ifndef MIGRATE_MODE_H_INCLUDED
#define MIGRATE_MODE_H_INCLUDED
/*
 * MIGRATE_ASYNC means never block
 * MIGRATE_SYNC_LIGHT in the current implementation means to allow blocking
 *	on most operations but not ->writepage as the potential stall time
 *	is too significant
 * MIGRATE_SYNC will block when migrating pages
 */
enum migrate_mode {
	MIGRATE_ASYNC, // kcompactd 에 의한 compaction 
	MIGRATE_SYNC_LIGHT, // page alloc 에 의한 compaction
	MIGRATE_SYNC,// sysfs 에 의한compaction
};

#endif		/* MIGRATE_MODE_H_INCLUDED */
