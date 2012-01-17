/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_open --
 *	Open a file handle.
 */
int
__wt_open(WT_SESSION_IMPL *session, const char *name, int create, WT_FH **fhp)
{
	const char *path;
	WT_CONNECTION_IMPL *conn;
	WT_FH *fh;
	mode_t mode;
	int f, fd, matched, ret;

	conn = S2C(session);
	fh = NULL;
	fd = -1;
	ret = 0;

	WT_VERBOSE(session, fileops, "%s: open", name);

	/* Increment the reference count if we already have the file open. */
	matched = 0;
	__wt_spin_lock(session, &conn->spinlock);
	TAILQ_FOREACH(fh, &conn->fhqh, q) {
		if (strcmp(name, fh->name) == 0) {
			++fh->refcnt;
			*fhp = fh;
			matched = 1;
			break;
		}
	}
	__wt_spin_unlock(session, &conn->spinlock);
	if (matched)
		return (0);

	WT_RET(__wt_filename(session, name, &path));

	f = O_RDWR;
#ifdef O_BINARY
	/* Windows clones: we always want to treat the file as a binary. */
	f |= O_BINARY;
#endif
	if (create) {
		f |= O_CREAT;
		mode = 0666;
	} else
		mode = 0;

	WT_SYSCALL_RETRY(((fd = open(path, f, mode)) == -1 ? 1 : 0), ret);
	if (ret != 0)
		WT_ERR_MSG(session, ret, "%s", name);

	WT_ERR(__wt_calloc(session, 1, sizeof(WT_FH), &fh));
	WT_ERR(__wt_strdup(session, name, &fh->name));

#if defined(HAVE_FCNTL) && defined(FD_CLOEXEC)
	/*
	 * Security:
	 * The application may spawn a new process, and we don't want another
	 * process to have access to our file handles.  There's an obvious
	 * race here...
	 */
	if ((f = fcntl(fd, F_GETFD)) == -1 ||
	    fcntl(fd, F_SETFD, f | FD_CLOEXEC) == -1)
		WT_ERR_MSG(session, __wt_errno(), "%s: fcntl", name);
#endif

	fh->fd = fd;
	fh->refcnt = 1;

	/* Set the file's size. */
	WT_ERR(__wt_filesize(session, fh, &fh->file_size));

	/* Link onto the environment's list of files. */
	__wt_spin_lock(session, &conn->spinlock);
	TAILQ_INSERT_TAIL(&conn->fhqh, fh, q);
	__wt_spin_unlock(session, &conn->spinlock);

	*fhp = fh;

	if (0) {
err:		if (fh != NULL) {
			__wt_free(session, fh->name);
			__wt_free(session, fh);
		}
		if (fd != -1)
			(void)close(fd);
	}

	__wt_free(session, path);
	return (ret);
}

/*
 * __wt_close --
 *	Close a file handle.
 */
int
__wt_close(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_CONNECTION_IMPL *conn;
	int ret;

	conn = S2C(session);
	ret = 0;

	if (fh == NULL || fh->refcnt == 0 || --fh->refcnt > 0)
		return (0);

	/* Remove from the list and discard the memory. */
	__wt_spin_lock(session, &conn->spinlock);
	TAILQ_REMOVE(&conn->fhqh, fh, q);
	__wt_spin_unlock(session, &conn->spinlock);

	if (close(fh->fd) != 0) {
		ret = __wt_errno();
		__wt_err(session, ret, "%s", fh->name);
	}

	__wt_free(session, fh->name);
	__wt_free(session, fh);
	return (ret);
}
