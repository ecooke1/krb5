/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* plugins/kdb/test/kdb_test.c - Test KDB module */
/*
 * Copyright (C) 2015 by the Massachusetts Institute of Technology.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This is a read-only KDB module intended to help test KDC behavior which
 * cannot be exercised with the DB2 module.  Responses are read from the
 * dbmodules subsection according to this example:
 *
 *     [dbmodules]
 *         test = {
 *             alias = {
 *                 aliasname = canonname
 *             }
 *             princs = {
 *                 krbtgt/KRBTEST.COM = {
 *                     flags = +preauth +ok-to-auth-as-delegate
 *                     maxlife = 1d
 *                     maxrenewlife = 7d
 *                     expiration = 14d # relative to current time
 *                     pwexpiration = 1h
 *                     # Initial number is kvno; defaults to 1.
 *                     keys = 3 aes256-cts aes128-cts:normal
 *                     keys = 2 rc4-hmac
 *                 }
 *             }
 *             delegation = {
 *                 intermediate_service = target_service
 *             }
 *         }
 *
 * Key values are generated using a hash of the kvno, enctype, salt type, and
 * principal name.  This module does not use master key encryption, so it
 * serves as a partial test of the DAL's ability to avoid that.
 */

#include "k5-int.h"
#include "kdb5.h"
#include "adm_proto.h"
#include <ctype.h>

typedef struct {
    void *profile;
    char *section;
    const char *names[6];
} *testhandle;

static void *
ealloc(size_t sz)
{
    void *p = calloc(sz, 1);

    if (p == NULL)
        abort();
    return p;
}

static char *
estrdup(const char *s)
{
    char *copy = strdup(s);

    if (copy == NULL)
        abort();
    return copy;
}

static void
check(krb5_error_code code)
{
    if (code != 0)
        abort();
}

/* Set up for a profile query using h->names.  Look up s1 -> s2 -> s3 (some of
 * which may be NULL) within this database's dbmodules section. */
static void
set_names(testhandle h, const char *s1, const char *s2, const char *s3)
{
    h->names[0] = KDB_MODULE_SECTION;
    h->names[1] = h->section;
    h->names[2] = s1;
    h->names[3] = s2;
    h->names[4] = s3;
    h->names[5] = NULL;
}

/* Look up a string within this database's dbmodules section. */
static char *
get_string(testhandle h, const char *s1, const char *s2, const char *s3)
{
    krb5_error_code ret;
    char **values, *val;

    set_names(h, s1, s2, s3);
    ret = profile_get_values(h->profile, h->names, &values);
    if (ret == PROF_NO_RELATION)
        return NULL;
    if (ret)
        abort();
    val = estrdup(values[0]);
    profile_free_list(values);
    return val;
}

/* Look up a duration within this database's dbmodules section. */
static krb5_deltat
get_duration(testhandle h, const char *s1, const char *s2, const char *s3)
{
    char *strval = get_string(h, s1, s2, s3);
    krb5_deltat val;

    if (strval == NULL)
        return 0;
    check(krb5_string_to_deltat(strval, &val));
    free(strval);
    return val;
}

/* Look up an absolute time within this database's dbmodules section.  The time
 * is expressed in the profile as an interval relative to the current time. */
static krb5_timestamp
get_time(testhandle h, const char *s1, const char *s2, const char *s3)
{
    char *strval = get_string(h, s1, s2, s3);
    krb5_deltat val;

    if (strval == NULL)
        return 0;
    check(krb5_string_to_deltat(strval, &val));
    free(strval);
    return val + time(NULL);
}

/* Initialize kb_out with a key of type etype, using a hash of kvno, etype,
 * salttype, and princstr for the key bytes. */
static void
make_keyblock(krb5_kvno kvno, krb5_enctype etype, int32_t salttype,
              const char *princstr, krb5_keyblock *kb_out)
{
    size_t keybytes, keylength, pos, n;
    char *hashstr;
    krb5_data d, rndin;
    krb5_checksum cksum;

    check(krb5_c_keylengths(NULL, etype, &keybytes, &keylength));
    alloc_data(&rndin, keybytes);

    /* Hash the kvno, enctype, salt type, and principal name together. */
    if (asprintf(&hashstr, "%d %d %d %s", (int)kvno, (int)etype,
                 (int)salttype, princstr) < 0)
        abort();
    d = string2data(hashstr);
    check(krb5_c_make_checksum(NULL, CKSUMTYPE_NIST_SHA, NULL, 0, &d, &cksum));

    /* Make the appropriate number of input bytes from the hash result. */
    for (pos = 0; pos < keybytes; pos += n) {
        n = (cksum.length < keybytes - pos) ? cksum.length : keybytes - pos;
        memcpy(rndin.data + pos, cksum.contents, n);
    }

    kb_out->enctype = etype;
    kb_out->length = keylength;
    kb_out->contents = ealloc(keylength);
    check(krb5_c_random_to_key(NULL, etype, &rndin, kb_out));
    free(cksum.contents);
    free(rndin.data);
    free(hashstr);
}

/* Return key data for the given key/salt tuple strings, using hashes of the
 * enctypes, salts, and princstr for the key contents. */
static void
make_keys(char **strings, const char *princstr, krb5_db_entry *ent)
{
    krb5_key_data *key_data, *kd;
    krb5_keyblock kb;
    int32_t *ks_list_sizes, nstrings, nkeys, i, j;
    krb5_key_salt_tuple **ks_lists, *ks;
    krb5_kvno *kvnos;
    char *s;

    for (nstrings = 0; strings[nstrings] != NULL; nstrings++);
    ks_lists = ealloc(nstrings * sizeof(*ks_lists));
    ks_list_sizes = ealloc(nstrings * sizeof(*ks_list_sizes));
    kvnos = ealloc(nstrings * sizeof(*kvnos));

    /* Convert each string into a key/salt tuple list and count the total
     * number of key data structures needed. */
    nkeys = 0;
    for (i = 0; i < nstrings; i++) {
        s = strings[i];
        /* Read a leading kvno if present; otherwise assume kvno 1. */
        if (isdigit(*s)) {
            kvnos[i] = strtol(s, &s, 10);
            while (isspace(*s))
                s++;
        } else {
            kvnos[i] = 1;
        }
        check(krb5_string_to_keysalts(s, NULL, NULL, FALSE, &ks_lists[i],
                                      &ks_list_sizes[i]));
        nkeys += ks_list_sizes[i];
    }

    /* Turn each key/salt tuple into a key data entry. */
    kd = key_data = ealloc(nkeys * sizeof(*kd));
    for (i = 0; i < nstrings; i++) {
        ks = ks_lists[i];
        for (j = 0; j < ks_list_sizes[i]; j++) {
            make_keyblock(kvnos[i], ks[j].ks_enctype, ks[j].ks_salttype,
                          princstr, &kb);
            kd->key_data_ver = 2;
            kd->key_data_kvno = kvnos[i];
            kd->key_data_type[0] = ks[j].ks_enctype;
            kd->key_data_length[0] = kb.length;
            kd->key_data_contents[0] = kb.contents;
            kd->key_data_type[1] = ks[j].ks_salttype;
            kd++;
        }
    }

    for (i = 0; i < nstrings; i++)
        free(ks_lists[i]);
    free(ks_lists);
    free(ks_list_sizes);
    free(kvnos);
    ent->key_data = key_data;
    ent->n_key_data = nkeys;
}

static krb5_error_code
test_init()
{
    return 0;
}

static krb5_error_code
test_cleanup()
{
    return 0;
}

static krb5_error_code
test_open(krb5_context context, char *conf_section, char **db_args, int mode)
{
    testhandle h;

    h = ealloc(sizeof(*h));
    h->profile = context->profile;
    h->section = estrdup(conf_section);
    context->dal_handle->db_context = h;
    return 0;
}

static krb5_error_code
test_close(krb5_context context)
{
    testhandle h = context->dal_handle->db_context;

    free(h->section);
    free(h);
    return 0;
}

static krb5_error_code
test_get_principal(krb5_context context, krb5_const_principal search_for,
                   unsigned int flags, krb5_db_entry **entry)
{
    krb5_error_code ret;
    krb5_principal_data empty_princ = { KV5M_PRINCIPAL };
    testhandle h = context->dal_handle->db_context;
    char *search_name, *canon, *flagstr, **names, **key_strings;
    const char *ename;
    krb5_db_entry *ent;

    *entry = NULL;

    check(krb5_unparse_name_flags(context, search_for,
                                  KRB5_PRINCIPAL_UNPARSE_NO_REALM,
                                  &search_name));
    canon = get_string(h, "alias", search_name, NULL);
    ename = (canon != NULL) ? canon : search_name;

    /* Check that the entry exists. */
    set_names(h, "princs", ename, NULL);
    ret = profile_get_relation_names(h->profile, h->names, &names);
    if (ret == PROF_NO_RELATION) {
        free(canon);
        return KRB5_KDB_NOENTRY;
    }
    profile_free_list(names);

    ent = ealloc(sizeof(*ent));

    check(krb5_parse_name(context, ename, &ent->princ));

    flagstr = get_string(h, "princs", ename, "flags");
    if (flagstr != NULL)
        check(krb5_string_to_flags(flagstr, "+", "-", &ent->attributes));
    free(flagstr);

    ent->max_life = get_duration(h, "princs", ename, "maxlife");
    ent->max_renewable_life = get_duration(h, "princs", ename, "maxrenewlife");
    ent->expiration = get_time(h, "princs", ename, "expiration");
    ent->pw_expiration = get_time(h, "princs", ename, "pwexpiration");

    /* Leave last_success, last_failed, fail_auth_count zeroed. */
    /* Leave tl_data and e_data empty. */

    set_names(h, "princs", ename, "keys");
    ret = profile_get_values(h->profile, h->names, &key_strings);
    if (ret != PROF_NO_RELATION) {
        make_keys(key_strings, ename, ent);
        profile_free_list(key_strings);
    }

    /* We must include mod-princ data or kadm5_get_principal() won't work and
     * we can't extract keys with kadmin.local. */
    check(krb5_dbe_update_mod_princ_data(context, ent, 0, &empty_princ));

    *entry = ent;
    free(canon);
    return 0;
}

static void
test_free_principal(krb5_context context, krb5_db_entry *entry)
{
    krb5_tl_data *tl, *next;
    int i, j;

    if (entry == NULL)
        return;
    free(entry->e_data);
    krb5_free_principal(context, entry->princ);
    for (tl = entry->tl_data; tl != NULL; tl = next) {
        next = tl->tl_data_next;
        free(tl->tl_data_contents);
        free(tl);
    }
    for (i = 0; i < entry->n_key_data; i++) {
        for (j = 0; j < entry->key_data[i].key_data_ver; j++) {
            if (entry->key_data[i].key_data_length[j]) {
                zapfree(entry->key_data[i].key_data_contents[j],
                        entry->key_data[i].key_data_length[j]);
            }
            entry->key_data[i].key_data_contents[j] = NULL;
            entry->key_data[i].key_data_length[j] = 0;
            entry->key_data[i].key_data_type[j] = 0;
        }
    }
    free(entry->key_data);
    free(entry);
}

static void *
test_alloc(krb5_context context, void *ptr, size_t size)
{
    return realloc(ptr, size);
}

static void
test_free(krb5_context context, void *ptr)
{
    free(ptr);
}

static krb5_error_code
test_fetch_master_key(krb5_context context, krb5_principal mname,
                      krb5_keyblock *key_out, krb5_kvno *kvno_out,
                      char *db_args)
{
    memset(key_out, 0, sizeof(*key_out));
    *kvno_out = 0;
    return 0;
}

static krb5_error_code
test_fetch_master_key_list(krb5_context context, krb5_principal mname,
                           const krb5_keyblock *key,
                           krb5_keylist_node **mkeys_out)
{
    /* krb5_dbe_get_mkvno() returns an error if we produce NULL, so return an
     * empty node to make kadm5_get_principal() work. */
    *mkeys_out = ealloc(sizeof(**mkeys_out));
    return 0;
}

static krb5_error_code
test_decrypt_key_data(krb5_context context, const krb5_keyblock *mkey,
                      const krb5_key_data *kd, krb5_keyblock *key_out,
                      krb5_keysalt *salt_out)
{
    key_out->magic = KV5M_KEYBLOCK;
    key_out->enctype = kd->key_data_type[0];
    key_out->length = kd->key_data_length[0];
    key_out->contents = ealloc(key_out->length);
    memcpy(key_out->contents, kd->key_data_contents[0], key_out->length);
    if (salt_out != NULL) {
        salt_out->type = (kd->key_data_ver > 1) ? kd->key_data_type[1] :
            KRB5_KDB_SALTTYPE_NORMAL;
        salt_out->data = empty_data();
    }
    return 0;
}

static krb5_error_code
test_encrypt_key_data(krb5_context context, const krb5_keyblock *mkey,
                      const krb5_keyblock *key, const krb5_keysalt *salt,
                      int kvno, krb5_key_data *kd_out)
{
    memset(kd_out, 0, sizeof(*kd_out));
    kd_out->key_data_ver = 2;
    kd_out->key_data_kvno = kvno;
    kd_out->key_data_type[0] = key->enctype;
    kd_out->key_data_length[0] = key->length;
    kd_out->key_data_contents[0] = ealloc(key->length);
    memcpy(kd_out->key_data_contents[0], key->contents, key->length);
    kd_out->key_data_type[1] = (salt != NULL) ? salt->type :
        KRB5_KDB_SALTTYPE_NORMAL;
    return 0;
}

static krb5_error_code
test_check_allowed_to_delegate(krb5_context context,
                               krb5_const_principal client,
                               const krb5_db_entry *server,
                               krb5_const_principal proxy)
{
    krb5_error_code ret;
    testhandle h = context->dal_handle->db_context;
    char *sprinc, *tprinc, **values, **v;
    krb5_boolean found = FALSE;

    check(krb5_unparse_name_flags(context, server->princ,
                                  KRB5_PRINCIPAL_UNPARSE_NO_REALM, &sprinc));
    check(krb5_unparse_name_flags(context, proxy,
                                  KRB5_PRINCIPAL_UNPARSE_NO_REALM, &tprinc));
    set_names(h, "delegation", sprinc, NULL);
    ret = profile_get_values(h->profile, h->names, &values);
    if (ret == PROF_NO_RELATION)
        return KRB5KDC_ERR_POLICY;
    for (v = values; *v != NULL; v++) {
        if (strcmp(*v, tprinc) == 0) {
            found = TRUE;
            break;
        }
    }
    profile_free_list(values);
    return found ? 0 : KRB5KDC_ERR_POLICY;
}

kdb_vftabl PLUGIN_SYMBOL_NAME(krb5_test, kdb_function_table) = {
    KRB5_KDB_DAL_MAJOR_VERSION,             /* major version number */
    0,                                      /* minor version number 0 */
    test_init,
    test_cleanup,
    test_open,
    test_close,
    NULL, /* create */
    NULL, /* destroy */
    NULL, /* get_age */
    NULL, /* lock */
    NULL, /* unlock */
    test_get_principal,
    test_free_principal,
    NULL, /* put_principal */
    NULL, /* delete_principal */
    NULL, /* iterate */
    NULL, /* create_policy */
    NULL, /* get_policy */
    NULL, /* put_policy */
    NULL, /* iter_policy */
    NULL, /* delete_policy */
    NULL, /* free_policy */
    test_alloc,
    test_free,
    test_fetch_master_key,
    test_fetch_master_key_list,
    NULL, /* store_master_key_list */
    NULL, /* dbe_search_enctype */
    NULL, /* change_pwd */
    NULL, /* promote_db */
    test_decrypt_key_data,
    test_encrypt_key_data,
    NULL, /* sign_authdata */
    NULL, /* check_transited_realms */
    NULL, /* check_policy_as */
    NULL, /* check_policy_tgs */
    NULL, /* audit_as_req */
    NULL, /* refresh_config */
    test_check_allowed_to_delegate
};
