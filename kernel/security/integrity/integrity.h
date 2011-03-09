/*
 * Copyright (C) 2009-2010 IBM Corporation
 *
 * Authors:
 * Mimi Zohar <zohar@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 */

#include <linux/types.h>
#include <linux/integrity.h>
#include <crypto/sha.h>

/* iint cache flags */
#define IMA_MEASURED		0x01

/* integrity data associated with an inode */
struct integrity_iint_cache {
	struct rb_node rb_node; /* rooted in integrity_iint_tree */
	struct inode *inode;	/* back pointer to inode in question */
	u64 version;		/* track inode changes */
	unsigned char flags;
	u8 digest[SHA1_DIGEST_SIZE];
	struct mutex mutex;	/* protects: version, flags, digest */
};

/* rbtree tree calls to lookup, insert, delete
 * integrity data associated with an inode.
 */
struct integrity_iint_cache *integrity_iint_insert(struct inode *inode);
struct integrity_iint_cache *integrity_iint_find(struct inode *inode);
