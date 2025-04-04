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

/* vlv.c */


/*
 * References to on-line documentation here.
 *
 * http://BLUES/users/dboreham/publish/Design_Documentation/RFCs/draft-ietf-asid-ldapv3-virtuallistview-01.html
 * http://warp.mcom.com/server/directory-server/clientsdk/hammerhead/design/virtuallistview.html
 * ftp://ftp.ietf.org/internet-drafts/draft-ietf-ldapext-ldapv3-vlv-00.txt
 * http://rocknroll/users/merrells/publish/vlvimplementation.html
 */


#include "back-ldbm.h"
#include "dblayer.h"
#include "vlv_srch.h"
#include "vlv_key.h"

static PRUint32 vlv_trim_candidates_byindex(PRUint32 length, const struct vlv_request *vlv_request_control);
static PRUint32 vlv_trim_candidates_byvalue(backend *be, const IDList *candidates, const sort_spec *sort_control, const struct vlv_request *vlv_request_control, back_txn *txn);
static int vlv_build_candidate_list(backend *be, struct vlvIndex *p, const struct vlv_request *vlv_request_control, IDList **candidates, struct vlv_response *vlv_response_control, int is_srchlist_locked, back_txn *txn);

/* New mutex for vlv locking
Slapi_RWLock * vlvSearchList_lock=NULL;
static struct vlvSearch *vlvSearchList= NULL;
*/

#define ISLEGACY(be) (be ? (be->be_instance_info ? (((ldbm_instance *)be->be_instance_info)->inst_li ? (((ldbm_instance *)be->be_instance_info)->inst_li->li_legacy_errcode) : 0) : 0) : 0)

/* Callback to add a new VLV Search specification. Added write lock.*/

int
vlv_AddSearchEntry(Slapi_PBlock *pb,
                   Slapi_Entry *entryBefore,
                   Slapi_Entry *entryAfter __attribute__((unused)),
                   int *returncode __attribute__((unused)),
                   char *returntext __attribute__((unused)),
                   void *arg)
{
    ldbm_instance *inst = (ldbm_instance *)arg;
    struct vlvSearch *newVlvSearch = vlvSearch_new();
    backend *be = NULL;
    if (inst) {
        be = inst->inst_be;
    }

    if (NULL == be) { /* backend is not associated */
        vlvSearch_delete(&newVlvSearch);
        return SLAPI_DSE_CALLBACK_ERROR;
    }
    vlvSearch_init(newVlvSearch, pb, entryBefore, inst);
    /* vlvSearchList is modified; need Wlock */
    slapi_rwlock_wrlock(be->vlvSearchList_lock);
    vlvSearch_addtolist(newVlvSearch, (struct vlvSearch **)&be->vlvSearchList);
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    return SLAPI_DSE_CALLBACK_OK;
}

/* Callback to add a new VLV Index specification. Added write lock.*/

int
vlv_AddIndexEntry(Slapi_PBlock *pb __attribute__((unused)),
                  Slapi_Entry *entryBefore,
                  Slapi_Entry *entryAfter __attribute__((unused)),
                  int *returncode __attribute__((unused)),
                  char *returntext __attribute__((unused)),
                  void *arg)
{
    struct vlvSearch *parent;
    backend *be = ((ldbm_instance *)arg)->inst_be;
    Slapi_DN parentdn;

    slapi_sdn_init(&parentdn);
    slapi_sdn_get_parent(slapi_entry_get_sdn(entryBefore), &parentdn);

    /* vlvIndex list is modified; need Wlock */
    slapi_rwlock_wrlock(be->vlvSearchList_lock);
    parent = vlvSearch_finddn((struct vlvSearch *)be->vlvSearchList, &parentdn);
    if (parent != NULL) {
        char *name = (char *)slapi_entry_attr_get_ref(entryBefore, type_vlvName);
        if (vlvSearch_findname(parent, name)) {
            /* The vlvindex is already in the vlvSearchList. Skip adding it. */
            slapi_log_err(SLAPI_LOG_BACKLDBM,
                          "vlv_AddIndexEntry", "%s is already in vlvSearchList\n",
                          slapi_entry_get_dn_const(entryBefore));
        } else {
            struct vlvIndex *newVlvIndex = vlvIndex_new();
            newVlvIndex->vlv_be = be;
            vlvIndex_init(newVlvIndex, be, parent, entryBefore);
            vlvSearch_addIndex(parent, newVlvIndex);
        }
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    slapi_sdn_done(&parentdn);
    return SLAPI_DSE_CALLBACK_OK;
}

/* Callback to delete a  VLV Index specification. Added write lock.*/

int
vlv_DeleteSearchEntry(Slapi_PBlock *pb __attribute__((unused)),
                      Slapi_Entry *entryBefore,
                      Slapi_Entry *entryAfter __attribute__((unused)),
                      int *returncode __attribute__((unused)),
                      char *returntext __attribute__((unused)),
                      void *arg)
{
    struct vlvSearch *p = NULL;
    ldbm_instance *inst = (ldbm_instance *)arg;
    backend *be = inst->inst_be;

    if (instance_set_busy(inst) != 0) {
        slapi_log_err(SLAPI_LOG_WARNING,
                      "vlv_DeleteSearchEntry", "Backend instance: '%s' is already in the middle of "
                                               "another task and cannot be disturbed.\n",
                      inst->inst_name);
        return SLAPI_DSE_CALLBACK_ERROR;
    }
    /* vlvSearchList is modified; need Wlock */
    slapi_rwlock_wrlock(be->vlvSearchList_lock);
    p = vlvSearch_finddn((struct vlvSearch *)be->vlvSearchList, slapi_entry_get_sdn(entryBefore));
    if (p != NULL) {
        slapi_log_err(SLAPI_LOG_INFO, "vlv_DeleteSearchEntry", "Deleted Virtual List View Search (%s).\n", p->vlv_name);
        vlvSearch_removefromlist((struct vlvSearch **)&be->vlvSearchList, p->vlv_dn);
        vlvSearch_delete(&p);
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    instance_set_not_busy(inst);
    return SLAPI_DSE_CALLBACK_OK;
}


/* Stub Callback to delete a  VLV Index specification.*/

int
vlv_DeleteIndexEntry(Slapi_PBlock *pb __attribute__((unused)),
                     Slapi_Entry *entryBefore __attribute__((unused)),
                     Slapi_Entry *entryAfter __attribute__((unused)),
                     int *returncode __attribute__((unused)),
                     char *returntext __attribute__((unused)),
                     void *arg)
{
    ldbm_instance *inst = (ldbm_instance *)arg;
    if (inst && is_instance_busy(inst)) {
        slapi_log_err(SLAPI_LOG_WARNING,
                      "vlv_DeleteIndexEntry", "Backend instance: '%s' is already in the middle of "
                                              "another task and cannot be disturbed.\n",
                      inst->inst_name);
        return SLAPI_DSE_CALLBACK_ERROR;
    } else {
        slapi_log_err(SLAPI_LOG_INFO,
                      "vlv_DeleteIndexEntry", "Deleted Virtual List View Index.\n");
        return SLAPI_DSE_CALLBACK_OK;
    }
}


/* Callback to modify a  VLV Search specification. Added read lock.*/

int
vlv_ModifySearchEntry(Slapi_PBlock *pb __attribute__((unused)),
                      Slapi_Entry *entryBefore,
                      Slapi_Entry *entryAfter __attribute__((unused)),
                      int *returncode __attribute__((unused)),
                      char *returntext __attribute__((unused)),
                      void *arg)
{
    struct vlvSearch *p = NULL;
    backend *be = ((ldbm_instance *)arg)->inst_be;

    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    p = vlvSearch_finddn((struct vlvSearch *)be->vlvSearchList, slapi_entry_get_sdn(entryBefore));
    if (p != NULL) {
        slapi_log_err(SLAPI_LOG_NOTICE, "vlv_ModifySearchEntry", "Modified Virtual List View Search (%s), "
                      "which will be enabled when the database is rebuilt.\n",
                      p->vlv_name);
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    return SLAPI_DSE_CALLBACK_OK;
}


/* Stub callback to modify a  VLV Index specification. */

int
vlv_ModifyIndexEntry(Slapi_PBlock *pb __attribute__((unused)),
                     Slapi_Entry *entryBefore __attribute__((unused)),
                     Slapi_Entry *entryAfter __attribute__((unused)),
                     int *returncode __attribute__((unused)),
                     char *returntext __attribute__((unused)),
                     void *arg __attribute__((unused)))
{
    slapi_log_err(SLAPI_LOG_NOTICE, "vlv_ModifyIndexEntry", "Modified Virtual List View Index, "
                  "you will need to reindex this VLV entry(or rebuilt database) for these changes to take effect.\n");
    return SLAPI_DSE_CALLBACK_OK;
}


/* Callback to rename a  VLV Search specification. Added read lock.*/

int
vlv_ModifyRDNSearchEntry(Slapi_PBlock *pb __attribute__((unused)),
                         Slapi_Entry *entryBefore,
                         Slapi_Entry *entryAfter __attribute__((unused)),
                         int *returncode __attribute__((unused)),
                         char *returntext __attribute__((unused)),
                         void *arg)
{
    struct vlvSearch *p = NULL;
    backend *be = ((ldbm_instance *)arg)->inst_be;

    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    p = vlvSearch_finddn((struct vlvSearch *)be->vlvSearchList, slapi_entry_get_sdn(entryBefore));
    if (p != NULL) {
        slapi_log_err(SLAPI_LOG_INFO, "vlv_ModifyRDNSearchEntry",
                      "Modified Virtual List View Search (%s), which will be enabled when the database is rebuilt.\n", p->vlv_name);
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    return SLAPI_DSE_CALLBACK_DO_NOT_APPLY;
}


/* Stub callback to modify a  VLV Index specification. */

int
vlv_ModifyRDNIndexEntry(Slapi_PBlock *pb __attribute__((unused)),
                        Slapi_Entry *entryBefore __attribute__((unused)),
                        Slapi_Entry *entryAfter __attribute__((unused)),
                        int *returncode __attribute__((unused)),
                        char *returntext __attribute__((unused)),
                        void *arg __attribute__((unused)))
{
    slapi_log_err(SLAPI_LOG_INFO, "vlv_ModifyRDNIndexEntry", "Modified Virtual List View Index.\n");
    return SLAPI_DSE_CALLBACK_DO_NOT_APPLY;
}

/* Something may have just read a VLV Entry. */

int
vlv_SearchIndexEntry(Slapi_PBlock *pb __attribute__((unused)),
                     Slapi_Entry *entryBefore,
                     Slapi_Entry *entryAfter __attribute__((unused)),
                     int *returncode __attribute__((unused)),
                     char *returntext __attribute__((unused)),
                     void *arg)
{
    char *name = (char *)slapi_entry_attr_get_ref(entryBefore, type_vlvName);
    backend *be = ((ldbm_instance *)arg)->inst_be;
    if (name != NULL) {
        struct vlvIndex *p = vlv_find_searchname(name, be); /* lock list */
        if (p != NULL) {
            if (vlvIndex_enabled(p)) {
                slapi_entry_attr_set_charptr(entryBefore, type_vlvEnabled, "1");
            } else {
                slapi_entry_attr_set_charptr(entryBefore, type_vlvEnabled, "0");
            }
            slapi_entry_attr_set_ulong(entryBefore, type_vlvUses, p->vlv_uses);
        }
    }
    return SLAPI_DSE_CALLBACK_OK;
}

/* Handle results of a search for objectclass "vlvIndex". Called by vlv_init at inittime -- no need to lock*/

static int
vlv_init_index_entry(Slapi_PBlock *pb __attribute__((unused)),
                     Slapi_Entry *entryBefore,
                     Slapi_Entry *entryAfter __attribute__((unused)),
                     int *returncode __attribute__((unused)),
                     char *returntext __attribute__((unused)),
                     void *arg)
{
    struct vlvIndex *newVlvIndex;
    struct vlvSearch *pSearch;
    Slapi_Backend *be = ((ldbm_instance *)arg)->inst_be;

    if (be != NULL) {
        Slapi_DN parentdn;

        slapi_sdn_init(&parentdn);
        newVlvIndex = vlvIndex_new();
        slapi_sdn_get_parent(slapi_entry_get_sdn(entryBefore), &parentdn);
        pSearch = vlvSearch_finddn((struct vlvSearch *)be->vlvSearchList, &parentdn);
        if (pSearch == NULL) {
            slapi_log_err(SLAPI_LOG_WARNING, "vlv_init_index_entry", "Parent doesn't exist for entry %s.\n",
                          slapi_entry_get_dn(entryBefore));
            vlvIndex_delete(&newVlvIndex);
        } else {
            vlvIndex_init(newVlvIndex, be, pSearch, entryBefore);
            vlvSearch_addIndex(pSearch, newVlvIndex);
        }
        slapi_sdn_done(&parentdn);
    }
    return SLAPI_DSE_CALLBACK_OK;
}

/* Handle results of a search for objectclass "vlvSearch". Called by vlv_init at inittime -- no need to lock*/

static int
vlv_init_search_entry(Slapi_PBlock *pb,
                      Slapi_Entry *entryBefore,
                      Slapi_Entry *entryAfter __attribute__((unused)),
                      int *returncode __attribute__((unused)),
                      char *returntext __attribute__((unused)),
                      void *arg)
{
    struct vlvSearch *newVlvSearch = vlvSearch_new();
    ldbm_instance *inst = (ldbm_instance *)arg;
    backend *be = inst->inst_be;

    if (NULL == be) { /* backend is not associated */
        vlvSearch_delete(&newVlvSearch);
        return SLAPI_DSE_CALLBACK_ERROR;
    }
    vlvSearch_init(newVlvSearch, pb, entryBefore, inst);
    vlvSearch_addtolist(newVlvSearch, (struct vlvSearch **)&be->vlvSearchList);
    return SLAPI_DSE_CALLBACK_OK;
}

/* Look at a new entry, and the set of VLV searches, and see whether
there are any which have deferred initialization and which can now
be initialized given the new entry. Added write lock. */


void
vlv_grok_new_import_entry(const struct backentry *e, backend *be, int *seen_them_all)
{
    struct vlvSearch *p = NULL;
    int any_not_done = 0;


    slapi_rwlock_wrlock(be->vlvSearchList_lock);
    if (*seen_them_all) {
        slapi_rwlock_unlock(be->vlvSearchList_lock);
        return;
    }
    p = (struct vlvSearch *)be->vlvSearchList;

    /* Walk the list of searches */
    for (; p != NULL; p = p->vlv_next)
        /* is this one not initialized ? */
        if (0 == p->vlv_initialized) {
            any_not_done = 1;
            /* Is its base the entry we have here ? */
            if (0 == slapi_sdn_compare(backentry_get_sdn(e), p->vlv_base)) {
                /* Then initialize it */
                vlvSearch_reinit(p, e);
            }
        }
    if (!any_not_done) {
        *seen_them_all = 1;
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
}

void
vlv_rebuild_scope_filter(backend *be)
{
    ldbm_instance *inst = (ldbm_instance *)be->be_instance_info;
    struct vlvSearch *p = NULL;
    back_txn new_txn = {NULL};
    back_txn *txn = NULL;
    Slapi_PBlock *pb;

    txn = dblayer_get_pvt_txn(); /* Let reuse existing txn if possible */
    if (!txn && dblayer_read_txn_begin(be, NULL, &new_txn) == 0) {
            txn = &new_txn;
    }
    pb = slapi_pblock_new();
    slapi_search_internal_set_pb(pb, "", 0, NULL, NULL, 0, NULL, NULL,
                                 (void *)plugin_get_default_component_id(), 0);
    slapi_pblock_set(pb, SLAPI_BACKEND, be);
    slapi_pblock_set(pb, SLAPI_PLUGIN, be->be_database);
    if (txn) {
        slapi_pblock_set(pb, SLAPI_TXN, txn->back_txn_txn);
    }

    slapi_rwlock_wrlock(be->vlvSearchList_lock);
    for (p = be->vlvSearchList; p != NULL; p = p->vlv_next) {
        if (p->vlv_scope != LDAP_SCOPE_ONELEVEL) {
            /* Only the LDAP_SCOPE_ONELEVEL needs to be rebuild as
             * they have parentid = baseentryid in their filter
             */
            continue;
        }
        p->vlv_initialized = 0;
        if (!slapi_sdn_isempty(p->vlv_base)) {
            struct backentry *e = NULL;
            entry_address addr;
            addr.sdn = p->vlv_base;
            addr.uniqueid = NULL;
            e = find_entry(pb, be, &addr, txn, NULL);
            if (NULL != e) {
                vlvSearch_reinit(p, e);
                CACHE_RETURN(&inst->inst_cache, &e);
                p->vlv_initialized = 1;
            }
        }
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    if (txn == &new_txn) {
        dblayer_read_txn_abort(be, txn);
    }
    slapi_pblock_destroy(pb);
}

void
vlv_close(ldbm_instance *inst)
{
    backend *be = inst->inst_be;

    if (be->vlvSearchList_lock) {
        slapi_destroy_rwlock(be->vlvSearchList_lock);
    }
}

/*
 * List vlv filenames without acceding to the vlv target
 *  backend (unlike vlv_init).
 * Note that vlv configuration is not fully checked so it is
 * possible to get names that are not assiciated to working index.
 * (Useless empty mdb db files are created in such case)
 */
char **
vlv_list_filenames(ldbm_instance *inst)
{
    /* The FE DSE *must* be initialised before we get here */
    const char *indexfilter = "(objectclass=vlvindex)";
    char * attrs[] = { (char*)type_vlvName, NULL };
    Slapi_Entry **entries = NULL;
    Slapi_PBlock *tmp_pb;
    char *basedn = NULL;
    char **names = NULL;

    if (!inst) {
        return names;
    }

    basedn = slapi_create_dn_string("cn=%s,cn=%s,cn=plugins,cn=config",
                                    inst->inst_name, inst->inst_li->li_plugin->plg_name);
    if (NULL == basedn) {
        return names;
    }

    tmp_pb = slapi_search_internal(basedn, LDAP_SCOPE_SUBTREE, indexfilter, NULL, attrs, 0);
    slapi_pblock_get(tmp_pb, SLAPI_PLUGIN_INTOP_SEARCH_ENTRIES, &entries);
    for (size_t i = 0; entries && entries[i] != NULL; i++) {
        const char *name = slapi_entry_attr_get_ref(entries[i], type_vlvName);
        char *filename = name ? vlvIndex_build_filename(name) : NULL;
        if (filename) {
            charray_add(&names, filename);
        }
    }
    slapi_free_search_results_internal(tmp_pb);
    slapi_pblock_destroy(tmp_pb);
    slapi_ch_free_string(&basedn);
    return names;
}

int
does_vlv_need_init(ldbm_instance *inst)
{
    return (inst && inst->inst_be->vlvSearchList_lock == NULL);
}

/*
 * Search for the VLV entries which describe the pre-computed indexes we
 * support.  Register administartion DSE callback functions.
 * This is exported to the backend initialisation routine.
 * 'inst' may be NULL for non-slapd initialization...
 */
int
vlv_init(ldbm_instance *inst)
{
    /* The FE DSE *must* be initialised before we get here */
    int return_value = LDAP_SUCCESS;
    int scope = LDAP_SCOPE_SUBTREE;
    char *basedn = NULL;
    const char *searchfilter = "(objectclass=vlvsearch)";
    const char *indexfilter = "(objectclass=vlvindex)";
    backend *be = NULL;

    if (!inst) {
        slapi_log_err(SLAPI_LOG_ERR, "vlv_init", "Invalid instance.\n");
        return_value = LDAP_OPERATIONS_ERROR;
        goto out;
    }

    be = inst->inst_be;

    /* Initialize lock first time through */
    if (be->vlvSearchList_lock == NULL) {
        be->vlvSearchList_lock = slapi_new_rwlock();
    }

    slapi_rwlock_wrlock(be->vlvSearchList_lock);
    if (NULL != (struct vlvSearch *)be->vlvSearchList) {
        struct vlvSearch *t = NULL;
        struct vlvSearch *nt = NULL;
        /* vlvSearchList is modified; need Wlock */
        for (t = (struct vlvSearch *)be->vlvSearchList; NULL != t;) {
            nt = t->vlv_next;
            vlvSearch_delete(&t);
            t = nt;
        }
        be->vlvSearchList = NULL;
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);

    {
        basedn = slapi_create_dn_string("cn=%s,cn=%s,cn=plugins,cn=config",
                                        inst->inst_name, inst->inst_li->li_plugin->plg_name);
        if (NULL == basedn) {
            slapi_log_err(SLAPI_LOG_ERR,
                          "vlv_init", "Failed to create vlv dn for plugin %s, instance %s\n",
                          inst->inst_name, inst->inst_li->li_plugin->plg_name);
            return_value = LDAP_PARAM_ERROR;
            return return_value;
        }
    }

    /* Find the VLV Search Entries */
    {
        Slapi_PBlock *tmp_pb;
        slapi_config_register_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_init_search_entry, (void *)inst);
        tmp_pb = slapi_search_internal(basedn, scope, searchfilter, NULL, NULL, 0);
        slapi_config_remove_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_init_search_entry);
        slapi_free_search_results_internal(tmp_pb);
        slapi_pblock_destroy(tmp_pb);
    }

    /* Find the VLV Index Entries */
    {
        Slapi_PBlock *tmp_pb;
        slapi_config_register_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_init_index_entry, (void *)inst);
        tmp_pb = slapi_search_internal(basedn, scope, indexfilter, NULL, NULL, 0);
        slapi_config_remove_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_init_index_entry);
        slapi_free_search_results_internal(tmp_pb);
        slapi_pblock_destroy(tmp_pb);
    }

    /* Only need to register these callbacks for SLAPD mode... */
    if (basedn) {
        /* In case the vlv indexes are already registered, clean them up before register them. */
        slapi_config_remove_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_SearchIndexEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_ADD, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_AddSearchEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_ADD, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_AddIndexEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_ModifySearchEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_ModifyIndexEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_DeleteSearchEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_DeleteIndexEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_MODRDN, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_ModifyRDNSearchEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_MODRDN, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_ModifyRDNIndexEntry);

        slapi_config_register_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_SearchIndexEntry, (void *)inst);
        slapi_config_register_callback(SLAPI_OPERATION_ADD, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_AddSearchEntry, (void *)inst);
        slapi_config_register_callback(SLAPI_OPERATION_ADD, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_AddIndexEntry, (void *)inst);
        slapi_config_register_callback(SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_ModifySearchEntry, (void *)inst);
        slapi_config_register_callback(SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_ModifyIndexEntry, (void *)inst);
        slapi_config_register_callback(SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_DeleteSearchEntry, (void *)inst);
        slapi_config_register_callback(SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_DeleteIndexEntry, (void *)inst);
        slapi_config_register_callback(SLAPI_OPERATION_MODRDN, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_ModifyRDNSearchEntry, (void *)inst);
        slapi_config_register_callback(SLAPI_OPERATION_MODRDN, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_ModifyRDNIndexEntry, (void *)inst);
        slapi_ch_free_string(&basedn);
    }

out:
    return return_value;
}

/* Removes callbacks from above when  instance is removed. */

int
vlv_remove_callbacks(ldbm_instance *inst)
{
    int return_value = LDAP_SUCCESS;
    int scope = LDAP_SCOPE_SUBTREE;
    char *basedn = NULL;
    const char *searchfilter = "(objectclass=vlvsearch)";
    const char *indexfilter = "(objectclass=vlvindex)";

    if (inst == NULL) {
        basedn = NULL;
    } else {
        basedn = slapi_create_dn_string("cn=%s,cn=%s,cn=plugins,cn=config",
                                        inst->inst_name, inst->inst_li->li_plugin->plg_name);
        if (NULL == basedn) {
            slapi_log_err(SLAPI_LOG_ERR,
                          "vlv_remove_callbacks", "Failed to create vlv dn for plugin %s, "
                                                  "instance %s\n",
                          inst->inst_name, inst->inst_li->li_plugin->plg_name);
            return_value = LDAP_PARAM_ERROR;
        }
    }
    if (basedn != NULL) {
        slapi_config_remove_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_SearchIndexEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_ADD, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_AddSearchEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_ADD, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_AddIndexEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_ModifySearchEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_ModifyIndexEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_DeleteSearchEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_DeleteIndexEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_MODRDN, DSE_FLAG_PREOP, basedn, scope, searchfilter, vlv_ModifyRDNSearchEntry);
        slapi_config_remove_callback(SLAPI_OPERATION_MODRDN, DSE_FLAG_PREOP, basedn, scope, indexfilter, vlv_ModifyRDNIndexEntry);
        slapi_ch_free_string(&basedn);
    }
    return return_value;
}

/* Find an enabled index which matches this description. */

static struct vlvIndex *
vlv_find_search(backend *be, const Slapi_DN *base, int scope, const char *filter, const sort_spec *sort_control)
{
    return vlvSearch_findenabled(be, (struct vlvSearch *)be->vlvSearchList, base, scope, filter, sort_control);
}


/* Find a search which matches this name. Added read lock. */

struct vlvIndex *
vlv_find_searchname(const char *name, backend *be)
{
    struct vlvIndex *p = NULL;

    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    p = vlvSearch_findname((struct vlvSearch *)be->vlvSearchList, name);
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    return p;
}

/* Find a search which matches this indexname. Added to read lock */

struct vlvIndex *
vlv_find_indexname(const char *name, backend *be)
{

    struct vlvIndex *p = NULL;

    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    p = vlvSearch_findindexname((struct vlvSearch *)be->vlvSearchList, name);
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    return p;
}


/* Get a list of known VLV Indexes. Added read lock */

char *
vlv_getindexnames(backend *be)
{
    char *n = NULL;

    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    n = vlvSearch_getnames((struct vlvSearch *)be->vlvSearchList);
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    return n;
}

/* Return the list of VLV indices to the import code. Added read lock */

void
vlv_getindices(int32_t (*callback_fn)(caddr_t, caddr_t), void *param, backend *be)
{
    /* Traverse the list, calling the import code's callback function */
    struct vlvSearch *ps = NULL;

    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    ps = (struct vlvSearch *)be->vlvSearchList;
    for (; ps != NULL; ps = ps->vlv_next) {
        struct vlvIndex *pi = ps->vlv_index;
        for (; pi != NULL; pi = pi->vlv_next) {
            callback_fn((caddr_t)(pi->vlv_attrinfo), (caddr_t)param);
        }
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
}

/*
 * Create a key for the entry in the vlv index.
 *
 * The key is a composite of a value from each sorted attribute.
 *
 * If a sorted attribute has many values, then the key is built
 * with the attribute value with the lowest value.
 *
 * The primary sorted attribute value is followed by a 0x00 to
 * ensure that short attribute values appear before longer ones.
 *
 * Many entries may have the same attribute values, which would
 * generate the same composite key, so we append the EntryID
 * to ensure the uniqueness of the key.
 *
 * May return NULL in case of errors (typically in some configuration error cases)
 */
static struct vlv_key *
vlv_create_key(struct vlvIndex *p, struct backentry *e)
{
    struct berval val;
    unsigned char char_min = 0x00;
    unsigned char char_max = 0xFF;
    struct vlv_key *key = vlv_key_new();
    struct berval **value = NULL;
    int free_value = 0;

    if (p->vlv_sortkey != NULL) {
        /* Foreach sorted attribute... */
        int sortattr = 0;
        while (p->vlv_sortkey[sortattr] != NULL) {
            Slapi_Attr *attr = attrlist_find(e->ep_entry->e_attrs, p->vlv_sortkey[sortattr]->sk_attrtype);
            {
                /*
                 * If there's a matching rule associated with the sorted
                 * attribute then use the indexer to mangle the attr values.
                 * This ensures that the international characters will
                 * collate in the correct order.
                 */

                /* xxxPINAKI */
                /* need to free some stuff! */
                Slapi_Value **cvalue = NULL;
                struct berval *lowest_value = NULL;

                if (attr != NULL && !valueset_isempty(&attr->a_present_values)) {
                    /* Sorted attribute found. */
                    int totalattrs;
                    if (p->vlv_sortkey[sortattr]->sk_matchruleoid == NULL) {
                        /* No matching rule. mangle values according to matching rule or syntax */
                        Slapi_Value **va = valueset_get_valuearray(&attr->a_present_values);
                        slapi_attr_values2keys_sv(attr, va, &cvalue, LDAP_FILTER_EQUALITY);
                        valuearray_get_bervalarray(cvalue, &value);

                        /* XXXSD need to free some more stuff */
                        {
                            int numval;
                            for (numval = 0; cvalue && cvalue[numval]; numval++) {
                                slapi_value_free(&cvalue[numval]);
                            }
                            if (cvalue)
                                slapi_ch_free((void **)&cvalue);
                        }

                        free_value = 1;
                    } else {
                        /* Matching rule. Do the magic mangling. Plugin owns the memory. */
                        if (p->vlv_mrpb[sortattr] != NULL) {
                            /* xxxPINAKI */
                            Slapi_Value **va = valueset_get_valuearray(&attr->a_present_values);
                            matchrule_values_to_keys(p->vlv_mrpb[sortattr], va, &value);
                        }
                    }

                    if (!value) {
                        goto error;
                    }

                    for (totalattrs = 0; value[totalattrs] != NULL; totalattrs++) {
                    }; /* Total Number of Attributes */
                    if (totalattrs == 1) {
                        lowest_value = value[0];
                    } else {
                        lowest_value = attr_value_lowest(value, slapi_berval_cmp);
                    }
                } /* end of if (attr != NULL && ...) */
                if (p->vlv_sortkey[sortattr]->sk_reverseorder) {
                    /*
                     * This attribute is reverse sorted, so we must
                     * invert the attribute value so that the keys
                     * will be in the correct order.
                     */
                    unsigned int i;
                    char *attributeValue = NULL;
                    /* Bug 605477 : Don't malloc 0 bytes */
                    if (attr != NULL && lowest_value && lowest_value->bv_len != 0) {
                        attributeValue = (char *)slapi_ch_malloc(lowest_value->bv_len);
                        for (i = 0; i < lowest_value->bv_len; i++) {
                            attributeValue[i] = UCHAR_MAX - ((char *)lowest_value->bv_val)[i];
                        }
                        val.bv_len = lowest_value->bv_len;
                        val.bv_val = (void *)attributeValue;
                    } else {
                        /* Reverse Sort: We use an attribute value of 0x00 when
                        * there is no attribute value or attrbute is absent
                        */
                        val.bv_val = (void *)&char_min;
                        val.bv_len = 1;
                    }
                    vlv_key_addattr(key, &val);
                    slapi_ch_free((void **)&attributeValue);
                } else {
                    /*
                     * This attribute is forward sorted, so add the
                     * attribute value to the end of all the keys.
                     */

                    /* If the forward-sorted attribute is absent or has no
                     * value, we need to use the value of 0xFF.
                     */
                    if (attr != NULL && lowest_value && lowest_value->bv_len > 0) {
                        vlv_key_addattr(key, lowest_value);
                    } else {
                        val.bv_val = (void *)&char_max;
                        val.bv_len = 1;
                        vlv_key_addattr(key, &val);
                    }
                }
                if (sortattr == 0) {
                    /*
                     * If this is the first attribute (the typedown attribute)
                     * then it should be followed by a zero.  This is to ensure
                     * that shorter attribute values appear before longer ones.
                     */
                    char zero = 0;
                    val.bv_len = 1;
                    val.bv_val = (void *)&zero;
                    vlv_key_addattr(key, &val);
                }
                if (free_value) {
                    ber_bvecfree(value);
                    free_value = 0;
                }
                value = NULL;
            }
            sortattr++;
        }
    }
    {
        /* Append the EntryID to the key to ensure uniqueness */
        val.bv_len = sizeof(e->ep_id);
        val.bv_val = (void *)&e->ep_id;
        vlv_key_addattr(key, &val);
    }
    return key;

error:
    if (free_value)
        ber_bvecfree(value);
    vlv_key_delete(&key);
    return NULL;
}

/*
 * Insert or Delete the entry to or from the index
 */

static int
do_vlv_update_index(back_txn *txn, struct ldbminfo *li, Slapi_PBlock *pb, struct vlvIndex *pIndex, struct backentry *entry, int insert)
{
    backend *be;
    int rc = 0;
    dbi_db_t *db = NULL;
    dbi_txn_t *db_txn = NULL;
    struct vlv_key *key = NULL;
    dbi_val_t data = {0};
    dblayer_private *priv = NULL;

    slapi_pblock_get(pb, SLAPI_BACKEND, &be);
    priv = (dblayer_private *)li->li_dblayer_private;

    rc = dblayer_get_index_file(be, pIndex->vlv_attrinfo, &db, DBOPEN_CREATE);
    if (rc != 0) {
        if (rc != DBI_RC_RETRY)
            slapi_log_err(SLAPI_LOG_ERR, "do_vlv_update_index", "Can't get index file '%s' (err %d)\n",
                          pIndex->vlv_attrinfo->ai_type, rc);
        return rc;
    }

    key = vlv_create_key(pIndex, entry);
    if (key == NULL) {
        slapi_log_err(SLAPI_LOG_ERR, "vlv_create_key", "Unable to generate vlv %s index key."
                      " There may be a configuration issue.\n", pIndex->vlv_name);
        dblayer_release_index_file(be, pIndex->vlv_attrinfo, db);
        return rc;
    }

    if (NULL != txn) {
        db_txn = txn->back_txn_txn;
    } else {
        /* Very bad idea to do this outside of a transaction */
    }
    if (txn && !txn->back_special_handling_fn && priv->dblayer_clear_vlv_cache_fn) {
        /* If there is a txn and it is not an import pseudo txn then clear the vlv cache */
        priv->dblayer_clear_vlv_cache_fn(be, db_txn, db);
    }
    data.size = sizeof(entry->ep_id);
    data.data = &entry->ep_id;

    if (insert) {
        if (txn && txn->back_special_handling_fn) {
            rc = txn->back_special_handling_fn(be, BTXNACT_VLV_ADD, db, &key->key, &data, txn);
        } else {
            rc = dblayer_db_op(be, db, db_txn, DBI_OP_PUT, &key->key, &data);
        }
        if (rc == 0) {
            slapi_log_err(SLAPI_LOG_TRACE,
                          "vlv_update_index", "%s Insert %s ID=%lu\n",
                          pIndex->vlv_name, (char *)key->key.data, (u_long)entry->ep_id);
            if (txn && txn->back_special_handling_fn) {
                /* In import only one thread works on a given vlv index */
                pIndex->vlv_indexlength++;
            } else {
                vlvIndex_increment_indexlength(be, pIndex, db, txn);
            }
        } else if (rc == DBI_RC_RUNRECOVERY) {
            ldbm_nasty("do_vlv_update_index", pIndex->vlv_name, 77, rc);
        } else if (rc != DBI_RC_RETRY) {
            /* jcm: This error is valid if the key already exists.
             * Identical multi valued attr values could do this. */
            slapi_log_err(SLAPI_LOG_TRACE,
                          "vlv_update_index", "%s Insert %s ID=%lu FAILED\n",
                          pIndex->vlv_name, (char *)key->key.data, (u_long)entry->ep_id);
        }
    } else {
        slapi_log_err(SLAPI_LOG_TRACE,
                      "vlv_update_index", "%s Delete %s\n",
                      pIndex->vlv_name, (char *)key->key.data);
        if (txn && txn->back_special_handling_fn) {
            rc = txn->back_special_handling_fn(be, BTXNACT_VLV_DEL, db, &key->key, &data, txn);
        } else {
            rc = dblayer_db_op(be, db, db_txn, DBI_OP_DEL, &key->key, NULL);
        }
        if (rc == 0) {
            if (txn && txn->back_special_handling_fn) {
                /* In import only one thread works on a given vlv index */
                pIndex->vlv_indexlength--;
            } else {
                vlvIndex_decrement_indexlength(be, pIndex, db, txn);
            }
        } else if (rc == DBI_RC_RUNRECOVERY) {
            ldbm_nasty("do_vlv_update_index", pIndex->vlv_name, 78, rc);
        } else if (rc != DBI_RC_RETRY) {
            slapi_log_err(SLAPI_LOG_TRACE,
                          "vlv_update_index", "%s Delete %s FAILED\n",
                          pIndex->vlv_name, (char *)key->key.data);
        }
    }

    vlv_key_delete(&key);
    dblayer_release_index_file(be, pIndex->vlv_attrinfo, db);
    return rc;
}

/*
 * Given an entry modification check if a VLV index needs to be updated.
 */

int
vlv_update_index(struct vlvIndex *p, back_txn *txn, struct ldbminfo *li, Slapi_PBlock *pb, struct backentry *oldEntry, struct backentry *newEntry)
{
    int return_value = 0;
    /* Check if the old entry is in this VLV index */
    if (oldEntry != NULL) {
        if (slapi_sdn_scope_test(backentry_get_sdn(oldEntry), vlvIndex_getBase(p), vlvIndex_getScope(p))) {
            if (slapi_filter_test(pb, oldEntry->ep_entry, vlvIndex_getFilter(p), 0 /* No ACL Check */) == 0) {
                /* Remove the entry from the index */
                return_value = do_vlv_update_index(txn, li, pb, p, oldEntry, 0 /* Delete Key */);
            }
        }
    }
    /* Check if the new entry should be in the VLV index */
    if (newEntry != NULL) {
        if (slapi_sdn_scope_test(backentry_get_sdn(newEntry), vlvIndex_getBase(p), vlvIndex_getScope(p))) {
            if (slapi_filter_test(pb, newEntry->ep_entry, vlvIndex_getFilter(p), 0 /* No ACL Check */) == 0) {
                /* Add the entry to the index */
                return_value = do_vlv_update_index(txn, li, pb, p, newEntry, 1 /* Insert Key */);
            }
        }
    }
    return return_value;
}

/*
 * Given an entry modification check if a VLV index needs to be updated.
 *
 * This is called for every modifying operation, so it must be very efficient.
 *
 * We need to know if we're adding, deleting, or modifying
 * because we could be leaving and/or joining an index
 *
 * ADD: oldEntry==NULL && newEntry!=NULL
 * DEL: oldEntry!=NULL && newEntry==NULL
 * MOD: oldEntry!=NULL && newEntry!=NULL
 *
 * JCM: If only non-sorted attributes are changed, then the indexes don't need updating.
 * JCM: Detecting this fact, given multi-valued atribibutes, might be tricky...
 * Read lock (traverse vlvSearchList; no change on vlvSearchList/vlvIndex lists)
 */

int
vlv_update_all_indexes(back_txn *txn, backend *be, Slapi_PBlock *pb, struct backentry *oldEntry, struct backentry *newEntry)
{
    int return_value = LDAP_SUCCESS;
    struct vlvSearch *ps = NULL;
    struct ldbminfo *li = ((ldbm_instance *)be->be_instance_info)->inst_li;

    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    ps = (struct vlvSearch *)be->vlvSearchList;
    for (; ps != NULL; ps = ps->vlv_next) {
        struct vlvIndex *pi = ps->vlv_index;
        for (return_value = LDAP_SUCCESS; return_value == LDAP_SUCCESS && pi != NULL; pi = pi->vlv_next)
            return_value = vlv_update_index(pi, txn, li, pb, oldEntry, newEntry);
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    return return_value;
}

/*
 * Determine the range of record numbers to return.
 * Prevent an underrun, or overrun.
 */
/* jcm: Should we make sure that start < stop */

static void
determine_result_range(const struct vlv_request *vlv_request_control, PRUint32 index, PRUint32 length, PRUint32 *pstart, PRUint32 *pstop)
{
    if (vlv_request_control == NULL) {
        *pstart = 0;
        if (0 == length) /* 609377: index size could be 0 */
        {
            *pstop = 0;
        } else {
            *pstop = length - 1;
        }
    } else {
        /* Make sure we don't run off the start */
        if ((ber_int_t)index < vlv_request_control->beforeCount) {
            *pstart = 0;
        } else {
            *pstart = index - vlv_request_control->beforeCount;
        }
        /* Make sure we don't run off the end */
        /*
         * if(UINT_MAX - index > vlv_request_control->afterCount), but after is int,
         * so right now, it could overflow before this condition ....
         */
        if (INT_MAX - (ber_int_t)index > vlv_request_control->afterCount) {
            *pstop = index + vlv_request_control->afterCount;
        } else {
            *pstop = UINT_MAX;
        }
        /* Client tried to index off the end */
        if (0 == length) /* 609377: index size could be 0 */
        {
            *pstop = 0;
        } else if (*pstop > length - 1) {
            *pstop = length - 1;
        }
    }
    slapi_log_err(SLAPI_LOG_TRACE, "vlv_determine_result_range", "Result Range %u-%u\n", *pstart, *pstop);
}

/*
 * This is a utility function to pass the client
 * supplied attribute value through the appropriate
 * matching rule indexer.
 *
 * It allocates a berval vector which the caller
 * must free.
 */

static struct berval **
vlv_create_matching_rule_value(Slapi_PBlock *pb, struct berval *original_value)
{
    struct berval **value = NULL;
    if (pb != NULL) {
        struct berval **outvalue = NULL;
        Slapi_Value v_in = {0};
        Slapi_Value *va_in[2] = { &v_in, NULL };
        slapi_value_init_berval(&v_in, original_value);
        /* The plugin owns the memory it returns in outvalue */
        matchrule_values_to_keys(pb, va_in, &outvalue);
        if (outvalue != NULL) {
            value = slapi_ch_bvecdup(outvalue);
        }
    }
    if (value == NULL) {
        struct berval *outvalue[2];
        outvalue[0] = original_value; /* jcm: cast away const */
        outvalue[1] = NULL;
        value = slapi_ch_bvecdup(outvalue);
    }
    return value;
}


/*
 * Find the record number in a VLV index for a given attribute value.
 * The returned index is counted from zero.
 */

static PRUint32
vlv_build_candidate_list_byvalue(backend *be, struct vlvIndex *p, dbi_cursor_t *dbc, PRUint32 length, const struct vlv_request *vlv_request_control)
{
    PRUint32 si = 0; /* The Selected Index */
    int err = 0;
    dbi_val_t key = {0};
    dbi_val_t data = {0};
    /*
     * If the primary sorted attribute has an associated
     * matching rule, then we must mangle the typedown
     * value.
     */
    struct berval **typedown_value = NULL;
    struct berval *invalue[2];
    invalue[0] = (struct berval *)&vlv_request_control->value; /* jcm: cast away const */
    invalue[1] = NULL;
    if (p->vlv_sortkey[0]->sk_matchruleoid == NULL) {
        Slapi_Attr sattr;
        slapi_attr_init(&sattr, p->vlv_sortkey[0]->sk_attrtype);
        slapi_attr_values2keys(&sattr, invalue, &typedown_value, LDAP_FILTER_EQUALITY); /* JCM SLOW FUNCTION */
        attr_done(&sattr);
    } else {
        typedown_value = vlv_create_matching_rule_value(p->vlv_mrpb[0], (struct berval *)&vlv_request_control->value); /* jcm: cast away const */
    }
    if (p->vlv_sortkey[0]->sk_reverseorder) {
        /*
         * The primary attribute is reverse sorted, so we must
         * invert the typedown value in order to match the key.
         */
        unsigned int i;
        for (i = 0; i < (*typedown_value)->bv_len; i++) {
            ((char *)(*typedown_value)->bv_val)[i] = UCHAR_MAX - ((char *)(*typedown_value)->bv_val)[i];
        }
    }

    dblayer_value_set(be, &key, typedown_value[0]->bv_val, typedown_value[0]->bv_len);
    dblayer_value_protect_data(be, &key);  /* typedown_value[0]->bv_val should not be freed */

    dblayer_value_init(be, &data);
    err = dblayer_cursor_op(dbc, DBI_OP_MOVE_NEAR_KEY, &key, &data);
    if (err == 0) {
        err = dblayer_cursor_op(dbc, DBI_OP_GET_RECNO, &key, &data);
        if (err == 0) {
            si = *((dbi_recno_t *)data.data);
            /* Records are numbered from one. */
            si--;
            slapi_log_err(SLAPI_LOG_TRACE, "vlv_build_candidate_list_byvalue", "Found. Index=%u\n", si);
        } else {
            /* Couldn't get the record number for the record we found. */
        }
    } else {
        /* Couldn't find an entry which matches the value,
         * so return the last entry
         * (609377) when the index file is empty, there is no "last entry".
         */
        if (0 == length) {
            si = 0;
        } else {
            si = length - 1;
        }
        slapi_log_err(SLAPI_LOG_TRACE, "vlv_build_candidate_list_byvalue", "Not Found. Index=%u\n", si);
    }
    dblayer_value_free(be, &data);
    dblayer_value_free(be, &key);
    ber_bvecfree((struct berval **)typedown_value);
    return si;
}

/* build a candidate list (IDL) from a VLV index, given the starting index
 * and the ending index (as an inclusive list).
 * returns 0 on success, or an LDAP error code.
 */
int
vlv_build_idl(backend *be, PRUint32 start, PRUint32 stop, dbi_db_t *db __attribute__((unused)), dbi_cursor_t *dbc, IDList **candidates, int dosort)
{
    IDList *idl = NULL;
    int err;
    PRUint32 recno;
    dbi_val_t key = {0};
    dbi_val_t data = {0};
    ID id;
    int rc = LDAP_SUCCESS;

    idl = idl_alloc(stop - start + 1);
    if (!idl) {
        /* out of memory :( */
        rc = LDAP_OPERATIONS_ERROR;
        goto error;
    }
    recno = start + 1;
    dblayer_value_set(be, &key, &recno, sizeof(recno)); /* key may be realloced */
    dblayer_value_protect_data(be, &key);               /* but &recno should not be freed */
    dblayer_value_set_buffer(be, &data, &id, sizeof(ID)); /* while data cannot be realloced */
    err = dblayer_cursor_op(dbc, DBI_OP_MOVE_TO_RECNO, &key, &data);
    while ((err == 0) && (recno <= stop + 1)) {
        idl_append(idl, *(ID *)data.data);
        if (++recno <= stop + 1) {
            err = dblayer_cursor_op(dbc, DBI_OP_NEXT, &key, &data);
            if (err == DBI_RC_NOTFOUND) {
                /* The provided limit (stop) is outdated and there
                 * is no more record after the current limit.
                 * This can occur if entries are deleted at the same time
                 * of vlv search.
                 */
                err = 0;
                break;
            }
        }
    }
    if (err != 0) {
        /* some db error...? */
        slapi_log_err(SLAPI_LOG_ERR, "vlv_build_idl", "Can't follow db cursor "
                                                      "(err %d)\n",
                      err);
        if (err == ENOMEM)
            slapi_log_err(SLAPI_LOG_ERR, "vlv_build_idl", "nomem: wants %ld key, %ld data\n",
                          key.size, data.size);
        rc = LDAP_OPERATIONS_ERROR;
        goto error;
    }

    if (!candidates) {
        goto error;
    }

    /* success! */
    if (dosort) {
        qsort((void *)&idl->b_ids[0], idl->b_nids,
              (size_t)sizeof(ID), idl_sort_cmp);
    }
    *candidates = idl;

    goto done;

error:
    if (idl)
        idl_free(&idl);

done:
    dblayer_value_free(be, &key);
    dblayer_value_free(be, &data);
    return rc;
}


/* This function does vlv_access, searching and building list all while holding read lock

  1. vlv_find_search fails, set:
                    unsigned int opnote = SLAPI_OP_NOTE_UNINDEXED;
                    slapi_pblock_set( pb, SLAPI_OPERATION_NOTES, &opnote );
     return FIND_SEARCH FAILED

  2. vlvIndex_accessallowed fails
     return VLV_LDBM_ACCESS_DENIED

  3. vlv_build_candidate_list fails:
     return VLV_BLD_LIST_FAILED

  4. return LDAP_SUCCESS
*/

int
vlv_search_build_candidate_list(Slapi_PBlock *pb, const Slapi_DN *base, int *vlv_rc, const sort_spec *sort_control, const struct vlv_request *vlv_request_control, IDList **candidates, struct vlv_response *vlv_response_control)
{
    struct vlvIndex *pi = NULL;
    backend *be;
    int scope, rc = LDAP_SUCCESS;
    char *fstr;
    back_txn txn = {NULL};

    slapi_pblock_get(pb, SLAPI_TXN, &txn.back_txn_txn);
    slapi_pblock_get(pb, SLAPI_BACKEND, &be);
    slapi_pblock_get(pb, SLAPI_SEARCH_SCOPE, &scope);
    slapi_pblock_get(pb, SLAPI_SEARCH_STRFILTER, &fstr);
    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    if ((pi = vlv_find_search(be, base, scope, fstr, sort_control)) == NULL) {
        int pr_idx = -1;
        Connection *pb_conn = NULL;
        Operation *pb_op = NULL;

        slapi_pblock_get(pb, SLAPI_PAGED_RESULTS_INDEX, &pr_idx);
        slapi_rwlock_unlock(be->vlvSearchList_lock);

        slapi_pblock_set_flag_operation_notes(pb, SLAPI_OP_NOTE_UNINDEXED);
        slapi_pblock_get(pb, SLAPI_OPERATION, &pb_op);
        slapi_pblock_get(pb, SLAPI_CONNECTION, &pb_conn);

        pagedresults_set_unindexed(pb_conn, pb_op, pr_idx);
        rc = VLV_FIND_SEARCH_FAILED;
    } else if ((*vlv_rc = vlvIndex_accessallowed(pi, pb)) != LDAP_SUCCESS) {
        slapi_rwlock_unlock(be->vlvSearchList_lock);
        rc = VLV_ACCESS_DENIED;
    } else if ((*vlv_rc = vlv_build_candidate_list(be, pi, vlv_request_control, candidates, vlv_response_control, 1, &txn)) != LDAP_SUCCESS) {
        rc = VLV_BLD_LIST_FAILED;
        vlv_response_control->result = *vlv_rc;
    }
    return rc;
}

/*
 * Given the SORT and VLV controls return a candidate list from the
 * pre-computed index file.
 *
 * Returns:
 *       success (0),
 *       operationsError (1),
 *       unwillingToPerform (53),
 *       timeLimitExceeded (3),
 *       adminLimitExceeded (11),
 *       indexRangeError (61),
 *       other (80)
 */


static int
vlv_build_candidate_list(backend *be, struct vlvIndex *p, const struct vlv_request *vlv_request_control, IDList **candidates, struct vlv_response *vlv_response_control, int is_srchlist_locked, back_txn *txn)
{
    int return_value = LDAP_SUCCESS;
    dbi_db_t *db = NULL;
    dbi_cursor_t dbc = {0};
    int rc, err;
    PRUint32 si = 0; /* The Selected Index */
    PRUint32 length;
    int do_trim = 1;
    dbi_txn_t *db_txn = NULL;

    slapi_log_err(SLAPI_LOG_TRACE,
                  "vlv_build_candidate_list", "%s %s Using VLV Index %s\n",
                  slapi_sdn_get_dn(vlvIndex_getBase(p)), p->vlv_search->vlv_filter,
                  vlvIndex_getName(p));
    if (!vlvIndex_online(p)) {
        if (is_srchlist_locked) {
            slapi_rwlock_unlock(be->vlvSearchList_lock);
        }
        return -1;
    }
    rc = dblayer_get_index_file(be, p->vlv_attrinfo, &db, 0);
    if (rc != 0) {
        /* shouldn't happen */
        slapi_log_err(SLAPI_LOG_ERR, "vlv_build_candidate_list", "Can't get index file '%s' (err %d)\n",
                      p->vlv_attrinfo->ai_type, rc);
        if (is_srchlist_locked) {
            slapi_rwlock_unlock(be->vlvSearchList_lock);
        }
        return -1;
    }

    length = vlvIndex_get_indexlength(be, p, db, 0 /* txn */);

    /* Increment the usage counter */
    vlvIndex_incrementUsage(p);

    if (is_srchlist_locked) {
        slapi_rwlock_unlock(be->vlvSearchList_lock);
    }
    if (txn) {
        db_txn = txn->back_txn_txn;
    }
    err = dblayer_new_cursor(be, db, db_txn, &dbc);
    if (err != 0) {
        /* shouldn't happen */
        slapi_log_err(SLAPI_LOG_ERR, "vlv_build_candidate_list", "Couldn't get cursor (err %d)\n",
                      rc);
        return -1;
    }

    if (vlv_request_control) {
        switch (vlv_request_control->tag) {
        case 0: /* byIndex */
            si = vlv_trim_candidates_byindex(length, vlv_request_control);
            break;
        case 1: /* byValue */
            si = vlv_build_candidate_list_byvalue(be, p, &dbc, length, vlv_request_control);
            if (si == length) {
                do_trim = 0;
                /* minimum idl_alloc size should be 1; 0 is considered ALLID */
                *candidates = idl_alloc(1);
            }
            break;
        default:
            /* Some wierd tag value.  Shouldn't ever happen */
            if (ISLEGACY(be)) {
                return_value = LDAP_OPERATIONS_ERROR;
            } else {
                return_value = LDAP_VIRTUAL_LIST_VIEW_ERROR;
            }
            break;
        }

        /* Tell the client what the real content count is.
         * Client counts from 1. */
        vlv_response_control->targetPosition = si + 1;
        vlv_response_control->contentCount = length;
        vlv_response_control->result = return_value;
    }

    if ((return_value == LDAP_SUCCESS) && do_trim) {
        /* Work out the range of records to return */
        PRUint32 start, stop;
        determine_result_range(vlv_request_control, si, length, &start, &stop);

        /* fetch the idl */
        return_value = vlv_build_idl(be, start, stop, db, &dbc, candidates, 0);
    }
    dblayer_cursor_op(&dbc, DBI_OP_CLOSE, NULL, NULL);

    dblayer_release_index_file(be, p->vlv_attrinfo, db);
    return return_value;
}

/*
 * Given a candidate list and a filter specification, filter the candidate list
 *
 * Returns:
 *       success (0),
 *       operationsError (1),
 *       unwillingToPerform (53),
 *       timeLimitExceeded (3),
 *       adminLimitExceeded (11),
 *       indexRangeError (61),
 *       other (80)
 */
int
vlv_filter_candidates(backend *be, Slapi_PBlock *pb, const IDList *candidates, const Slapi_DN *base, int scope, Slapi_Filter *filter, IDList **filteredCandidates, int lookthrough_limit, struct timespec *expire_time)
{
    IDList *resultIdl = NULL;
    int return_value = LDAP_SUCCESS;

    /* Refuse to filter a non-existent IDlist */
    if (NULL == candidates || NULL == filteredCandidates) {
        return LDAP_UNWILLING_TO_PERFORM;
    }

    slapi_log_err(SLAPI_LOG_TRACE, "vlv_filter_candidates", "Filtering %lu Candidates\n", (u_long)candidates->b_nids);

    if (0 == return_value && candidates->b_nids > 0) {
        /* jcm: Could be an idlist function. create_filtered_idlist */
        /* Iterate over the ID List applying the filter */
        int lookedat = 0;
        int done = 0;
        int counter = 0;
        ID id = NOID;
        back_txn txn = {NULL};
        idl_iterator current = idl_iterator_init(candidates);
        resultIdl = idl_alloc(candidates->b_nids);
        slapi_pblock_get(pb, SLAPI_TXN, &txn.back_txn_txn);
        do {
            id = idl_iterator_dereference_increment(&current, candidates);
            if (id != NOID) {
                int err = 0;
                struct backentry *e = NULL;
                e = id2entry(be, id, &txn, &err);
                if (e == NULL) {
                    /*
                     * The ALLIDS ID List contains IDs for which there is no entry.
                     * This is because the entries have been deleted.  An error in
                     * this case is ok.
                     */
                    if (!(ALLIDS(candidates) && err == DBI_RC_NOTFOUND)) {
                        slapi_log_err(SLAPI_LOG_ERR, "vlv_filter_candidates",
                                      "Candidate %lu not found err=%d\n", (u_long)id, err);
                    }
                } else {
                    lookedat++;
                    if (slapi_sdn_scope_test(backentry_get_sdn(e), base, scope)) {
                        if (slapi_filter_test(pb, e->ep_entry, filter, 0 /* No ACL Check */) == 0) {
                            /* The entry passed the filter test, add the id to the list */
                            slapi_log_err(SLAPI_LOG_TRACE, "vlv_filter_candidates",
                                          "Candidate %lu Passed Filter\n", (u_long)id);
                            idl_append(resultIdl, id);
                        }
                    }
                    CACHE_RETURN(&(((ldbm_instance *)be->be_instance_info)->inst_cache), &e);
                }
            }

            done = slapi_op_abandoned(pb);

            /* Check to see if our journey is really necessary */
            if (counter++ % 10 == 0) {
                /* check time limit */
                if (slapi_timespec_expire_check(expire_time) == TIMER_EXPIRED) {
                    slapi_log_err(SLAPI_LOG_TRACE, "vlv_filter_candidates",
                                  "LDAP_TIMELIMIT_EXCEEDED\n");
                    return_value = LDAP_TIMELIMIT_EXCEEDED;
                    done = 1;
                }

                /* check lookthrough limit */
                if (lookthrough_limit != -1 && lookedat > lookthrough_limit) {
                    return_value = LDAP_ADMINLIMIT_EXCEEDED;
                    done = 1;
                }
            }
        } while (!done && id != NOID);
    }

    *filteredCandidates = resultIdl;
    slapi_log_err(SLAPI_LOG_TRACE, "vlv_filter_candidates", "Filtering done\n");

    return return_value;
}

/*
 * Given a candidate list and a virtual list view specification, trim the candidate list
 *
 * Returns:
 *       success (0),
 *       operationsError (1),
 *       unwillingToPerform (53),
 *       timeLimitExceeded (3),
 *       adminLimitExceeded (11),
 *       indexRangeError (61),
 *       other (80)
 */
int
vlv_trim_candidates_txn(backend *be, const IDList *candidates, const sort_spec *sort_control, const struct vlv_request *vlv_request_control, IDList **trimmedCandidates, struct vlv_response *vlv_response_control, back_txn *txn)
{
    IDList *resultIdl = NULL;
    int return_value = LDAP_SUCCESS;
    PRUint32 si = 0; /* The Selected Index */
    int do_trim = 1;

    /* Refuse to trim a non-existent IDlist */
    if (NULL == candidates || candidates->b_nids == 0 || NULL == trimmedCandidates) {
        return LDAP_UNWILLING_TO_PERFORM;
    }

    switch (vlv_request_control->tag) {
    case 0: /* byIndex */
        si = vlv_trim_candidates_byindex(candidates->b_nids, vlv_request_control);
        break;
    case 1: /* byValue */
        si = vlv_trim_candidates_byvalue(be, candidates, sort_control, vlv_request_control, txn);
        /* Don't bother sending results if the attribute value wasn't found */
        if (si == candidates->b_nids) {
            do_trim = 0;
            /* minimum idl_alloc size should be 1; 0 is considered ALLID */
            resultIdl = idl_alloc(1);
        }
        break;
    default:
        /* Some wierd tag value.  Shouldn't ever happen */
        if (ISLEGACY(be)) {
            return_value = LDAP_OPERATIONS_ERROR;
        } else {
            return_value = LDAP_VIRTUAL_LIST_VIEW_ERROR;
        }
        break;
    }

    /* Tell the client what the real content count is. Clients count from 1 */
    vlv_response_control->targetPosition = si + 1;
    vlv_response_control->contentCount = candidates->b_nids;

    if (return_value == LDAP_SUCCESS && do_trim) {
        /* Work out the range of records to return */
        PRUint32 start, stop;
        determine_result_range(vlv_request_control, si, candidates->b_nids, &start, &stop);
        /* Build a new list containing the (start..stop) range */
        /* JCM: Should really be a function in idlist.c to copy a range */
        resultIdl = idl_alloc(stop - start + 1);
        {
            PRUint32 cursor = 0;
            for (cursor = start; cursor <= stop; cursor++) {
                slapi_log_err(SLAPI_LOG_TRACE, "vlv_trim_candidates", "Include ID %lu\n", (u_long)candidates->b_ids[cursor]);
                idl_append(resultIdl, candidates->b_ids[cursor]);
            }
        }
    }
    slapi_log_err(SLAPI_LOG_TRACE, "vlv_trim_candidates",
                  "Trimmed list contains %lu entries.\n", (u_long)(resultIdl ? resultIdl->b_nids : 0));
    *trimmedCandidates = resultIdl;
    return return_value;
}

int
vlv_trim_candidates(backend *be, const IDList *candidates, const sort_spec *sort_control, const struct vlv_request *vlv_request_control, IDList **trimmedCandidates, struct vlv_response *vlv_response_control)
{
    return vlv_trim_candidates_txn(be, candidates, sort_control, vlv_request_control, trimmedCandidates, vlv_response_control, NULL);
}

/*
 * Work out the Selected Index given the length of the candidate list
 * and the request control from the client.
 *
 * If the client sends Index==0 we behave as if I=1
 * If the client sends Index==Size==1 we behave as if I=1, S=0
 */
static PRUint32
vlv_trim_candidates_byindex(PRUint32 length, const struct vlv_request *vlv_request_control)
{
    PRUint32 si = 0; /* The Selected Index */
    slapi_log_err(SLAPI_LOG_TRACE, "vlv_trim_candidates_byindex",
                  "length=%u index=%d size=%d\n", length, vlv_request_control->index, vlv_request_control->contentCount);
    if (vlv_request_control->index == 0) {
        /* Always select the first entry in the list */
        si = 0;
    } else {
        if (vlv_request_control->contentCount == 0) {
            /* The client has no idea what the content count might be. */
            /* Can't scale the index, so use as is */
            si = vlv_request_control->index;
            if (0 == length) /* 609377: index size could be 0 */
            {
                if (si > 0) {
                    si = length;
                }
            } else if (si > length - 1) {
                si = length - 1;
            }
        } else {
            if (vlv_request_control->index >= vlv_request_control->contentCount) {
                /* Always select the last entry in the list */
                if (0 == length) /* 609377: index size could be 0 */
                {
                    si = 0;
                } else {
                    si = length - 1;
                }
            } else {
                /* The three components of this expression are (PRUint32) and may well have a value up to UINT_MAX */
                /* SelectedIndex = ActualContentCount * ( ClientIndex / ClientContentCount ) */
                si = ((PRUint32)((double)length * (double)(vlv_request_control->index / (double)vlv_request_control->contentCount)));
            }
        }
    }
    slapi_log_err(SLAPI_LOG_TRACE, "vlv_trim_candidates_byindex", "Selected Index %u\n", si);
    return si;
}

/*
 * Iterate over the Candidate ID List looking for an entry >= the provided attribute value.
 */
static PRUint32
vlv_trim_candidates_byvalue(backend *be, const IDList *candidates, const sort_spec *sort_control, const struct vlv_request *vlv_request_control, back_txn *txn)
{
    PRUint32 si = 0; /* The Selected Index */
    PRUint32 low = 0;
    PRUint32 high = 0;
    PRUint32 current = 0;
    ID id = NOID;
    int found = 0;
    struct berval **typedown_value = NULL;

    /* For non-matchrule indexing */
    value_compare_fn_type compare_fn = NULL;

    /*
     * If the primary sorted attribute has an associated
     * matching rule, then we must mangle the typedown
     * value.
     */
    if (sort_control->matchrule == NULL) {
        attr_get_value_cmp_fn(&sort_control->sattr, &compare_fn);
        if (compare_fn == NULL) {
            slapi_log_err(SLAPI_LOG_WARNING, "vlv_trim_candidates_byvalue",
                          "Attempt to compare an unordered attribute [%s]\n",
                          sort_control->type);
            compare_fn = slapi_berval_cmp;
        }

        {
            struct berval *invalue[2];
            invalue[0] = (struct berval *)&vlv_request_control->value; /* jcm: cast away const */
            invalue[1] = NULL;
            slapi_attr_values2keys(&sort_control->sattr, invalue, &typedown_value, LDAP_FILTER_EQUALITY); /* JCM SLOW FUNCTION */
            if (compare_fn == NULL) {
                slapi_log_err(SLAPI_LOG_WARNING, "vlv_trim_candidates_byvalue",
                              "Attempt to compare an unordered attribute\n");
                compare_fn = slapi_berval_cmp;
            }
        }
    } else {
        typedown_value = vlv_create_matching_rule_value(sort_control->mr_pb, (struct berval *)&vlv_request_control->value);
        compare_fn = slapi_berval_cmp;
    }
retry:
    /*
     * Perform a binary search over the candidate list
     */
    if (0 == candidates->b_nids) { /* idlist is empty */
        slapi_log_err(SLAPI_LOG_ERR, "vlv_trim_candidates_byvalue", "Candidate ID List is empty.\n");
        ber_bvecfree((struct berval **)typedown_value);
        return candidates->b_nids; /* not found */
    }
    low = 0;
    high = candidates->b_nids - 1;
    do {
        int err = 0;
        struct backentry *e = NULL;
        if (!sort_control->order) {
            current = (low + high) / 2;
        } else {
            current = (1 + low + high) / 2;
        }
        id = candidates->b_ids[current];
        e = id2entry(be, id, txn, &err);
        if (e == NULL) {
            int rval;
            slapi_log_err(SLAPI_LOG_ERR, "vlv_trim_candidates_byvalue",
                          "Candidate ID %lu not found err=%d\n", (u_long)id, err);
            rval = idl_delete((IDList **)&candidates, id);
            if (0 == rval || 1 == rval || 2 == rval) {
                goto retry;
            } else {
                ber_bvecfree((struct berval **)typedown_value);
                return candidates->b_nids; /* not found */
            }
        } else {
            /* Check if vlv_request_control->value is greater than or equal to the primary key. */
            int match;
            Slapi_Attr *attr;
            if ((NULL != compare_fn) && (slapi_entry_attr_find(e->ep_entry, sort_control->type, &attr) == 0)) {
                /*
                 * If there's a matching rule associated with the primary
                 * attribute then use the indexer to mangle the attr values.
                 */
                Slapi_Value **csn_value = valueset_get_valuearray(&attr->a_present_values);
                struct berval **entry_value = /* xxxPINAKI needs modification attr->a_vals */ NULL;
                PRBool needFree = PR_FALSE;

                if (sort_control->mr_pb != NULL) {
                    /* Matching rule. Do the magic mangling. Plugin owns the memory. */
                    matchrule_values_to_keys(sort_control->mr_pb, csn_value, &entry_value);
                } else {
                    valuearray_get_bervalarray(csn_value, &entry_value);
                    needFree = PR_TRUE; /* entry_value is a copy */
                }
                if (!sort_control->order) {
                    match = sort_attr_compare(entry_value, (struct berval **)typedown_value, compare_fn);
                } else {
                    match = sort_attr_compare((struct berval **)typedown_value, entry_value, compare_fn);
                }
                if (needFree) {
                    ber_bvecfree((struct berval **)entry_value);
                    entry_value = NULL;
                }
            } else {
                /*
                 * This attribute doesn't exist on this entry.
                 */
                if (sort_control->order) {
                    match = 1;
                } else {
                    match = 0;
                }
            }
            if (!sort_control->order) {
                if (match >= 0) {
                    high = current;
                } else {
                    low = current + 1;
                }
            } else {
                if (match >= 0) {
                    high = current - 1;
                } else {
                    low = current;
                }
            }
            if (low >= high) {
                found = 1;
                si = high;
                if (si == candidates->b_nids && !match) {
                    /* Couldn't find an entry which matches the value, so return contentCount */
                    slapi_log_err(SLAPI_LOG_TRACE, "vlv_trim_candidates_byvalue", "Not Found. Index %u\n", si);
                    si = candidates->b_nids;
                } else {
                    slapi_log_err(SLAPI_LOG_TRACE, "vlv_trim_candidates_byvalue", "Found. Index %u\n", si);
                }
            }
            CACHE_RETURN(&(((ldbm_instance *)be->be_instance_info)->inst_cache),
                         &e);
        }
    } while (!found);
    ber_bvecfree((struct berval **)typedown_value);
    return si;
}

/*
 * Encode the VLV RESPONSE control.
 *
 * Create a virtual list view response control,
 * and add it to the PBlock to be returned to the client.
 *
 * Returns:
 *   success ( 0 )
 *   operationsError (1),
 */
int
vlv_make_response_control(Slapi_PBlock *pb, const struct vlv_response *vlvp)
{
    BerElement *ber = NULL;
    struct berval *bvp = NULL;
    int rc = -1;

    /*
     VirtualListViewResponse ::= SEQUENCE {
             targetPosition    INTEGER (0 .. maxInt),
             contentCount     INTEGER (0 .. maxInt),
             virtualListViewResult ENUMERATED {
                     success (0),
                     operationsError (1),
                     unwillingToPerform (53),
                     insufficientAccessRights (50),
                     busy (51),
                     timeLimitExceeded (3),
                     adminLimitExceeded (11),
                     sortControlMissing (60),
                     indexRangeError (61),
                     other (80) }  }
     */

    if ((ber = ber_alloc()) == NULL) {
        return rc;
    }

    rc = ber_printf(ber, "{iie}", vlvp->targetPosition, vlvp->contentCount, vlvp->result);
    if (rc != -1) {
        rc = ber_flatten(ber, &bvp);
    }

    ber_free(ber, 1);

    if (rc != -1) {
        LDAPControl new_ctrl = {0};
        new_ctrl.ldctl_oid = LDAP_CONTROL_VLVRESPONSE;
        new_ctrl.ldctl_value = *bvp;
        new_ctrl.ldctl_iscritical = 1;
        rc = slapi_pblock_set(pb, SLAPI_ADD_RESCONTROL, &new_ctrl);
        ber_bvfree(bvp);
    }

    slapi_log_err(SLAPI_LOG_TRACE, "vlv_make_response_control", "Index=%d Size=%d Result=%d\n",
                  vlvp->targetPosition, vlvp->contentCount, vlvp->result);

    return (rc == -1 ? LDAP_OPERATIONS_ERROR : LDAP_SUCCESS);
}

/*
 * Generate a logging string for the vlv request and response
 */
void
vlv_print_access_log(Slapi_PBlock *pb,
                     struct vlv_request *vlvi,
                     struct vlv_response *vlvo,
                     sort_spec_thing *sort_control)
{
    #define NUMLEN 10 /* 32 bit integer maximum lenght (i.e minus + up to 9 digits) */
    char resp_status[3*NUMLEN+5];
    char buffer[4+NUMLEN*3+4+sizeof resp_status];
    int32_t log_format = config_get_accesslog_log_format();

    if (log_format != LOG_FORMAT_DEFAULT) {
        slapd_log_pblock logpb = {0};

        slapd_log_pblock_init(&logpb, log_format, pb);
        logpb.vlv_req_before_count = vlvi->beforeCount;
        logpb.vlv_req_after_count = vlvi->afterCount;
        logpb.vlv_req_content_count = vlvi->contentCount;
        logpb.vlv_req_index = vlvi->index;
        logpb.vlv_req_value = vlvi->value.bv_val;
        logpb.vlv_req_value_len = vlvi->value.bv_len;
        if (sort_control) {
            logpb.vlv_sort_str = sort_log_access(pb, sort_control, NULL, PR_TRUE);
        } else {
            logpb.vlv_sort_str = slapi_ch_strdup("None ");
        }
        if (vlvo) {
            logpb.vlv_res_target_position = vlvo->targetPosition;
            logpb.vlv_res_content_count = vlvo->contentCount;
            logpb.vlv_res_result = vlvo->result;
        }
        slapd_log_access_vlv(&logpb);
        slapi_ch_free_string((char **)&logpb.vlv_sort_str);
    } else {
        /* Prepare VLV response */
        if (vlvo == NULL) {
            strcpy(resp_status, "None");
        } else {
            sprintf(resp_status, "%d:%d (%d)",
                    vlvo->targetPosition,
                    vlvo->contentCount,
                    vlvo->result);
        }

        /* Prepare VLV result + response*/
        if (0 == vlvi->tag) {
            PR_snprintf(buffer, (sizeof buffer), "VLV %d:%d:%d:%d %s",
                        vlvi->beforeCount,
                        vlvi->afterCount,
                        vlvi->index,
                        vlvi->contentCount,
                        resp_status);
            ldbm_log_access_message(pb, buffer);
        } else {
            char fmt[18+NUMLEN];
            char *msg = NULL;
            PR_snprintf(fmt, (sizeof fmt), "VLV %%d:%%d:%%.%lds %%s", vlvi->value.bv_len);

            msg = slapi_ch_smprintf(fmt,
                                    vlvi->beforeCount,
                                    vlvi->afterCount,
                                    vlvi->value.bv_val,
                                    resp_status);
            ldbm_log_access_message(pb, msg);
            slapi_ch_free_string(&msg);
        }
        if (sort_control) {
            sort_log_access(pb, sort_control, NULL, PR_FALSE);
        }
    }
}

/*
 * Decode the VLV REQUEST control.
 *
 * If the client sends Index==0 we behave as if I=1
 *
 * Returns:
 *       success (0),
 *       operationsError (1),
 *
 */
int
vlv_parse_request_control(backend *be, struct berval *vlv_spec_ber, struct vlv_request *vlvp)
{
    /* This control looks like this :

     VirtualListViewRequest ::= SEQUENCE {
             beforeCount    INTEGER (0 .. maxInt),
             afterCount     INTEGER (0 .. maxInt),
             CHOICE {
                     byIndex [0] SEQUENCE {
                     index           INTEGER (0 .. maxInt),
                     contentCount    INTEGER (0 .. maxInt) }
                     greaterThanOrEqual [1] assertionValue }
       */
    BerElement *ber = NULL;
    int return_value = LDAP_SUCCESS;

    vlvp->value.bv_len = 0;
    vlvp->value.bv_val = NULL;

    if (!BV_HAS_DATA(vlv_spec_ber)) {
        return_value = LDAP_OPERATIONS_ERROR;
        return return_value;
    }

    ber = ber_init(vlv_spec_ber);
    if (ber_scanf(ber, "{ii", &vlvp->beforeCount, &vlvp->afterCount) == LBER_ERROR) {
        return_value = LDAP_OPERATIONS_ERROR;
    } else {
        slapi_log_err(SLAPI_LOG_TRACE, "vlv_parse_request_control", "Before=%d After=%d\n",
                      vlvp->beforeCount, vlvp->afterCount);
        if (ber_scanf(ber, "t", &vlvp->tag) == LBER_ERROR) {
            return_value = LDAP_OPERATIONS_ERROR;
        } else {
            switch (vlvp->tag) {
            case LDAP_TAG_VLV_BY_INDEX:
                /* byIndex */
                vlvp->tag = 0;
                if (ber_scanf(ber, "{ii}}", &vlvp->index, &vlvp->contentCount) == LBER_ERROR) {
                    if (ISLEGACY(be)) {
                        return_value = LDAP_OPERATIONS_ERROR;
                    } else {
                        return_value = LDAP_VIRTUAL_LIST_VIEW_ERROR;
                    }
                } else {
                    /* Client Counts from 1. */
                    if (vlvp->index != 0) {
                        vlvp->index--;
                    }
                    slapi_log_err(SLAPI_LOG_TRACE, "vlv_parse_request_control", "Index=%d Content=%d\n",
                                  vlvp->index, vlvp->contentCount);
                }
                break;
            case LDAP_TAG_VLV_BY_VALUE:
                /* byValue */
                vlvp->tag = 1;
                if (ber_scanf(ber, "o}", &vlvp->value) == LBER_ERROR) {
                    if (ISLEGACY(be)) {
                        return_value = LDAP_OPERATIONS_ERROR;
                    } else {
                        return_value = LDAP_VIRTUAL_LIST_VIEW_ERROR;
                    }
                }
                {
                    /* jcm: isn't there a utility fn to do this? */
                    char *p = slapi_ch_malloc(vlvp->value.bv_len + 1);
                    strncpy(p, vlvp->value.bv_val, vlvp->value.bv_len);
                    p[vlvp->value.bv_len] = '\0';
                    slapi_log_err(SLAPI_LOG_TRACE, "vlv_parse_request_control", "Value=%s\n", p);
                    slapi_ch_free((void **)&p);
                }
                break;
            default:
                if (ISLEGACY(be)) {
                    return_value = LDAP_OPERATIONS_ERROR;
                } else {
                    return_value = LDAP_VIRTUAL_LIST_VIEW_ERROR;
                }
            }
        }
    }

    /* the ber encoding is no longer needed */
    ber_free(ber, 1);

    return return_value;
}

/* given a slapi_filter, check if there's a vlv index that matches that
 * filter.  if so, return the IDL for that index (else return NULL).
 * -- a vlv index will match ONLY if that vlv index is subtree-scope and
 * has the same search base and search filter.
 * added read lock */

IDList *
vlv_find_index_by_filter_txn(struct backend *be, const char *base, Slapi_Filter *f, back_txn *txn)
{
    struct vlvSearch *t = NULL;
    struct vlvIndex *vi;
    Slapi_DN base_sdn;
    PRUint32 length;
    int err;
    dbi_db_t *db = NULL;
    dbi_cursor_t dbc = {0};
    IDList *idl;
    Slapi_Filter *vlv_f;
    dbi_txn_t *db_txn = NULL;

    if (txn) {
        db_txn = txn->back_txn_txn;
    }

    slapi_sdn_init_dn_byref(&base_sdn, base);
    slapi_rwlock_rdlock(be->vlvSearchList_lock);
    for (t = (struct vlvSearch *)be->vlvSearchList; t; t = t->vlv_next) {
        /* all vlv "filters" start with (|(xxx)(objectclass=referral)).
         * we only care about the (xxx) part.
         */
        vlv_f = t->vlv_slapifilter->f_or;
        if ((t->vlv_scope == LDAP_SCOPE_SUBTREE) &&
            (slapi_sdn_compare(t->vlv_base, &base_sdn) == 0) &&
            (slapi_filter_compare(vlv_f, f) == 0)) {
            /* found match! */
            slapi_sdn_done(&base_sdn);

            /* is there an index that's ready? */
            vi = t->vlv_index;
            while (!vlvIndex_online(vi) && vi) {
                vi = vi->vlv_next;
            }
            if (!vi) {
                /* no match */
                slapi_log_err(SLAPI_LOG_TRACE, "vlv_find_index_by_filter_txn", "No index online for %s\n",
                              t->vlv_filter);
                slapi_rwlock_unlock(be->vlvSearchList_lock);
                return NULL;
            }

            if (dblayer_get_index_file(be, vi->vlv_attrinfo, &db, 0) == 0) {
                length = vlvIndex_get_indexlength(be, vi, db, 0 /* txn */);
                slapi_rwlock_unlock(be->vlvSearchList_lock);
                err = dblayer_new_cursor(be, db, db_txn, &dbc);
                if (err == 0) {
                    if (length == 0) /* 609377: index size could be 0 */
                    {
                        slapi_log_err(SLAPI_LOG_TRACE, "vlv_find_index_by_filter_txn", "Index %s is empty\n",
                                      t->vlv_filter);
                        idl = NULL;
                    } else {
                        err = vlv_build_idl(be, 0, length - 1, db, &dbc, &idl, 1 /* dosort */);
                    }
                    dblayer_cursor_op(&dbc, DBI_OP_CLOSE, NULL, NULL);
                }
                dblayer_release_index_file(be, vi->vlv_attrinfo, db);
                if (err == 0) {
                    return idl;
                } else {
                    slapi_log_err(SLAPI_LOG_ERR, "vlv_find_index_by_filter_txn", "vlv find index: err %d\n",
                                  err);
                    return NULL;
                }
            }
        }
    }
    slapi_rwlock_unlock(be->vlvSearchList_lock);
    /* no match */
    slapi_sdn_done(&base_sdn);
    return NULL;
}

IDList *
vlv_find_index_by_filter(struct backend *be, const char *base, Slapi_Filter *f)
{
    return vlv_find_index_by_filter_txn(be, base, f, NULL);
}

/* similar to what the console GUI does */

char *
create_vlv_search_tag(const char *dn)
{
    char *tmp2 = slapi_ch_strdup(dn);

    replace_char(tmp2, ',', ' ');
    replace_char(tmp2, '"', '-');
    replace_char(tmp2, '+', '_');
    return tmp2;
}

/* Builds strings from Slapi_DN similar console GUI. Uses those dns to
   delete vlvsearch's if they match. New write lock.
 */
int
vlv_delete_search_entry(Slapi_PBlock *pb __attribute__((unused)), Slapi_Entry *e, ldbm_instance *inst)
{
    int rc = 0;
    Slapi_PBlock *tmppb;
    Slapi_DN *newdn;
    struct vlvSearch *p = NULL;
    char *base1 = NULL, *base2 = NULL, *tag1 = NULL, *tag2 = NULL;
    const char *dn = slapi_sdn_get_dn(&e->e_sdn);
    backend *be = NULL;
    if (NULL == inst) {
        return LDAP_OPERATIONS_ERROR;
    }
    be = inst->inst_be;

    if (instance_set_busy(inst) != 0) {
        slapi_log_err(SLAPI_LOG_ERR,
                      "vlv_delete_search_entry",
                      "Backend instance: '%s' is already in the middle of "
                      "another task and cannot be disturbed.\n",
                      inst->inst_name);
        return LDAP_OPERATIONS_ERROR;
    }
    tag1 = create_vlv_search_tag(dn);
    base1 = slapi_create_dn_string("cn=MCC %s,cn=%s,cn=%s,cn=plugins,cn=config",
                                   tag1, inst->inst_name, inst->inst_li->li_plugin->plg_name);
    if (NULL == base1) {
        slapi_log_err(SLAPI_LOG_ERR,
                      "vlv_delete_search_entry",
                      "failed to craete vlv search entry dn (rdn: cn=MCC %s) for "
                      "plugin %s, instance %s\n",
                      tag1, inst->inst_li->li_plugin->plg_name, inst->inst_name);
        rc = LDAP_PARAM_ERROR;
        goto bail;
    }
    newdn = slapi_sdn_new_dn_byval(base1);
    /* vlvSearchList is modified; need Wlock */
    slapi_rwlock_wrlock(be->vlvSearchList_lock);
    p = vlvSearch_finddn((struct vlvSearch *)be->vlvSearchList, newdn);
    if (p != NULL) {
        slapi_log_err(SLAPI_LOG_ERR, "vlv_delete_search_entry",
                      "Deleted Virtual List View Search (%s).\n", p->vlv_name);
        tag2 = create_vlv_search_tag(dn);
        base2 = slapi_create_dn_string("cn=by MCC %s,%s", tag2, base1);
        if (NULL == base2) {
            slapi_log_err(SLAPI_LOG_ERR,
                          "vlv_delete_search_entry", "Failed to create "
                                                     "vlv search entry dn (rdn: cn=by MCC %s) for "
                                                     "plugin %s, instance %s\n",
                          tag2, inst->inst_li->li_plugin->plg_name, inst->inst_name);
            rc = LDAP_PARAM_ERROR;
            slapi_ch_free((void **)&tag2);
            slapi_rwlock_unlock(be->vlvSearchList_lock);
            goto bail;
        }
        vlvSearch_removefromlist((struct vlvSearch **)&be->vlvSearchList, p->vlv_dn);
        /* This line release lock to prevent recursive deadlock caused by slapi_internal_delete calling vlvDeleteSearchEntry */
        slapi_rwlock_unlock(be->vlvSearchList_lock);
        vlvSearch_delete(&p);
        tmppb = slapi_pblock_new();
        slapi_delete_internal_set_pb(tmppb, base2, NULL, NULL,
                                     (void *)plugin_get_default_component_id(), 0);
        slapi_delete_internal_pb(tmppb);
        slapi_pblock_get(tmppb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
        if (rc != LDAP_SUCCESS) {
            slapi_log_err(SLAPI_LOG_ERR, "vlv_delete_search_entry", "Can't delete dse entry '%s' error %d\n", base2, rc);
        }
        pblock_done(tmppb);
        pblock_init(tmppb);
        slapi_delete_internal_set_pb(tmppb, base1, NULL, NULL,
                                     (void *)plugin_get_default_component_id(), 0);
        slapi_delete_internal_pb(tmppb);
        slapi_pblock_get(tmppb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
        if (rc != LDAP_SUCCESS) {
            slapi_log_err(SLAPI_LOG_ERR, "vlv_delete_search_entry", "Can't delete dse entry '%s' error %d\n", base1, rc);
        }
        slapi_pblock_destroy(tmppb);
        slapi_ch_free((void **)&tag2);
        slapi_ch_free((void **)&base2);
    } else {
        slapi_rwlock_unlock(be->vlvSearchList_lock);
    }
bail:
    instance_set_not_busy(inst);
    slapi_ch_free((void **)&tag1);
    slapi_ch_free((void **)&base1);
    slapi_sdn_free(&newdn);
    return rc;
}

void
vlv_acquire_lock(backend *be)
{
    slapi_log_err(SLAPI_LOG_TRACE, "vlv_acquire_lock", "Trying to acquire the lock\n");
    slapi_rwlock_wrlock(be->vlvSearchList_lock);
}

void
vlv_release_lock(backend *be)
{
    slapi_log_err(SLAPI_LOG_TRACE, "vlv_release_lock", "Trying to release the lock\n");
    slapi_rwlock_unlock(be->vlvSearchList_lock);
}
