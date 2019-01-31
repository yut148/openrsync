/*	$Id$ */

/*
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/mman.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "extern.h"

/*
 * We need to process our directories in post-order.
 * This is because touching files within the directory will change the
 * directory file; and also, if our mode is not writable, we wouldn't be
 * able to write to that directory!
 * Returns zero on failure, non-zero on success.
 */
static int
post_dir(struct sess *sess, int root, const struct flist *f, int newdir)
{
	struct timespec	 tv[2];
	int		 rc;

	/* We already warned about the directory in pre_process_dir(). */

	if ( ! sess->opts->recursive)
		return 1;
	else if (sess->opts->dry_run)
		return 1;

	/* XXX: re-check that this is a directory? */

	if (sess->opts->preserve_times) {
		tv[0].tv_sec = time(NULL);
		tv[0].tv_nsec = 0;
		tv[1].tv_sec = f->st.mtime;
		tv[1].tv_nsec = 0;
		rc = utimensat(root, f->path, tv, 0);
		if (-1 == rc) {
			ERR(sess, "utimensat: %s", f->path);
			return 0;
		}
		LOG4(sess, "%s: updated date", f->path);
	}

	if (newdir || sess->opts->preserve_perms) {
		rc = fchmodat(root, f->path, f->st.mode, 0);
		if (-1 == rc) {
			ERR(sess, "fchmodat: %s", f->path);
			return 0;
		}
		LOG4(sess, "%s: updated mode: %o",
			f->path, f->st.mode);
	}

	return 1;
}

/* 
 * Pledges: unveil, rpath, cpath, wpath, stdio, fattr.
 * Pledges (dry-run): -cpath, -wpath, -fattr.
 * Pledges (!preserve_times): -fattr.
 */
int
rsync_receiver(struct sess *sess,
	int fdin, int fdout, const char *root)
{
	struct flist	*fl = NULL, *dfl = NULL;
	size_t		 i, flsz = 0, csum_length = CSUM_LENGTH_PHASE1,
			 dflsz = 0;
	char		*tofree;
	int		 rc = 0, dfd = -1, phase = 0, c, timeo;
	int32_t	 	 ioerror;
	int		*newdir = NULL;
	mode_t		 oumask;
	struct pollfd	 pfd[2];
	struct download	*dl = NULL;

	if (-1 == pledge("unveil rpath cpath wpath stdio fattr", NULL)) {
		ERR(sess, "pledge");
		goto out;
	}

	/* XXX: what does this do? */

	if ( ! sess->opts->server &&
	     ! io_write_int(sess, fdout, 0)) {
		ERRX1(sess, "io_write_int: zero premable");
		goto out;
	}

	/*
	 * Start by receiving the file list and our mystery number.
	 * These we're going to be touching on our local system.
	 */

	if ( ! flist_recv(sess, fdin, &fl, &flsz)) {
		ERRX1(sess, "flist_recv");
		goto out;
	} else if ( ! io_read_int(sess, fdin, &ioerror)) {
		ERRX1(sess, "io_read_int: io_error");
		goto out;
	} else if (0 != ioerror) {
		ERRX1(sess, "io_error is non-zero");
		goto out;
	}

	if (0 == flsz && ! sess->opts->server) {
		WARNX(sess, "receiver has empty file list: exiting");
		rc = 1;
		goto out;
	} else if ( ! sess->opts->server)
		LOG1(sess, "Transfer starting: %zu files", flsz);

	LOG2(sess, "%s: receiver destination", root);

	/*
	 * Create the path for our destination directory, if we're not
	 * in dry-run mode (which would otherwise crash w/the pledge).
	 * This uses our current umask: we might set the permissions on
	 * this directory in post_dir().
	 */

	if ( ! sess->opts->dry_run) {
		if (NULL == (tofree = strdup(root))) {
			ERR(sess, "strdup");
			goto out;
		} else if (mkpath(sess, tofree) < 0) {
			ERRX1(sess, "%s: mkpath", root);
			free(tofree);
			goto out;
		}
		free(tofree);
	}

	/*
	 * Disable umask() so we can set permissions fully.
	 * Then open the directory iff we're not in dry_run.
	 */

	oumask = umask(0);

	if ( ! sess->opts->dry_run) {
		dfd = open(root, O_RDONLY | O_DIRECTORY, 0);
		if (-1 == dfd) {
			ERR(sess, "%s: open", root);
			goto out;
		}
	}

	/*
	 * Begin by conditionally getting all files we have currently
	 * available in our destination.
	 * XXX: do this *before* the unveil() because fts_read() doesn't
	 * work properly afterward.
	 */

	if (sess->opts->del && sess->opts->recursive)
		if ( ! flist_gen_local(sess, root, &dfl, &dflsz)) {
			ERRX1(sess, "%s: flist_gen_local", root);
			goto out;
		}

	/*
	 * Make our entire view of the file-system be limited to what's
	 * in the root directory.
	 * This prevents us from accidentally (or "under the influence")
	 * writing into other parts of the file-system.
	 */

	if (-1 == unveil(root, "rwc")) {
		ERR(sess, "%s: unveil", root);
		goto out;
	} else if (-1 == unveil(NULL, NULL)) {
		ERR(sess, "%s: unveil (lock down)", root);
		goto out;
	}

	/* If we have a local set, go for the deletion. */

	if (NULL != dfl)
		if ( ! flist_del(sess, dfd, dfl, dflsz, fl, flsz)) {
			ERRX1(sess, "%s: flist_del", root);
			goto out;
		}

	/*
	 * FIXME: I never use the full checksum amount; but if I were,
	 * here is where the "again" label would go.
	 * This has been demonstrated to work, but I just don't use it
	 * til I understand the need.
	 */

	LOG2(sess, "%s: ready for phase 1 data", root);

	if (NULL == (newdir = calloc(flsz, sizeof(int)))) {
		ERR(sess, "calloc");
		goto out;
	}

	/* Initialise poll events to listen from the sender. */

	pfd[0].fd = fdin;
	pfd[1].fd = -1;
	pfd[0].events = pfd[1].events = POLLIN;

	for (i = 0;;) {
		/*
		 * If we've sent all of our block requests, so "i" is
		 * set to the file list length, then we wait to receive
		 * until the phase changes.
		 * If we're waiting for a local file to open so we can
		 * mmap it, so "i" is a valid file and pfd[1].fd is
		 * valid, then wait for that perpetually.
		 * Otherwise, we don't wait at all so that the uploader
		 * can get the next file sent upstream.
		 */

		timeo = (i == flsz || -1 != pfd[1].fd) ? INFTIM : 0;

		if (-1 == (c = poll(pfd, 2, timeo))) {
			ERR(sess, "poll");
			goto out;
		} else if ((pfd[0].revents & (POLLERR|POLLNVAL))) {
			ERRX(sess, "poll: bad fd");
			goto out;
		} else if ((pfd[1].revents & (POLLERR|POLLNVAL))) {
			ERRX(sess, "poll: bad fd");
			goto out;
		} else if ((pfd[0].revents & POLLHUP)) {
			ERRX(sess, "poll: hangup");
			goto out;
		} else if ((pfd[1].revents & POLLHUP)) {
			ERRX(sess, "poll: hangup");
			goto out;
		}

		/*
		 * We run the uploader if we have files left to examine
		 * (if "i" is less than the file list) or if we have a
		 * file that we've opened and is read to mmap.
		 */

		if (i < flsz || POLLIN & pfd[1].revents) {
			c = rsync_uploader(fdout, dfd, &i, 
				&pfd[1].fd, fl, flsz, sess, 
				csum_length, oumask, newdir);
			if (c < 0) {
				ERRX1(sess, "rsync_uploader");
				goto out;
			}
		}

		/* 
		 * We run the downloader if there are reads coming from
		 * the sender.
		 * These reads may just be multiplexed log messages, so
		 * let's check for that before passing to the downloader.
		 * We would otherwise block after flushing the log
		 * message and waiting for data.
		 */

		if (POLLIN & pfd[0].revents) {
			if (sess->mplex_reads) {
				if ( ! io_read_flush(sess, fdin)) {
					ERRX1(sess, "io_read_flush");
					goto out;
				} else if (0 == sess->mplex_read_remain)
					continue;
			}
			c = rsync_downloader
				(fdin, dfd, &dl, fl, flsz, sess);
			if (c < 0) {
				ERRX1(sess, "rsync_downloader");
				goto out;
			} else if (0 == c) {
				assert(0 == phase);
				phase++;
				LOG2(sess, "%s: receiver ready "
					"for phase 2 data", root);
				break;
			}

			/*
			 * FIXME: if we have any errors during the
			 * download, most notably files getting out of
			 * sync between the send and the receiver, then
			 * here we should bump our checksum length and
			 * go into the second phase.
			 */
		} 
	}

	/* Fix up the directory permissions and times post-order. */

	if (sess->opts->preserve_times ||
	    sess->opts->preserve_perms)
		for (i = 0; i < flsz; i++) {
			if ( ! S_ISDIR(fl[i].st.mode))
				continue;
			if ( ! post_dir(sess, dfd, &fl[i], newdir[i]))
				goto out;
		}

	/* Properly close us out by progressing through the phases. */

	if (1 == phase) {
		if ( ! io_write_int(sess, fdout, -1)) {
			ERRX1(sess, "io_write_int: send complete");
			goto out;
		} else if ( ! io_read_int(sess, fdin, &ioerror)) {
			ERRX1(sess, "io_read_int: phase ack");
			goto out;
		} else if (-1 != ioerror) {
			ERRX(sess, "expected phase ack");
			goto out;
		}
		phase++;
	}

	/* If we're the client, read server statistics. */

	if ( ! sess->opts->server &&
	     ! sess_stats_recv(sess, fdin)) {
		ERRX1(sess, "sess_stats_recv");
		goto out;
	}

	/* Final "goodbye" message. */

	if ( ! io_write_int(sess, fdout, -1)) {
		ERRX1(sess, "io_write_int: update complete");
		goto out;
	}

	LOG2(sess, "receiver finished updating");

	rc = 1;
out:
	if (-1 != dfd)
		close(dfd);
	flist_free(fl, flsz);
	flist_free(dfl, dflsz);
	free(newdir);
	return rc;
}
