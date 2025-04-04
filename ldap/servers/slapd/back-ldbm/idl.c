/** BEGIN COPYRIGHT BLOCK
 * Copyright (C) 2001 Sun Microsystems, Inc. Used by permission.
 * Copyright (C) 2005 Red Hat, Inc.
 * All rights reserved.
 *
 * License: GPL (version 3 or any later version).
 * See LICENSE for details.
 * END COPYRIGHT BLOCK **/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* idl.c - ldap id list handling routines */

#include "back-ldbm.h"
#include "dblayer.h"

/*
 * Disable idl locking since it causes unbreakable deadlock.
 */
#undef IDL_LOCKING_ENABLE

static void make_cont_key(dbi_val_t *contkey, dbi_val_t *key, ID id);
static int idl_insert_maxids(IDList **idl, ID id, int maxids);

/* for the cache of open index files */
struct idl_private
{
    int idl_maxids;         /* Number of IDS in a block */
    int idl_maxindirect;    /* Number of blocks allowed */
    size_t idl_allidslimit; /* Max number of IDs before it turns to allids */
#ifdef IDL_LOCKING_ENABLE
    Slapi_RWLock *idl_rwlock;
#endif
};

static int idl_tune = DEFAULT_IDL_TUNE; /* tuning parameters for IDL code */
#define IDL_TUNE_BSEARCH 1              /* do a binary search when inserting into an IDL */
#define IDL_TUNE_NOPAD 2                /* Don't pad IDLs with space at the end */

/* if still needed, need to find a solution
 * just moved here to clenaup dblayer
 */

static int32_t
idl_old_get_optimal_block_size(backend *be)
{
    dblayer_private *priv = NULL;
    uint32_t *page_size = NULL;
    PR_ASSERT(NULL != be);

    struct ldbminfo *li = (struct ldbminfo *)be->be_database->plg_private;
    priv = (dblayer_private *)li->li_dblayer_private;
    PR_ASSERT(NULL != priv);

    priv->dblayer_get_info_fn(be, BACK_INFO_DB_PAGESIZE, (void **)&page_size);
    if (priv->dblayer_idl_divisor == 0) {
        return *page_size - DB_EXTN_PAGE_HEADER_SIZE;
    } else {
        return *page_size / priv->dblayer_idl_divisor;
    }
}


void
idl_old_set_tune(int val)
{
    idl_tune = val;
}

int
idl_old_get_tune(void)
{
    return idl_tune;
}

size_t
idl_old_get_allidslimit(struct attrinfo *a)
{
    idl_private *priv = NULL;

    PR_ASSERT(NULL != a);
    PR_ASSERT(NULL != a->ai_idl);

    priv = a->ai_idl;

    return priv->idl_allidslimit;
}

static void
idl_init_maxids(backend *be, idl_private *priv)
{
    struct ldbminfo *li = (struct ldbminfo *)be->be_database->plg_private;
    const size_t blksize = idl_old_get_optimal_block_size(be);

    if (0 == li->li_allidsthreshold) {
        li->li_allidsthreshold = DEFAULT_ALLIDSTHRESHOLD;
    }
    if (li->li_old_idl_maxids) {
        priv->idl_maxids = li->li_old_idl_maxids;
    } else {
        priv->idl_maxids = (blksize / sizeof(ID)) - 2;
    }
    priv->idl_maxindirect = (li->li_allidsthreshold / priv->idl_maxids) + 1;
    priv->idl_allidslimit = (priv->idl_maxids * priv->idl_maxindirect);
    slapi_log_err(SLAPI_LOG_ARGS,
                  "idl_init_maxids", "blksize %lu, maxids %i, maxindirect %i\n",
                  (unsigned long)blksize, priv->idl_maxids, priv->idl_maxindirect);
}

/* routine to initialize the private data used by the IDL code per-attribute */
int
idl_old_init_private(backend *be __attribute__((unused)), struct attrinfo *a)
{
    idl_private *priv = NULL;

    PR_ASSERT(NULL != a);
    PR_ASSERT(NULL == a->ai_idl);

    priv = (idl_private *)slapi_ch_malloc(sizeof(idl_private));
    if (NULL == priv) {
        return -1; /* Memory allocation failure */
    }
    {
        priv->idl_maxids = 0;
        priv->idl_maxindirect = 0;
    }
#ifdef IDL_LOCKING_ENABLE
    priv->idl_rwlock = slapi_new_rwlock();

    if (NULL == priv->idl_rwlock) {
        slapi_ch_free((void **)&priv);
        return -1;
    }
#endif
    a->ai_idl = (void *)priv;
    return 0;
}

/* routine to release resources used by IDL private data structure */
int
idl_old_release_private(struct attrinfo *a)
{
    PR_ASSERT(NULL != a);
    if (NULL != a->ai_idl) {
#ifdef IDL_LOCKING_ENABLE
        idl_private *priv = a->ai_idl;
        PR_ASSERT(NULL != priv->idl_rwlock);
        slapi_destroy_rwlock(priv->idl_rwlock);
#endif
        slapi_ch_free((void **)&(a->ai_idl));
    }
    return 0;
}

/* Locks one IDL so we can modify it knowing that
 * nobody else is trying to do so at the same time
 * also called by readers, since they need to be blocked
 * when they read to avoid them seeing inconsistent data
 * This is not really necessary for update operations
 * today because they are already serialized by a lock
 * at the backend level but is still necessary to
 * stop concurrent access by one update thread and
 * some other search threads
 */

#ifdef IDL_LOCKING_ENABLE
static void
idl_Wlock_list(idl_private *priv, dbi_val_t *key)
{
    Slapi_RWLock *lock = NULL;

    PR_ASSERT(NULL != priv);
    lock = priv->idl_rwlock;
    PR_ASSERT(NULL != lock);

    slapi_rwlock_wrlock(lock);
}

static void
idl_Rlock_list(idl_private *priv, dbi_val_t *key)
{
    Slapi_RWLock *lock = NULL;

    PR_ASSERT(NULL != priv);
    lock = priv->idl_rwlock;
    PR_ASSERT(NULL != lock);

    slapi_rwlock_rdlock(lock);
}

static void
idl_unlock_list(idl_private *priv, dbi_val_t *key)
{
    Slapi_RWLock *lock = NULL;

    PR_ASSERT(NULL != priv);
    lock = priv->idl_rwlock;
    PR_ASSERT(NULL != lock);

    slapi_rwlock_unlock(lock);
}
#endif

#ifndef IDL_LOCKING_ENABLE
#define idl_Wlock_list(idl, dbt)
#define idl_Rlock_list(idl, dbt)
#define idl_unlock_list(idl, dbt)
#endif

/*
 * idl_fetch_one - fetch a single IDList from the database and return a
 *     pointer to it.
 *
 * this routine always propagates errors other than DBI_RC_RETRY.
 * for DBI_RC_RETRY, it propagates the error if called inside a
 * transaction. if called not inside a transaction, it loops on
 * DBI_RC_RETRY, retrying the fetch.
 *
 */
static IDList *
idl_fetch_one(
    backend *be,
    dbi_db_t *db,
    dbi_val_t *key,
    dbi_txn_t *txn,
    int *err)
{
    dbi_val_t data = {0};
    IDList *idl = NULL;
    dblayer_value_init(be, &data);

    do {
        *err = dblayer_db_op(be, db, txn, DBI_OP_GET, key, &data);
        if (0 != *err && DBI_RC_NOTFOUND != *err && DBI_RC_RETRY != *err) {
            const char *msg;
            if (EPERM == *err && *err != errno) {
                slapi_log_err(SLAPI_LOG_ERR,
                              "idl_fetch_one", "(%s) Database failed to run, "
                                               "There is either insufficient disk space or "
                                               "insufficient memory available for database.\n",
                              ((char *)key->dptr)[key->dsize - 1] ? "" : (char *)key->dptr);
            } else {
                slapi_log_err(SLAPI_LOG_ERR,
                              "idl_fetch_one", "Error %d %s\n",
                              *err, (msg = dblayer_strerror(*err)) ? msg : "");
            }
        }
    } while (DBI_RC_RETRY == *err && NULL == txn);

    if (0 == *err) {
        idl = (IDList *)data.data;
    }

    return (idl);
}

IDList *
idl_old_fetch(
    backend *be,
    dbi_db_t *db,
    dbi_val_t *key,
    dbi_txn_t *txn,
    struct attrinfo *a __attribute__((unused)),
    int *err)
{
    struct ldbminfo *li = (struct ldbminfo *)be->be_database->plg_private;
    dbi_val_t k2 = {0};
    IDList *idl;
    IDList **tmp;
    back_txn s_txn;
    char *kstr;
    int i;
    unsigned long nids;

    /* slapi_log_err(SLAPI_LOG_TRACE, "=> idl_fetch\n", 0, 0, 0 ); */
    if ((idl = idl_fetch_one(be, db, key, txn, err)) == NULL) {
        return (NULL);
    }

    /* regular block */
    if (!INDIRECT_BLOCK(idl)) {
        /* make sure we have the current value of highest id */
        if (ALLIDS(idl)) {
            idl_free(&idl);
            idl = idl_allids(be);
        }
        return (idl);
    }
    idl_free(&idl);

    /* Taking a transaction is expensive; so we try and optimize for the common case by not
       taking one above. If we have a indirect block; we need to take a transaction and re-read
       the idl since they could have been changed by another thread after we read the first block
       above */

    dblayer_txn_init(li, &s_txn);
    if (NULL != txn) {
        dblayer_read_txn_begin(be, txn, &s_txn);
    }
    if ((idl = idl_fetch_one(be, db, key, s_txn.back_txn_txn, err)) == NULL) {
        dblayer_read_txn_commit(be, &s_txn);
        return (NULL);
    }

    /* regular block */
    if (!INDIRECT_BLOCK(idl)) {
        dblayer_read_txn_commit(be, &s_txn);
        /* make sure we have the current value of highest id */
        if (ALLIDS(idl)) {
            idl_free(&idl);
            idl = idl_allids(be);
        }
        return (idl);
    }
    /*
     * this is an indirect block which points to other blocks.
     * we need to read in all the blocks it points to and construct
     * a big id list containing all the ids, which we will return.
     */

    /* count the number of blocks & allocate space for pointers to them */
    for (i = 0; idl->b_ids[i] != NOID; i++)
        ; /* NULL */
    tmp = (IDList **)slapi_ch_malloc((i + 1) * sizeof(IDList *));

    /* read in all the blocks */
    kstr = (char *)slapi_ch_malloc(key->dsize + 20);
    nids = 0;
    for (i = 0; idl->b_ids[i] != NOID; i++) {
        ID thisID = idl->b_ids[i];
        ID nextID = idl->b_ids[i + 1];

        sprintf(kstr, "%c%s%lu", CONT_PREFIX, (char *)key->dptr, (u_long)thisID);
        k2.dptr = kstr;
        k2.dsize = strlen(kstr) + 1;

        if ((tmp[i] = idl_fetch_one(be, db, &k2, s_txn.back_txn_txn, err)) == NULL) {
            if (*err == DBI_RC_RETRY) {
                dblayer_read_txn_abort(be, &s_txn);
            } else {
                dblayer_read_txn_commit(be, &s_txn);
            }
            slapi_ch_free((void **)&kstr);
            slapi_ch_free((void **)&tmp);
            return (NULL);
        }

        nids += tmp[i]->b_nids;

        /* Check for inconsistencies: */
        if (tmp[i]->b_ids[0] != thisID) {
            slapi_log_err(SLAPI_LOG_WARNING, "idl_old_fetch", "(%s)->b_ids[0] == %lu\n",
                          (char *)k2.dptr, (u_long)tmp[i]->b_ids[0]);
        }
        if (nextID != NOID) {
            if (nextID <= thisID) {
                slapi_log_err(SLAPI_LOG_WARNING, "idl_old_fetch",
                              "Indirect block (%s) contains %lu, %lu\n",
                              (char *)key->dptr, (u_long)thisID, (u_long)nextID);
            }
            if (nextID <= tmp[i]->b_ids[(tmp[i]->b_nids) - 1]) {
                slapi_log_err(SLAPI_LOG_WARNING, "idl_old_fetch", "(%s)->b_ids[last] == %lu"
                                                                  " >= %lu (next indirect ID)\n",
                              (char *)k2.dptr, (u_long)tmp[i]->b_ids[(tmp[i]->b_nids) - 1], (u_long)nextID);
            }
        }
    }
    dblayer_read_txn_commit(be, &s_txn);
    tmp[i] = NULL;
    slapi_ch_free((void **)&kstr);
    idl_free(&idl);

    /* allocate space for the big block */
    idl = idl_alloc(nids);
    idl->b_nids = nids;
    nids = 0;

    /* copy in all the ids from the component blocks */
    for (i = 0; tmp[i] != NULL; i++) {
        if (tmp[i] == NULL) {
            continue;
        }

        SAFEMEMCPY((char *)&idl->b_ids[nids], (char *)tmp[i]->b_ids,
                   tmp[i]->b_nids * sizeof(ID));
        nids += tmp[i]->b_nids;

        idl_free(&tmp[i]);
    }
    slapi_ch_free((void **)&tmp);

    slapi_log_err(SLAPI_LOG_TRACE, "idl_old_fetch", "<= %lu ids (%lu max)\n", (u_long)idl->b_nids,
                  (u_long)idl->b_nmax);
    return (idl);
}

static int
idl_store(
    backend *be __attribute__((unused)),
    dbi_db_t *db,
    dbi_val_t *key,
    IDList *idl,
    dbi_txn_t *txn)
{
    int rc;
    dbi_val_t data = {0};

    /* slapi_log_err(SLAPI_LOG_TRACE, "=> idl_store\n", 0, 0, 0 ); */

    data.dptr = (char *)idl;
    data.dsize = (2 + idl->b_nmax) * sizeof(ID);

    rc = dblayer_db_op(be, db, txn, DBI_OP_PUT, key, &data);
    if (0 != rc) {
        const char *msg;
        if (EPERM == rc && rc != errno) {
            slapi_log_err(SLAPI_LOG_ERR,
                          "idl_store", "(%s) Database failed to run, "
                                       "There is insufficient memory available for database.\n",
                          ((char *)key->dptr)[key->dsize - 1] ? "" : (char *)key->dptr);
        } else {
            if (LDBM_OS_ERR_IS_DISKFULL(rc)) {
                operation_out_of_disk_space();
            }
            slapi_log_err(((DBI_RC_RETRY == rc) ? SLAPI_LOG_TRACE : SLAPI_LOG_ERR),
                          "idl_store", "(%s) Returns %d %s\n",
                          ((char *)key->dptr)[key->dsize - 1] ? "" : (char *)key->dptr,
                          rc, (msg = dblayer_strerror(rc)) ? msg : "");
            if (rc == DBI_RC_RUNRECOVERY) {
                slapi_log_err(SLAPI_LOG_WARNING, "idl_store",
                              "Failures can be an indication of insufficient disk space.\n");
                ldbm_nasty("idl_store", "db->put", 71, rc);
            }
        }
    }

    /* slapi_log_err(SLAPI_LOG_TRACE, "<= idl_store %d\n", rc, 0, 0 ); */
    return (rc);
}

static void
idl_split_block(
    IDList *b,
    ID id,
    IDList **n1,
    IDList **n2)
{
    ID i;

    /* find where to split the block */
    for (i = 0; i < b->b_nids && id > b->b_ids[i]; i++)
        ; /* NULL */

    *n1 = idl_alloc(i == 0 ? 1 : i);
    *n2 = idl_alloc(b->b_nids - i + (i == 0 ? 0 : 1));

    /*
     * everything before the id being inserted in the first block
     * unless there is nothing, in which case the id being inserted
     * goes there.
     */
    SAFEMEMCPY((char *)&(*n1)->b_ids[0], (char *)&b->b_ids[0],
               i * sizeof(ID));
    (*n1)->b_nids = (i == 0 ? 1 : i);

    if (i == 0) {
        (*n1)->b_ids[0] = id;
    } else {
        (*n2)->b_ids[0] = id;
    }

    /* the id being inserted & everything after in the second block */
    SAFEMEMCPY((char *)&(*n2)->b_ids[i == 0 ? 0 : 1],
               (char *)&b->b_ids[i], (b->b_nids - i) * sizeof(ID));
    (*n2)->b_nids = b->b_nids - i + (i == 0 ? 0 : 1);
}

/*
 * idl_change_first - called when an indirect block's first key has
 * changed, meaning it needs to be stored under a new key, and the
 * header block pointing to it needs updating.
 */

static int
idl_change_first(
    backend *be,
    dbi_db_t *db,
    dbi_val_t *hkey, /* header block key    */
    IDList *h, /* header block     */
    int pos,   /* pos in h to update    */
    dbi_val_t *bkey, /* data block key    */
    IDList *b, /* data block         */
    dbi_txn_t *txn)
{
    int rc;
    const char *msg;

    /* delete old key block */
    rc = dblayer_db_op(be, db, txn, DBI_OP_DEL, bkey, 0);
    if ((rc != 0) && (DBI_RC_RETRY != rc)) {
        slapi_log_err(SLAPI_LOG_ERR, "idl_change_first", "del (%s) err %d %s\n",
                      (char *)bkey->dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
        if (rc == DBI_RC_RUNRECOVERY) {
            ldbm_nasty("idl_change_first", "db->del", 72, rc);
        }
        return (rc);
    }

    /* write block with new key */
    sprintf(bkey->dptr, "%c%s%lu", CONT_PREFIX, (char *)hkey->dptr, (u_long)b->b_ids[0]);
    bkey->dsize = strlen(bkey->dptr) + 1;
    if ((rc = idl_store(be, db, bkey, b, txn)) != 0) {
        return (rc);
    }

    /* update + write indirect header block */
    h->b_ids[pos] = b->b_ids[0];
    if ((rc = idl_store(be, db, hkey, h, txn)) != 0) {
        return (rc);
    }

    return (0);
}


#define IDL_CHECK_FAILED(FORMAT, ARG1, ARG2)                                                         \
    do {                                                                                             \
        char *fmt = slapi_ch_malloc(strlen(func) + strlen(note) + strlen(FORMAT) + 30);              \
        if (fmt != NULL) {                                                                           \
            sprintf(fmt, "IDL_CHECK_FAILED - %s(%%s,%lu) %s: %s\n", func, (u_long)id, note, FORMAT); \
            slapi_log_err(SLAPI_LOG_ERR, fmt, key->dptr, ARG1, ARG2);                                \
            slapi_ch_free((void **)&fmt);                                                            \
        }                                                                                            \
    } while (0)


static void
idl_check_indirect(IDList *idl, int i, IDList *tmp, IDList *tmp2, char *func, char *note, dbi_val_t *key, ID id)
/* Check for inconsistencies
    The caller alleges that *idl is a header block, in which the
    i'th item points to the indirect block *tmp, and either tmp2 == NULL
    or *tmp2 is the indirect block to which the i+1'th item in *idl points.
    The other parameters are merely output in each error message, like:
    printf ("%s(%s,%lu) %s: ...", func, key->dptr, (u_long)id, note, ...)
      */
{
    /* The implementation is optimized for no inconsistencies. */
    const ID thisID = idl->b_ids[i];
    const ID nextID = idl->b_ids[i + 1];
    const ID tmp0 = tmp->b_ids[0];
    const ID tmpLast = tmp->b_ids[tmp->b_nids - 1];

    if (tmp0 != thisID) {
        IDL_CHECK_FAILED("tmp->b_ids[0] == %lu, not %lu\n",
                         (u_long)tmp0, (u_long)thisID);
    }
    if (tmp0 > tmpLast) {
        IDL_CHECK_FAILED("tmp->b_ids[0] == %lu > %lu [last]\n",
                         (u_long)tmp0, (u_long)tmpLast);
    }
    if (nextID == NOID) {
        if (tmp2 != NULL) {
            IDL_CHECK_FAILED("idl->b_ids[%i+1] == NOID, but tmp2 != NULL\n", i, 0);
        }
    } else {
        if (nextID <= thisID) {
            IDL_CHECK_FAILED("idl->b_ids contains %lu, %lu\n", (u_long)thisID, (u_long)nextID);
        }
        if (nextID <= tmpLast) {
            IDL_CHECK_FAILED("idl->b_ids[i+1] == %lu <= %lu (last of idl->b_ids[i])\n",
                             (u_long)nextID, (u_long)tmpLast);
        }
        if (tmp2 != NULL && tmp2->b_ids[0] != nextID) {
            IDL_CHECK_FAILED("tmp2->b_ids[0] == %lu, not %lu\n",
                             (u_long)tmp2->b_ids[0], (u_long)nextID);
        }
    }
}


int
idl_old_insert_key(
    backend *be,
    dbi_db_t *db,
    dbi_val_t *key,
    ID id,
    dbi_txn_t *txn,
    struct attrinfo *a,
    int *disposition)
{
    int i, j, rc = 0;
    const char *msg;
    IDList *idl, *tmp, *tmp2, *tmp3;
    char *kstr;
    dbi_val_t k2 = {0};
    dbi_val_t k3 = {0};

    if (NULL != disposition) {
        *disposition = IDL_INSERT_NORMAL;
    }

    if (0 == a->ai_idl->idl_maxids) {
        idl_init_maxids(be, a->ai_idl);
    }

    idl_Wlock_list(a->ai_idl, key);
    if ((idl = idl_fetch_one(be, db, key, txn, &rc)) == NULL) {
        if (rc != 0 && rc != DBI_RC_NOTFOUND) {
            if (rc != DBI_RC_RETRY) {
                slapi_log_err(SLAPI_LOG_ERR, "idl_old_insert_key", "0 BAD %d %s\n",
                              rc, (msg = dblayer_strerror(rc)) ? msg : "");
            }
            return (rc);
        }
        idl = idl_alloc(1);
        idl->b_ids[idl->b_nids++] = id;
        rc = idl_store(be, db, key, idl, txn);
        if (rc != 0 && rc != DBI_RC_RETRY) {
            slapi_log_err(SLAPI_LOG_ERR, "idl_old_insert_key", "1 BAD %d %s\n",
                          rc, (msg = dblayer_strerror(rc)) ? msg : "");
        }

        idl_free(&idl);
        idl_unlock_list(a->ai_idl, key);
        return (rc);
    }

    /* regular block */
    if (!INDIRECT_BLOCK(idl)) {
        switch (idl_insert_maxids(&idl, id, a->ai_idl->idl_maxids)) {
        case 0: /* id inserted - store the updated block */
        case 1:
            rc = idl_store(be, db, key, idl, txn);
            break;

        case 2: /* id already there - nothing to do */
            rc = 0;
            /* Could be an ALLID block, let's check */
            if (ALLIDS(idl)) {
                if (NULL != disposition) {
                    *disposition = IDL_INSERT_ALLIDS;
                }
            }
            break;

        case 3: /* id not inserted - block must be split */
            /* check threshold for marking this an all-id block */
            if (a->ai_idl->idl_maxindirect < 2) {
                idl_free(&idl);
                idl = idl_allids(be);
                rc = idl_store(be, db, key, idl, txn);
                idl_free(&idl);

                idl_unlock_list(a->ai_idl, key);
                if (rc != 0 && rc != DBI_RC_RETRY) {
                    slapi_log_err(SLAPI_LOG_ERR, "idl_old_insert_key", "2 BAD %d %s\n",
                                  rc, (msg = dblayer_strerror(rc)) ? msg : "");
                }
                if (NULL != disposition) {
                    *disposition = IDL_INSERT_NOW_ALLIDS;
                }
                return (rc);
            }

            idl_split_block(idl, id, &tmp, &tmp2);
            idl_free(&idl);

            /* create the header indirect block */
            idl = idl_alloc(3);
            idl->b_nmax = 3;
            idl->b_nids = INDBLOCK;
            idl->b_ids[0] = tmp->b_ids[0];
            idl->b_ids[1] = tmp2->b_ids[0];
            idl->b_ids[2] = NOID;

            /* store it */
            rc = idl_store(be, db, key, idl, txn);
            if (rc != 0) {
                idl_free(&idl);
                idl_free(&tmp);
                idl_free(&tmp2);
                if (rc != DBI_RC_RETRY) {
                    slapi_log_err(SLAPI_LOG_ERR, "idl_old_insert_key", "3 BAD %d %s\n",
                                  rc, (msg = dblayer_strerror(rc)) ? msg : "");
                }
                return (rc);
            }

            /* store the first id block */
            kstr = (char *)slapi_ch_malloc(key->dsize + 20);
            sprintf(kstr, "%c%s%lu", CONT_PREFIX, (char *)key->dptr,
                    (u_long)tmp->b_ids[0]);
            k2.dptr = kstr;
            k2.dsize = strlen(kstr) + 1;
            rc = idl_store(be, db, &k2, tmp, txn);

            /* store the second id block */
            sprintf(kstr, "%c%s%lu", CONT_PREFIX, (char *)key->dptr,
                    (u_long)tmp2->b_ids[0]);
            k2.dptr = kstr;
            k2.dsize = strlen(kstr) + 1;
            rc = idl_store(be, db, &k2, tmp2, txn);
            if (rc != 0) {
                idl_free(&idl);
                idl_free(&tmp);
                idl_free(&tmp2);
                if (rc != DBI_RC_RETRY) {
                    slapi_log_err(SLAPI_LOG_ERR, "idl_old_insert_key", "4 BAD %d %s\n",
                                  rc, (msg = dblayer_strerror(rc)) ? msg : "");
                }
                return (rc);
            }
            idl_check_indirect(idl, 0, tmp, tmp2,
                               "idl_insert_key", "split", key, id);

            slapi_ch_free((void **)&kstr);
            idl_free(&tmp);
            idl_free(&tmp2);
            break;
        }

        idl_free(&idl);
        idl_unlock_list(a->ai_idl, key);
        if (rc != 0 && rc != DBI_RC_RETRY) {
            slapi_log_err(SLAPI_LOG_ERR, "idl_old_insert_key", "5 BAD %d %s\n",
                          rc, (msg = dblayer_strerror(rc)) ? msg : "");
        }
        return (rc);
    }

    /*
     * this is an indirect block which points to other blocks.
     * we need to read in the block into which the id should be
     * inserted, then insert the id and store the block.  we might
     * have to split the block if it is full, which means we also
     * need to write a new "header" block.
     */

    /* select the block to try inserting into */
    for (i = 0; idl->b_ids[i] != NOID && id > idl->b_ids[i]; i++)
        ;                      /* NULL */
    if (id == idl->b_ids[i]) { /* already in a block */
#ifdef _DEBUG_LARGE_BLOCKS
        slapi_log_err(SLAPI_LOG_DEBUG,
                      "idl_old_insert_key", "id %lu for key (%s) is already in block %d\n",
                      (u_long)id, key.dptr, i);
#endif
        idl_unlock_list(a->ai_idl, key);
        idl_free(&idl);
        return (0);
    }
    if (i != 0) {
        i--;
    }

    /* get the block */
    kstr = (char *)slapi_ch_malloc(key->dsize + 20);
    sprintf(kstr, "%c%s%lu", CONT_PREFIX, (char *)key->dptr, (u_long)idl->b_ids[i]);
    k2.dptr = kstr;
    k2.dsize = strlen(kstr) + 1;
    if ((tmp = idl_fetch_one(be, db, &k2, txn, &rc)) == NULL) {
        if (rc != 0) {
            if (rc != DBI_RC_RETRY) {
                slapi_log_err(SLAPI_LOG_ERR, "idl_old_insert_key", "6 BAD %d %s\n",
                              rc, (msg = dblayer_strerror(rc)) ? msg : "");
            }
            return (rc);
        }
        slapi_log_err(SLAPI_LOG_ERR,
                      "idl_old_insert_key", "nonexistent continuation block (%s)\n", (char *)k2.dptr);
        idl_unlock_list(a->ai_idl, key);
        idl_free(&idl);
        slapi_ch_free((void **)&kstr);
        return (-1);
    }

    /* insert the id */
    switch (idl_insert_maxids(&tmp, id, a->ai_idl->idl_maxids)) {
    case 0: /* id inserted ok */
        rc = idl_store(be, db, &k2, tmp, txn);
        if (0 != rc) {
            idl_check_indirect(idl, i, tmp, NULL,
                               "idl_insert_key", "indirect", key, id);
        }
        break;

    case 1: /* id inserted - first id in block has changed */
        /*
         * key for this block has changed, so we have to
         * write the block under the new key, delete the
         * old key block + update and write the indirect
         * header block.
         */

        rc = idl_change_first(be, db, key, idl, i, &k2, tmp, txn);
        if (rc != 0) {
            break; /* return error in rc */
        }
        idl_check_indirect(idl, i, tmp, NULL,
                           "idl_insert_key", "indirect 1", key, id);
        break;

    case 2: /* id not inserted - already there */
        idl_check_indirect(idl, i, tmp, NULL,
                           "idl_insert_key", "indirect no change", key, id);
        break;

    case 3: /* id not inserted - block is full */
        /*
         * first, see if we can shift ids down one, moving
         * the last id in the current block to the next
         * block, and then adding the id we are inserting to
         * the current block. we'll need to split the block
         * otherwise.
         */

        /* is there a next block? */
        if (idl->b_ids[i + 1] != NOID) {
            char *kstr3 = (char *)slapi_ch_malloc(key->dsize + 20);
            /* yes - read it in */
            sprintf(kstr3, "%c%s%lu", CONT_PREFIX, (char *)key->dptr,
                    (u_long)idl->b_ids[i + 1]);
            k3.dptr = kstr3;
            k3.dsize = strlen(kstr3) + 1;
            if ((tmp2 = idl_fetch_one(be, db, &k3, txn, &rc)) == NULL) {
                if (rc != DBI_RC_RETRY) {
                    slapi_log_err(SLAPI_LOG_ERR,
                                  "idl_old_insert_key", "(%s) returns NULL\n", (char *)k3.dptr);
                }
                if (0 != rc) {
                    idl_check_indirect(idl, i, tmp, NULL,
                                       "idl_insert_key", "indirect missing", key, id);
                }
                break;
            }

            /*
             * insert the last key in the previous block in
             * the next block. it should go at the beginning
             * always, if it fits at all.
             */
            rc = idl_insert_maxids(&tmp2,
                                   id > tmp->b_ids[tmp->b_nids - 1] ? id : tmp->b_ids[tmp->b_nids - 1],
                                   a->ai_idl->idl_maxids);
            switch (rc) {
            case 1: /* id inserted first in block */
                rc = idl_change_first(be, db, key, idl,
                                      i + 1, &k3, tmp2, txn);
                if (rc != 0) {
                    break; /* return error in rc */
                }

                if (id < tmp->b_ids[tmp->b_nids - 1]) {
                    /*
                     * we inserted the last id in the previous
                     * block in this block. we need to "remove"
                     * it from the previous block and insert the
                     * new id. decrementing the b_nids count
                     * in the previous block has the effect
                     * of removing the last id.
                     */

                    /* remove last id in previous block */
                    tmp->b_nids--;

                    /* insert new id in previous block */
                    switch ((rc = idl_insert_maxids(&tmp, id,
                                                    a->ai_idl->idl_maxids))) {
                    case 0: /* id inserted */
                        rc = idl_store(be, db, &k2, tmp, txn);
                        break;
                    case 1: /* first in block */
                        rc = idl_change_first(be, db, key, idl,
                                              i, &k2, tmp, txn);
                        break;
                    case 2: /* already there - how? */
                    case 3: /* split block - how? */
                        slapi_log_err(SLAPI_LOG_ERR,
                                      "idl_old_insert_key", "Not expecting (%d) from idl_insert_maxids "
                                                            "of %lu in (%s).  Likely database corruption\n",
                                      rc, (u_long)id, (char *)k2.dptr);
                        rc = 0;
                        break;
                    }
                }
                if (rc != 0) {
                    break; /* return error in rc */
                }
                idl_check_indirect(idl, i, tmp, tmp2,
                                   "idl_insert_key", "overflow", key, id);

                slapi_ch_free((void **)&(k2.dptr));
                slapi_ch_free((void **)&(k3.dptr));
                idl_free(&tmp);
                idl_free(&tmp2);
                idl_free(&idl);
                idl_unlock_list(a->ai_idl, key);
                return (rc);

            case 0: /* id inserted not at start - how? */
            case 2: /* id already there - how? */
                /*
                 * if either of these cases happen, this
                 * index entry must have been corrupt when
                 * we started this insert. what can we do
                 * aside from log a warning?
                 */
                slapi_log_err(SLAPI_LOG_ERR,
                              "idl_old_insert_key", "Not expecting return %d from idl_insert_maxids "
                                                    "of id %lu in block with key (%s).  Likely database corruption\n",
                              rc, (u_long)tmp->b_ids[tmp->b_nids - 1], (char *)k3.dptr);
            /* FALL */
            case 3: /* block is full */
                /*
                 * if this case happens, we fall back to
                 * splitting the original block.
                                 * This is not an error condition. So set
                                 * rc = 0 to continue. Otherwise, it will break
                                 * from the case statement and return rc=3,
                                 * which is not correct.
                                 */
                rc = 0;
                idl_free(&tmp2);
                break;
            }
            if (rc != 0) {
                break; /* return error in rc */
            }
        }

        /*
         * must split the block, write both new blocks + update
         * and write the indirect header block.
         */

        /* count how many indirect blocks */
        for (j = 0; idl->b_ids[j] != NOID; j++)
            ; /* NULL */

        /* check it against all-id thresholed */
        if (j + 1 > a->ai_idl->idl_maxindirect) {
            /*
             * we've passed the all-id threshold, meaning
             * that this set of blocks should be replaced
             * by a single "all-id" block.  our job: delete
             * all the indirect blocks, and replace the header
             * block by an all-id block.
             */

            /* delete all indirect blocks */
            for (j = 0; idl->b_ids[j] != NOID; j++) {
                sprintf(kstr, "%c%s%lu", CONT_PREFIX, (char *)key->dptr,
                        (u_long)idl->b_ids[j]);
                k2.dptr = kstr;
                k2.dsize = strlen(kstr) + 1;

                rc = dblayer_db_op(be, db, txn, DBI_OP_DEL, &k2, 0);
                if (rc != 0) {
                    if (rc == DBI_RC_RUNRECOVERY) {
                        ldbm_nasty("idl_old_insert_key", "db->del", 73, rc);
                    }
                    break;
                }
            }

            /* store allid block in place of header block */
            if (0 == rc) {
                idl_free(&idl);
                idl = idl_allids(be);
                rc = idl_store(be, db, key, idl, txn);
                if (NULL != disposition) {
                    *disposition = IDL_INSERT_NOW_ALLIDS;
                }
            }

            slapi_ch_free((void **)&(k2.dptr));
            slapi_ch_free((void **)&(k3.dptr));
            idl_free(&idl);
            idl_free(&tmp);
            idl_unlock_list(a->ai_idl, key);
            return (rc);
        }

        idl_split_block(tmp, id, &tmp2, &tmp3);
        idl_free(&tmp);

        /* create a new updated indirect header block */
        tmp = idl_alloc(idl->b_nmax + 1);
        tmp->b_nids = INDBLOCK;
        /* everything up to the split block */
        SAFEMEMCPY((char *)tmp->b_ids, (char *)idl->b_ids,
                   i * sizeof(ID));
        /* the two new blocks */
        tmp->b_ids[i] = tmp2->b_ids[0];
        tmp->b_ids[i + 1] = tmp3->b_ids[0];
        /* everything after the split block */
        SAFEMEMCPY((char *)&tmp->b_ids[i + 2], (char *)&idl->b_ids[i + 1], (idl->b_nmax - i - 1) * sizeof(ID));

        /* store the header block */
        rc = idl_store(be, db, key, tmp, txn);
        if (rc != 0) {
            idl_free(&tmp2);
            idl_free(&tmp3);
            break;
        }

        /* store the first id block */
        sprintf(kstr, "%c%s%lu", CONT_PREFIX, (char *)key->dptr,
                (u_long)tmp2->b_ids[0]);
        k2.dptr = kstr;
        k2.dsize = strlen(kstr) + 1;
        rc = idl_store(be, db, &k2, tmp2, txn);
        if (rc != 0) {
            idl_free(&tmp2);
            idl_free(&tmp3);
            break;
        }

        /* store the second id block */
        sprintf(kstr, "%c%s%lu", CONT_PREFIX, (char *)key->dptr,
                (u_long)tmp3->b_ids[0]);
        k2.dptr = kstr;
        k2.dsize = strlen(kstr) + 1;
        rc = idl_store(be, db, &k2, tmp3, txn);
        if (rc != 0) {
            idl_free(&tmp2);
            idl_free(&tmp3);
            break;
        }

        idl_check_indirect(tmp, i, tmp2, tmp3,
                           "idl_insert_key", "indirect split", key, id);
        idl_free(&tmp2);
        idl_free(&tmp3);
        break;
    }

    slapi_ch_free((void **)&(k2.dptr));
    slapi_ch_free((void **)&(k3.dptr));
    idl_free(&tmp);
    idl_free(&idl);
    idl_unlock_list(a->ai_idl, key);
    return (rc);
}


/* Store a complete IDL all in one go, there must not be an existing key with the same value */
/* Routine used by merging import code */
int
idl_old_store_block(
    backend *be,
    dbi_db_t *db,
    dbi_val_t *key,
    IDList *idl,
    dbi_txn_t *txn,
    struct attrinfo *a)
{
    struct ldbminfo *li = (struct ldbminfo *)be->be_database->plg_private;
    int ret = 0;
    idl_private *priv = a->ai_idl;
    IDList *main_block = NULL;

    if (0 == a->ai_idl->idl_maxids) {
        idl_init_maxids(be, a->ai_idl);
    }

    /* First, is it an ALLIDS block ? */
    if (ALLIDS(idl)) {
        /* If so, we can store it as-is */
        ret = idl_store(be, db, key, idl, txn);
    } else {
        /* Next, is it a block with so many IDs in it that it _should_ be an ALLIDS block ? */
        if (idl->b_nids > (ID)li->li_allidsthreshold) {
            /* If so, store an ALLIDS block */
            IDList *all = idl_allids(be);
            ret = idl_store(be, db, key, all, txn);
            idl_free(&all);
        } else {
            /* Then , is it a block which is smaller than the size at which it needs splitting ? */
            if (idl->b_nids <= (ID)priv->idl_maxids) {
                /* If so, store as-is */
                ret = idl_store(be, db, key, idl, txn);
            } else {
                size_t number_of_ids = 0;
                size_t max_ids_in_block = 0;
                size_t number_of_cont_blks = 0;
                size_t i = 0;
                size_t number_of_ids_left = 0;
                size_t index = 0;
                dbi_val_t cont_key = {0};

                number_of_ids = idl->b_nids;
                max_ids_in_block = priv->idl_maxids;
                number_of_cont_blks = number_of_ids / max_ids_in_block;
                if (0 != number_of_ids % max_ids_in_block) {
                    number_of_cont_blks++;
                }
                number_of_ids_left = number_of_ids;
                /* Block needs splitting into continuation blocks */
                /* We need to make up a main block and n continuation blocks */
                /* Alloc main block */
                main_block = idl_alloc(number_of_cont_blks + 1);
                if (NULL == main_block) {
                    ret = -1;
                    goto done;
                }
                main_block->b_nids = INDBLOCK;
                main_block->b_ids[number_of_cont_blks] = NOID;
                /* Iterate over ids making the continuation blocks */
                for (i = 0; i < number_of_cont_blks; i++) {
                    IDList *this_cont_block = NULL;
                    size_t size_of_this_block = 0;
                    ID lead_id = NOID;
                    size_t j = 0;

                    lead_id = idl->b_ids[index];
                    if (number_of_ids_left >= max_ids_in_block) {
                        size_of_this_block = max_ids_in_block;
                    } else {
                        size_of_this_block = number_of_ids_left;
                    }
                    this_cont_block = idl_alloc(size_of_this_block);
                    if (NULL == this_cont_block) {
                        ret = -1;
                        goto done;
                    }
                    this_cont_block->b_nids = size_of_this_block;
                    /* Copy over the ids to the cont block we're making */
                    for (j = 0; j < size_of_this_block; j++) {
                        this_cont_block->b_ids[j] = idl->b_ids[index + j];
                    }
                    /* Make the continuation key */
                    make_cont_key(&cont_key, key, lead_id);
                    /* Now store the continuation block */
                    ret = idl_store(be, db, &cont_key, this_cont_block, txn);
                    idl_free(&this_cont_block);
                    dblayer_value_free(be, &cont_key);
                    if (ret != 0 && ret != DBI_RC_RETRY) {
                        slapi_log_err(SLAPI_LOG_ERR, "idl_old_store_block", "(%s) BAD %d %s\n",
                                      (char *)key->data, ret, dblayer_strerror(ret));
                        goto done;
                    }
                    /* Put the lead ID number in the header block */
                    main_block->b_ids[i] = lead_id;

                    /* Make our loop invariants correct */
                    number_of_ids_left -= size_of_this_block;
                    index += size_of_this_block;
                }
                PR_ASSERT(0 == number_of_ids_left);
                /* Now store the main block */
                ret = idl_store(be, db, key, main_block, txn);
            }
        }
    }
done:
    /* Free main block */
    idl_free(&main_block);
    return ret;
}

/*
 * idl_insert - insert an id into an id list.
 */
void
idl_insert(IDList **idl, ID id)
{
    ID i, j;
    NIDS nids;

    if ((*idl) == NULL) {
        (*idl) = idl_alloc(1);
        idl_append((*idl), id);
        return;
    }

    if (ALLIDS(*idl)) {
        return;
    }

    i = nids = (*idl)->b_nids;

    if (nids > 0) {
        /* optimize for a simple append */
        if (id == (*idl)->b_ids[nids - 1]) {
            return;
        } else if (id > (*idl)->b_ids[nids - 1]) {
            if (nids < (*idl)->b_nmax) {
                (*idl)->b_ids[nids] = id;
                (*idl)->b_nids++;
                return;
            }

            i = nids;

        } else if (id < (*idl)->b_ids[0]) {
            /* prepend */
            i = 0;
        } else {
            int lo = 0;
            int hi = (*idl)->b_nids - 1;
            int mid = 0;
            ID *ids = (*idl)->b_ids;

            if (0 != (*idl)->b_nids) {
                while (lo <= hi) {
                    mid = (hi + lo) >> 1;
                    if (ids[mid] > id) {
                        hi = mid - 1;
                    } else {
                        if (ids[mid] < id) {
                            lo = mid + 1;
                        } else {
                            /* Found it ! */
                            return;
                        }
                    }
                }
            }
            i = lo;
        }
    }

    /* do we need to make room for it? */
    if ((*idl)->b_nids == (*idl)->b_nmax) {
        (*idl)->b_nmax *= 2;

        (*idl) = (IDList *)slapi_ch_realloc((char *)(*idl),
                                            ((*idl)->b_nmax + 2) * sizeof(ID) + sizeof(IDList));
    }

    /* make a slot for the new id */
    for (j = (*idl)->b_nids; j != i; j--) {
        (*idl)->b_ids[j] = (*idl)->b_ids[j - 1];
    }

    (*idl)->b_ids[i] = id;
    (*idl)->b_nids++;

    memset((char *)&(*idl)->b_ids[(*idl)->b_nids], '\0',
           ((*idl)->b_nmax - (*idl)->b_nids) * sizeof(ID));

    return;
}

/*
 * idl_insert_maxids - insert an id into an id list.
 * returns    0    id inserted
 *        1    id inserted, first id in block has changed
 *        2    id not inserted, already there
 *        3    id not inserted, block must be split
 */

static int
idl_insert_maxids(IDList **idl, ID id, int maxids)
{
    ID i = 0, j = 0;
    NIDS nids;

    if (ALLIDS(*idl)) {
        return (2); /* already there */
    }

    nids = (*idl)->b_nids;

    if (nids > 0) {
        /* optimize for a simple append */
        if (id == (*idl)->b_ids[nids - 1]) {
            return (2);
        } else if (id > (*idl)->b_ids[nids - 1]) {
            if (nids < (*idl)->b_nmax) {
                (*idl)->b_ids[nids] = id;
                (*idl)->b_nids++;
                return 0;
            }

            i = nids;

        } else if (idl_tune & IDL_TUNE_BSEARCH) {
            int lo = 0;
            int hi = (*idl)->b_nids - 1;
            int mid = 0;
            ID *ids = (*idl)->b_ids;
            if (0 != (*idl)->b_nids) {
                while (lo <= hi) {
                    mid = (hi + lo) >> 1;
                    if (ids[mid] > id) {
                        hi = mid - 1;
                    } else {
                        if (ids[mid] < id) {
                            lo = mid + 1;
                        } else {
                            /* Found it ! */
                            return (2);
                        }
                    }
                }
            }
            i = lo;
        } else {
            /* is it already there? linear search */
            for (i = 0; i < (*idl)->b_nids && id > (*idl)->b_ids[i]; i++) {
                ; /* NULL */
            }
            if (i < (*idl)->b_nids && (*idl)->b_ids[i] == id) {
                return (2); /* already there */
            }
        }
    }

    /* do we need to make room for it? */
    if ((*idl)->b_nids == (*idl)->b_nmax) {
        /* make room or indicate block needs splitting */
        if ((*idl)->b_nmax == (ID)maxids) {
            return (3); /* block needs splitting */
        }

        if (idl_tune & IDL_TUNE_NOPAD) {
            (*idl)->b_nmax++;
        } else {
            (*idl)->b_nmax *= 2;
        }
        if ((*idl)->b_nmax > (ID)maxids) {
            (*idl)->b_nmax = maxids;
        }
        *idl = (IDList *)slapi_ch_realloc((char *)*idl,
                                          ((*idl)->b_nmax + 2) * sizeof(ID) + sizeof(IDList));
    }

    /* make a slot for the new id */
    for (j = (*idl)->b_nids; j != i; j--) {
        (*idl)->b_ids[j] = (*idl)->b_ids[j - 1];
    }
    (*idl)->b_ids[i] = id;
    (*idl)->b_nids++;
    (void)memset((char *)&(*idl)->b_ids[(*idl)->b_nids], '\0',
                 ((*idl)->b_nmax - (*idl)->b_nids) * sizeof(ID));

    return (i == 0 ? 1 : 0); /* inserted - first id changed or not */
}

/*
 * idl_delete_key - delete an id from the index entry identified by key
 * returns    0    id was deleted
 *        -666    no such index entry or id in index entry
 *        other    an error code from db
 */

int
idl_old_delete_key(
    backend *be,
    dbi_db_t *db,
    dbi_val_t *key,
    ID id,
    dbi_txn_t *txn,
    struct attrinfo *a __attribute__((unused)))
{
    int i, j, rc;
    const char *msg;
    IDList *idl, *didl;
    dbi_val_t contkey = {0};

    slapi_log_err(SLAPI_LOG_TRACE, "idl_old_delete_key", "=> (%s,%lu)\n",
                  (char *)key->dptr, (u_long)id);

    idl_Wlock_list(a->ai_idl, key);

    if ((idl = idl_fetch_one(be, db, key, txn, &rc)) == NULL) {
        idl_unlock_list(a->ai_idl, key);
        if (rc != 0 && rc != DBI_RC_NOTFOUND && rc != DBI_RC_RETRY) {
            slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "(%s) 0 BAD %d %s\n",
                          (char *)key->dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
        }
        if (0 == rc || DBI_RC_NOTFOUND == rc)
            rc = -666;
        slapi_log_err(SLAPI_LOG_TRACE, "idl_old_delete_key", "<= (%s,%lu) %d !idl_fetch_one\n",
                      (char *)key->dptr, (u_long)id, rc);
        return rc;
    }

    /* regular block */
    if (!INDIRECT_BLOCK(idl)) {
        switch (idl_delete(&idl, id)) {
        case 0: /* id deleted, store the updated block */
        case 1: /* first id changed - ok in direct block */
            rc = idl_store(be, db, key, idl, txn);
            if (rc != 0 && rc != DBI_RC_RETRY) {
                slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "(%s) 1 BAD %d %s\n",
                              (char *)key->dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
            }
            break;

        case 2: /* id deleted, block empty - delete it */
            rc = dblayer_db_op(be, db, txn, DBI_OP_DEL, key, 0);
            if (rc != 0 && rc != DBI_RC_RETRY) {
                slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "(%s) 2 BAD %d %s\n",
                              (char *)key->dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
                if (rc == DBI_RC_RUNRECOVERY) {
                    ldbm_nasty("idl_old_delete_key", "db->del", 74, rc);
                }
            }
            break;

        case 3: /* not there - previously deleted */
        case 4: /* all ids block */
            rc = 0;
            break;

        default:
            slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "(%s) 3 BAD idl_delete\n",
                          (char *)key->dptr);
            break;
        }

        idl_free(&idl);
        idl_unlock_list(a->ai_idl, key);
        slapi_log_err(SLAPI_LOG_TRACE, "idl_old_delete_key", "<= (%s,%lu) %d (not indirect)\n",
                      (char *)key->dptr, (u_long)id, rc);
        return (rc);
    }

    /*
     * this is an indirect block that points to other blocks. we
     * need to read the block containing the id to delete, delete
     * the id, and store the changed block. if the first id in the
     * block changes, or the block becomes empty, we need to rewrite
     * the header block too.
     */

    /* select the block the id is in */
    for (i = 0; idl->b_ids[i] != NOID && id > idl->b_ids[i]; i++) {
        ; /* NULL */
    }
    /* id smaller than smallest id - not there */
    if (i == 0 && id < idl->b_ids[i]) {
        idl_free(&idl);
        idl_unlock_list(a->ai_idl, key);
        slapi_log_err(SLAPI_LOG_TRACE, "idl_old_delete_key", "<= (%s,%lu) -666 (id not found)\n",
                      (char *)key->dptr, (u_long)id);
        return (-666);
    }
    if (id != idl->b_ids[i]) {
        i--;
    }

    /* get the block to delete from */
    make_cont_key(&contkey, key, idl->b_ids[i]);
    if ((didl = idl_fetch_one(be, db, &contkey, txn, &rc)) == NULL) {
        idl_free(&idl);
        idl_unlock_list(a->ai_idl, key);
        if (rc != DBI_RC_RETRY) {
            slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "(%s) 5 BAD %d %s\n",
                          (char *)contkey.dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
        }
        slapi_log_err(SLAPI_LOG_TRACE, "idl_old_delete_key", "<= (%s,%lu) %d idl_fetch_one(contkey)\n",
                      (char *)contkey.dptr, (u_long)id, rc);
        slapi_ch_free((void **)&(contkey.dptr));
        return (rc);
    }

    rc = 0;
    switch (idl_delete(&didl, id)) {
    case 0: /* id deleted - rewrite block */
        if ((rc = idl_store(be, db, &contkey, didl, txn)) != 0) {
            if (rc != DBI_RC_RETRY) {
                slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "(%s) BAD %d %s\n",
                              (char *)contkey.dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
            }
        }
        if (0 != rc) {
            idl_check_indirect(idl, i, didl, NULL, "idl_old_delete_key", "0", key, id);
        }
        break;

    case 1: /* id deleted, first id changed, - write hdr, block  */
        rc = idl_change_first(be, db, key, idl, i, &contkey, didl, txn);
        if (rc != 0 && rc != DBI_RC_RETRY) {
            slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "(%s) 7 BAD %d %s\n",
                          (char *)contkey.dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
        }
        if (0 != rc) {
            idl_check_indirect(idl, i, didl, NULL, "idl_old_delete_key", "1", key, id);
        }
        break;

    case 2: /* id deleted, block empty - write hdr, del block */
        for (j = i; idl->b_ids[j] != NOID; j++) {
            idl->b_ids[j] = idl->b_ids[j + 1];
        }
        if (idl->b_ids[0] != NOID) { /* Write the header, first: */
            rc = idl_store(be, db, key, idl, txn);
            if (rc != 0 && rc != DBI_RC_RETRY) {
                slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "idl_store(%s) BAD %d %s\n",
                              (char *)key->dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
            }
        } else { /* This index is entirely empty.  Delete the header: */
            rc = dblayer_db_op(be, db, txn, DBI_OP_DEL, key, 0);
            if (rc != 0 && rc != DBI_RC_RETRY) {
                slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "db->del(%s) 0 BAD %d %s\n",
                              (char *)key->dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
                if (rc == DBI_RC_RUNRECOVERY) {
                    ldbm_nasty("idl_old_delete_key", "db->del", 75, rc);
                }
            }
        }
        if (rc == 0) { /* Delete the indirect block: */
            rc = dblayer_db_op(be, db, txn, DBI_OP_DEL, &contkey, 0);
            if (rc != 0 && rc != DBI_RC_RETRY) {
                slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "db->del(%s) 1 BAD %d %s\n",
                              (char *)contkey.dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
                if (rc == DBI_RC_RUNRECOVERY) {
                    ldbm_nasty("idl_old_delete_key", "db->del", 76, rc);
                }
            }
        }
        break;

    case 3: /* id not found - previously deleted */
        rc = 0;
        idl_check_indirect(idl, i, didl, NULL, "idl_old_delete_key", "3", key, id);
        break;
    case 4: /* all ids block - should not happen */
        slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "cont block (%s) is allids\n",
                      (char *)contkey.dptr);
        rc = 0;
        break;
    }
    idl_free(&idl);
    idl_free(&didl);
    slapi_ch_free((void **)&(contkey.dptr));
    idl_unlock_list(a->ai_idl, key);
    if (rc != 0 && rc != DBI_RC_RETRY) {
        slapi_log_err(SLAPI_LOG_ERR, "idl_old_delete_key", "(%s) 9 BAD %d %s\n",
                      (char *)key->dptr, rc, (msg = dblayer_strerror(rc)) ? msg : "");
    }
    slapi_log_err(SLAPI_LOG_TRACE, "idl_old_delete_key", "<= (%s,%lu) %d (indirect)\n",
                  (char *)key->dptr, (u_long)id, rc);
    return (rc);
}

/*
 * idl_delete - delete an id from an id list.
 * returns    0    id deleted
 *        1    id deleted, first id in block has changed
 *        2    id deleted, block is empty
 *        3    id not there
 *        4    cannot delete from allids block
 */

int
idl_delete(IDList **idl, ID id)
{
    ID i, delpos;

    if (ALLIDS(*idl)) {
        return (4); /* cannot delete from allids block */
    }

    /* find the id to delete */
    for (i = 0; i < (*idl)->b_nids && id > (*idl)->b_ids[i]; i++) {
        ; /* NULL */
    }
    if (i == (*idl)->b_nids || (*idl)->b_ids[i] != id) {
        return (3); /* id not there */
    }

    if (--((*idl)->b_nids) == 0) {
        return (2); /* id deleted, block empty */
    }

    /* delete it */
    delpos = i;
    for (; i < (*idl)->b_nids; i++) {
        (*idl)->b_ids[i] = (*idl)->b_ids[i + 1];
    }

    return (delpos == 0 ? 1 : 0); /* first id changed : id deleted */
}


static void
make_cont_key(dbi_val_t *contkey, dbi_val_t *key, ID id)
{
    contkey->dptr = (char *)slapi_ch_malloc(key->dsize + 20);
    sprintf(contkey->dptr, "%c%s%lu", CONT_PREFIX, (char *)key->dptr, (u_long)id);
    contkey->dsize = strlen(contkey->dptr) + 1;
}

int
idl_sort_cmp(const void *x, const void *y)
{
    return *(ID *)x - *(ID *)y;
}
