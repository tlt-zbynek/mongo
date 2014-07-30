/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __session_checkpoint(WT_SESSION *, const char *);
static int __session_rollback_transaction(WT_SESSION *, const char *);

/*
 * __wt_session_reset_cursors --
 *	Reset all open cursors.
 */
int
__wt_session_reset_cursors(WT_SESSION_IMPL *session)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;

	TAILQ_FOREACH(cursor, &session->cursors, q)
		WT_TRET(cursor->reset(cursor));
	return (ret);
}

/*
 * __session_close_cache --
 *	Close any cached handles in a session.  Called holding the schema lock.
 */
static void
__session_close_cache(WT_SESSION_IMPL *session)
{
	WT_DATA_HANDLE_CACHE *dhandle_cache;

	while ((dhandle_cache = SLIST_FIRST(&session->dhandles)) != NULL)
		__wt_session_discard_btree(session, dhandle_cache);
}

/*
 * __session_clear --
 *	Clear a session structure.
 */
static void
__session_clear(WT_SESSION_IMPL *session)
{
	/*
	 * There's no serialization support around the review of the hazard
	 * array, which means threads checking for hazard pointers first check
	 * the active field (which may be 0) and then use the hazard pointer
	 * (which cannot be NULL).
	 *
	 * Additionally, the session structure can include information that
	 * persists past the session's end-of-life, stored as part of page
	 * splits.
	 *
	 * For these reasons, be careful when clearing the session structure.
	 */
	memset(session, 0, WT_SESSION_CLEAR_SIZE(session));
	session->hazard_size = 0;
	session->nhazard = 0;
}

/*
 * __session_close --
 *	WT_SESSION->close method.
 */
static int
__session_close(WT_SESSION *wt_session, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_session->connection;
	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, close, config, cfg);
	WT_UNUSED(cfg);

	/* Rollback any active transaction. */
	if (F_ISSET(&session->txn, TXN_RUNNING))
		WT_TRET(__session_rollback_transaction(wt_session, NULL));

	/* Close all open cursors. */
	while ((cursor = TAILQ_FIRST(&session->cursors)) != NULL) {
		/*
		 * Notify the user that we are closing the cursor handle
		 * via the registered close callback.
		 */
		if (session->event_handler->handle_close != NULL)
			WT_TRET(session->event_handler->handle_close(
			    session->event_handler, wt_session, cursor));
		WT_TRET(cursor->close(cursor));
	}

	WT_ASSERT(session, session->ncursors == 0);

	/* Discard cached handles. */
	__session_close_cache(session);

	/* Close all tables. */
	__wt_schema_close_tables(session);

	/* Discard metadata tracking. */
	__wt_meta_track_discard(session);

	/* Discard scratch buffers. */
	__wt_scr_discard(session);

	/* Free transaction information. */
	__wt_txn_destroy(session);

	/* Confirm we're not holding any hazard pointers. */
	__wt_hazard_close(session);

	/* Cleanup */
	if (session->block_manager_cleanup != NULL)
		WT_TRET(session->block_manager_cleanup(session));
	if (session->reconcile_cleanup != NULL)
		WT_TRET(session->reconcile_cleanup(session));

	/* Free the eviction exclusive-lock information. */
	__wt_free(session, session->excl);

	/* Destroy the thread's mutex. */
	WT_TRET(__wt_cond_destroy(session, &session->cond));

	/* The API lock protects opening and closing of sessions. */
	__wt_spin_lock(session, &conn->api_lock);

	/*
	 * Sessions are re-used, clear the structure: the clear sets the active
	 * field to 0, which will exclude the hazard array from review by the
	 * eviction thread.   Because some session fields are accessed by other
	 * threads, the structure must be cleared carefully.
	 *
	 * We don't need to publish here, because regardless of the active field
	 * being non-zero, the hazard pointer is always valid.
	 */
	__session_clear(session);
	session = conn->default_session;

	/*
	 * Decrement the count of active sessions if that's possible: a session
	 * being closed may or may not be at the end of the array, step toward
	 * the beginning of the array until we reach an active session.
	 */
	while (conn->sessions[conn->session_cnt - 1].active == 0)
		if (--conn->session_cnt == 0)
			break;

	__wt_spin_unlock(session, &conn->api_lock);

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_reconfigure --
 *	WT_SESSION->reconfigure method.
 */
static int
__session_reconfigure(WT_SESSION *wt_session, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, reconfigure, config, cfg);

	if (F_ISSET(&session->txn, TXN_RUNNING))
		WT_ERR_MSG(session, EINVAL, "transaction in progress");

	WT_TRET(__wt_session_reset_cursors(session));

	WT_ERR(__wt_config_gets_def(session, cfg, "isolation", 0, &cval));
	if (cval.len != 0)
		session->isolation = session->txn.isolation =
		    WT_STRING_MATCH("snapshot", cval.str, cval.len) ?
		    TXN_ISO_SNAPSHOT :
		    WT_STRING_MATCH("read-uncommitted", cval.str, cval.len) ?
		    TXN_ISO_READ_UNCOMMITTED : TXN_ISO_READ_COMMITTED;

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_open_cursor --
 *	Internal version of WT_SESSION::open_cursor.
 */
int
__wt_open_cursor(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_COLGROUP *colgroup;
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;

	if (WT_PREFIX_MATCH(uri, "backup:"))
		ret = __wt_curbackup_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "colgroup:")) {
		/*
		 * Column groups are a special case: open a cursor on the
		 * underlying data source.
		 */
		WT_RET(__wt_schema_get_colgroup(session, uri, NULL, &colgroup));
		ret = __wt_open_cursor(
		    session, colgroup->source, owner, cfg, cursorp);
	} else if (WT_PREFIX_MATCH(uri, "config:"))
		ret = __wt_curconfig_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __wt_curfile_open(session, uri, owner, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "lsm:"))
		ret = __wt_clsm_open(session, uri, owner, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, WT_METADATA_URI))
		ret = __wt_curmetadata_open(session, uri, owner, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "index:"))
		ret = __wt_curindex_open(session, uri, owner, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "statistics:"))
		ret = __wt_curstat_open(session, uri, cfg, cursorp);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __wt_curtable_open(session, uri, cfg, cursorp);
	else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL)
		ret = dsrc->open_cursor == NULL ?
		    __wt_object_unsupported(session, uri) :
		    __wt_curds_create(session, uri, owner, cfg, dsrc, cursorp);
	else
		ret = __wt_bad_object_type(session, uri);

	return (ret);
}

/*
 * __session_open_cursor --
 *	WT_SESSION->open_cursor method.
 */
static int
__session_open_cursor(WT_SESSION *wt_session,
    const char *uri, WT_CURSOR *to_dup, const char *config, WT_CURSOR **cursorp)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cursor = *cursorp = NULL;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, open_cursor, config, cfg);

	if ((to_dup == NULL && uri == NULL) || (to_dup != NULL && uri != NULL))
		WT_ERR_MSG(session, EINVAL,
		    "should be passed either a URI or a cursor to duplicate, "
		    "but not both");

	if (to_dup != NULL) {
		uri = to_dup->uri;
		if (!WT_PREFIX_MATCH(uri, "colgroup:") &&
		    !WT_PREFIX_MATCH(uri, "index:") &&
		    !WT_PREFIX_MATCH(uri, "file:") &&
		    !WT_PREFIX_MATCH(uri, "lsm:") &&
		    !WT_PREFIX_MATCH(uri, WT_METADATA_URI) &&
		    !WT_PREFIX_MATCH(uri, "table:") &&
		    __wt_schema_get_source(session, uri) == NULL)
			WT_ERR(__wt_bad_object_type(session, uri));
	}

	WT_ERR(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
	if (to_dup != NULL)
		WT_ERR(__wt_cursor_dup_position(to_dup, cursor));

	*cursorp = cursor;

	if (0) {
err:		if (cursor != NULL)
			WT_TRET(cursor->close(cursor));
	}

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_session_create_strip --
 *	Discard any configuration information from a schema entry that is not
 * applicable to an session.create call, here for the wt dump command utility,
 * which only wants to dump the schema information needed for load.
 */
int
__wt_session_create_strip(WT_SESSION *wt_session,
    const char *v1, const char *v2, const char **value_ret)
{
	WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;
	const char *cfg[] =
	    { WT_CONFIG_BASE(session, session_create), v1, v2, NULL };

	return (__wt_config_collapse(session, cfg, value_ret));
}

/*
 * __session_create --
 *	WT_SESSION->create method.
 */
static int
__session_create(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, create, config, cfg);
	WT_UNUSED(cfg);

	/* Disallow objects in the WiredTiger name space. */
	WT_ERR(__wt_schema_name_check(session, uri));

	/*
	 * Type configuration only applies to tables, column groups and indexes.
	 * We don't want applications to attempt to layer LSM on top of their
	 * extended data-sources, and the fact we allow LSM as a valid URI is an
	 * invitation to that mistake: nip it in the bud.
	 */
	if (!WT_PREFIX_MATCH(uri, "colgroup:") &&
	    !WT_PREFIX_MATCH(uri, "index:") &&
	    !WT_PREFIX_MATCH(uri, "table:")) {
		/*
		 * We can't disallow type entirely, a configuration string might
		 * innocently include it, for example, a dump/load pair.  If the
		 * URI type prefix and the type are the same, let it go.
		 */
		if ((ret =
		    __wt_config_getones(session, config, "type", &cval)) == 0 &&
		    (strncmp(uri, cval.str, cval.len) != 0 ||
		    uri[cval.len] != ':'))
			WT_ERR_MSG(session, EINVAL,
			    "%s: unsupported type configuration", uri);
		WT_ERR_NOTFOUND_OK(ret);
	}

	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_create(session, uri, config));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_log_printf --
 *	WT_SESSION->log_printf method.
 */
static int
__session_log_printf(WT_SESSION *wt_session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	WT_SESSION_IMPL *session;
	WT_DECL_RET;
	va_list ap;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL_NO_CONF(session, log_printf);

	va_start(ap, fmt);
	ret = __wt_log_vprintf(session, fmt, ap);
	va_end(ap);

err:	API_END_RET(session, ret);
}

/*
 * __session_rename --
 *	WT_SESSION->rename method.
 */
static int
__session_rename(WT_SESSION *wt_session,
    const char *uri, const char *newuri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, rename, config, cfg);

	/* Disallow objects in the WiredTiger name space. */
	WT_ERR(__wt_schema_name_check(session, uri));
	WT_ERR(__wt_schema_name_check(session, newuri));

	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_rename(session, uri, newuri, cfg));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_compact --
 *	WT_SESSION->compact method.
 */
static int
__session_compact(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	/* Disallow objects in the WiredTiger name space. */
	WT_RET(__wt_schema_name_check(session, uri));

	if (!WT_PREFIX_MATCH(uri, "colgroup:") &&
	    !WT_PREFIX_MATCH(uri, "file:") &&
	    !WT_PREFIX_MATCH(uri, "index:") &&
	    !WT_PREFIX_MATCH(uri, "lsm:") &&
	    !WT_PREFIX_MATCH(uri, "table:"))
		return (__wt_bad_object_type(session, uri));

	return (__wt_session_compact(wt_session, uri, config));
}

/*
 * __session_drop --
 *	WT_SESSION->drop method.
 */
static int
__session_drop(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, drop, config, cfg);

	/* Disallow objects in the WiredTiger name space. */
	WT_ERR(__wt_schema_name_check(session, uri));

	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_drop(session, uri, cfg));

err:	/* Note: drop operations cannot be unrolled (yet?). */
	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_salvage --
 *	WT_SESSION->salvage method.
 */
static int
__session_salvage(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, salvage, config, cfg);
	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_worker(session, uri, __wt_salvage,
		NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_SALVAGE));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_truncate --
 *	WT_SESSION->truncate method.
 */
static int
__session_truncate(WT_SESSION *wt_session,
    const char *uri, WT_CURSOR *start, WT_CURSOR *stop, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_CURSOR *cursor;
	int cmp;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_TXN_API_CALL(session, truncate, config, cfg);

	/*
	 * If the URI is specified, we don't need a start/stop, if start/stop
	 * is specified, we don't need a URI.
	 *
	 * If no URI is specified, and both cursors are specified, start/stop
	 * must reference the same object.
	 *
	 * Any specified cursor must have been initialized.
	 */
	if ((uri == NULL && start == NULL && stop == NULL) ||
	    (uri != NULL && (start != NULL || stop != NULL)))
		WT_ERR_MSG(session, EINVAL,
		    "the truncate method should be passed either a URI or "
		    "start/stop cursors, but not both");

	if (uri != NULL) {
		/* Disallow objects in the WiredTiger name space. */
		WT_ERR(__wt_schema_name_check(session, uri));

		WT_WITH_SCHEMA_LOCK(session,
		    ret = __wt_schema_truncate(session, uri, cfg));
		goto done;
	}

	/*
	 * Cursor truncate is only supported for some objects, check for the
	 * supporting methods we need, range_truncate and compare.
	 */
	cursor = start == NULL ? stop : start;
	if (cursor->compare == NULL)
		WT_ERR(__wt_bad_object_type(session, cursor->uri));

	/*
	 * If both cursors set, check they're correctly ordered with respect to
	 * each other.  We have to test this before any search, the search can
	 * change the initial cursor position.
	 *
	 * Rather happily, the compare routine will also confirm the cursors
	 * reference the same object and the keys are set.
	 */
	if (start != NULL && stop != NULL) {
		WT_ERR(start->compare(start, stop, &cmp));
		if (cmp > 0)
			WT_ERR_MSG(session, EINVAL,
			    "the start cursor position is after the stop "
			    "cursor position");
	}

	/*
	 * Truncate does not require keys actually exist so that applications
	 * can discard parts of the object's name space without knowing exactly
	 * what records currently appear in the object.  For this reason, do a
	 * search-near, rather than a search.  Additionally, we have to correct
	 * after calling search-near, to position the start/stop cursors on the
	 * next record greater than/less than the original key.  If the cursors
	 * hit the beginning/end of the object, or the start/stop keys cross,
	 * we're done, the range must be empty.
	 */
	if (start != NULL) {
		WT_ERR(start->search_near(start, &cmp));
		if (cmp < 0 && (ret = start->next(start)) != 0) {
			WT_ERR_NOTFOUND_OK(ret);
			goto done;
		}
	}
	if (stop != NULL) {
		WT_ERR(stop->search_near(stop, &cmp));
		if (cmp > 0 && (ret = stop->prev(stop)) != 0) {
			WT_ERR_NOTFOUND_OK(ret);
			goto done;
		}

		if (start != NULL) {
			WT_ERR(start->compare(start, stop, &cmp));
			if (cmp > 0)
				goto done;
		}
	}

	WT_ERR(__wt_schema_range_truncate(session, start, stop));

done:
err:	TXN_API_END_RETRY(session, ret, 0);
	return ((ret) == WT_NOTFOUND ? ENOENT : (ret));
}

/*
 * __session_upgrade --
 *	WT_SESSION->upgrade method.
 */
static int
__session_upgrade(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, upgrade, config, cfg);
	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_worker(session, uri, __wt_upgrade,
		NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_UPGRADE));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_verify --
 *	WT_SESSION->verify method.
 */
static int
__session_verify(WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	SESSION_API_CALL(session, verify, config, cfg);
	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_worker(session, uri, __wt_verify,
		NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_VERIFY));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_begin_transaction --
 *	WT_SESSION->begin_transaction method.
 */
static int
__session_begin_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, begin_transaction, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, txn_begin);

	if (F_ISSET(&session->txn, TXN_RUNNING))
		WT_ERR_MSG(session, EINVAL, "Transaction already running");

	WT_ERR(__wt_session_reset_cursors(session));

	/*
	 * Now there are no cursors open and no transaction active in this
	 * thread.  Check if the cache is full: if we have to block for
	 * eviction, this is the best time to do it.
	 */
	WT_ERR(__wt_cache_full_check(session));

	ret = __wt_txn_begin(session, cfg);

err:	API_END_RET(session, ret);
}

/*
 * __session_commit_transaction --
 *	WT_SESSION->commit_transaction method.
 */
static int
__session_commit_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_TXN *txn;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, commit_transaction, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, txn_commit);

	txn = &session->txn;
	if (F_ISSET(txn, TXN_ERROR)) {
		__wt_errx(session, "failed transaction requires rollback");
		ret = EINVAL;
	}

	WT_TRET(__wt_session_reset_cursors(session));

	if (ret == 0)
		ret = __wt_txn_commit(session, cfg);
	else
		WT_TRET(__wt_txn_rollback(session, cfg));

err:	API_END_RET(session, ret);
}

/*
 * __session_rollback_transaction --
 *	WT_SESSION->rollback_transaction method.
 */
static int
__session_rollback_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, rollback_transaction, config, cfg);
	WT_STAT_FAST_CONN_INCR(session, txn_rollback);

	WT_TRET(__wt_session_reset_cursors(session));

	WT_TRET(__wt_txn_rollback(session, cfg));

err:	API_END_RET(session, ret);
}

/*
 * __session_checkpoint --
 *	WT_SESSION->checkpoint method.
 */
static int
__session_checkpoint(WT_SESSION *wt_session, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_TXN *txn;

	session = (WT_SESSION_IMPL *)wt_session;

	txn = &session->txn;

	WT_STAT_FAST_CONN_INCR(session, txn_checkpoint);
	SESSION_API_CALL(session, checkpoint, config, cfg);

	/* Don't highjack the session checkpoint thread for eviction. */
	F_SET(session, WT_SESSION_NO_CACHE_CHECK);

	/*
	 * Checkpoints require a snapshot to write a transactionally consistent
	 * snapshot of the data.
	 *
	 * We can't use an application's transaction: if it has uncommitted
	 * changes, they will be written in the checkpoint and may appear after
	 * a crash.
	 *
	 * Use a real snapshot transaction: we don't want any chance of the
	 * snapshot being updated during the checkpoint.  Eviction is prevented
	 * from evicting anything newer than this because we track the oldest
	 * transaction ID in the system that is not visible to all readers.
	 */
	if (F_ISSET(txn, TXN_RUNNING))
		WT_ERR_MSG(session, EINVAL,
		    "Checkpoint not permitted in a transaction");

	/*
	 * Reset open cursors.  Do this explicitly, even though it will happen
	 * implicitly in the call to begin_transaction for the checkpoint, the
	 * checkpoint code will acquire the schema lock before we do that, and
	 * some implementation of WT_CURSOR::reset might need the schema lock.
	 */
	WT_ERR(__wt_session_reset_cursors(session));

	/*
	 * Only one checkpoint can be active at a time, and checkpoints must run
	 * in the same order as they update the metadata.  It's probably a bad
	 * idea to run checkpoints out of multiple threads, but serialize them
	 * here to ensure we don't get into trouble.
	 */
	WT_STAT_FAST_CONN_SET(session, txn_checkpoint_running, 1);
	__wt_spin_lock(session, &S2C(session)->checkpoint_lock);

	ret = __wt_txn_checkpoint(session, cfg);

	WT_STAT_FAST_CONN_SET(session, txn_checkpoint_running, 0);
	__wt_spin_unlock(session, &S2C(session)->checkpoint_lock);

err:	F_CLR(session, WT_SESSION_NO_CACHE_CHECK);
	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_open_session --
 *	Allocate a session handle.  The internal parameter is used for sessions
 *	opened by WiredTiger for its own use.
 */
int
__wt_open_session(WT_CONNECTION_IMPL *conn, int internal,
    WT_EVENT_HANDLER *event_handler, const char *config,
    WT_SESSION_IMPL **sessionp)
{
	static const WT_SESSION stds = {
		NULL,
		__session_close,
		__session_reconfigure,
		__session_open_cursor,
		__session_create,
		__session_compact,
		__session_drop,
		__session_log_printf,
		__session_rename,
		__session_salvage,
		__session_truncate,
		__session_upgrade,
		__session_verify,
		__session_begin_transaction,
		__session_commit_transaction,
		__session_rollback_transaction,
		__session_checkpoint
	};
	WT_DECL_RET;
	WT_SESSION_IMPL *session, *session_ret;
	uint32_t i;

	*sessionp = NULL;

	session = conn->default_session;
	session_ret = NULL;

	__wt_spin_lock(session, &conn->api_lock);

	/* Find the first inactive session slot. */
	for (session_ret = conn->sessions,
	    i = 0; i < conn->session_size; ++session_ret, ++i)
		if (!session_ret->active)
			break;
	if (i == conn->session_size)
		WT_ERR_MSG(session, WT_ERROR,
		    "only configured to support %" PRIu32 " thread contexts",
		    conn->session_size);

	/*
	 * If the active session count is increasing, update it.  We don't worry
	 * about correcting the session count on error, as long as we don't mark
	 * this session as active, we'll clean it up on close.
	 */
	if (i >= conn->session_cnt)	/* Defend against off-by-one errors. */
		conn->session_cnt = i + 1;

	session_ret->id = i;
	session_ret->iface = stds;
	session_ret->iface.connection = &conn->iface;

	WT_ERR(__wt_cond_alloc(session, "session", 0, &session_ret->cond));

	__wt_event_handler_set(session_ret,
	    event_handler == NULL ? session->event_handler : event_handler);

	TAILQ_INIT(&session_ret->cursors);
	SLIST_INIT(&session_ret->dhandles);

	/* Initialize transaction support: default to read-committed. */
	session_ret->isolation = TXN_ISO_READ_COMMITTED;
	WT_ERR(__wt_txn_init(session_ret));

	/*
	 * The session's hazard pointer memory isn't discarded during normal
	 * session close because access to it isn't serialized.  Allocate the
	 * first time we open this session.
	 */
	if (session_ret->hazard == NULL)
		WT_ERR(__wt_calloc_def(
		    session, conn->hazard_max, &session_ret->hazard));

	/*
	 * Set an initial size for the hazard array. It will be grown as
	 * required up to hazard_max. The hazard_size is reset on close, since
	 * __wt_hazard_close ensures the array is cleared - so it is safe to
	 * reset the starting size on each open.
	 */
	session_ret->hazard_size = WT_HAZARD_INCR;

	/*
	 * Public sessions are automatically closed during WT_CONNECTION->close.
	 * If the session handles for internal threads were to go on the public
	 * list, there would be complex ordering issues during close.  Set a
	 * flag to avoid this: internal sessions are not closed automatically.
	 */
	if (internal)
		F_SET(session_ret, WT_SESSION_INTERNAL);

	/*
	 * Configuration: currently, the configuration for open_session is the
	 * same as session.reconfigure, so use that function.
	 */
	if (config != NULL)
		WT_ERR(
		    __session_reconfigure((WT_SESSION *)session_ret, config));

	/*
	 * Publish: make the entry visible to server threads.  There must be a
	 * barrier for two reasons, to ensure structure fields are set before
	 * any other thread will consider the session, and to push the session
	 * count to ensure the eviction thread can't review too few slots.
	 */
	WT_PUBLISH(session_ret->active, 1);

	STATIC_ASSERT(offsetof(WT_SESSION_IMPL, iface) == 0);
	*sessionp = session_ret;

err:	__wt_spin_unlock(session, &conn->api_lock);
	return (ret);
}
