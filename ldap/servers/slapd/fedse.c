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

/*
 * fedse.c - Front End DSE (DSA-Specific Entry) persistent storage.
 *
 * The DSE store is an LDIF file contained in the file dse.ldif.
 * The file is located in the directory specified with '-D'
 * when staring the server.
 *
 * In core, the DSEs are stored in an AVL tree, keyed on
 * DN.  Whenever a modification is made to a DSE, the
 * in-core entry is updated, then dse_write_file() is
 * called to commit the changes to disk.
 *
 * This is designed for a small number of DSEs, say
 * a maximum of 10 or 20.  Currently, there is only
 * one DSE, the root DSE.  If large numbers of DSEs
 * need to be stored, this approach of writing out
 * the entire contents on every modification will
 * be insufficient.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <prio.h>
#include <prcountr.h>
#include <dlfcn.h>
#include "slap.h"
#include "fe.h"
#include <pwd.h>

extern char **getSupportedCiphers(void);
extern char **getEnabledCiphers(void);
extern int getSSLVersionInfo(int *ssl2, int *ssl3, int *tls1);
extern int getSSLVersionRange(char **min, char **max);

static struct slapdplugin fedse_plugin = {0};

/* Note: These DNs are no need to be normalized */
static const char *internal_entries[] =
    {
        "dn:\n"
        "objectclass: top\n"
        "aci: (targetattr != \"aci\")(version 3.0; aci \"rootdse anon read access\"; allow(read,search,compare) userdn=\"ldap:///anyone\";)\n",

        "dn:oid=2.16.840.1.113730.3.4.9,cn=features,cn=config\n"
        "objectclass:top\n"
        "objectclass:directoryServerFeature\n"
        "oid:2.16.840.1.113730.3.4.9\n"
        "cn: VLV Request Control\n"
        "aci: (targetattr != \"aci\")(version 3.0; acl \"VLV Request Control\"; allow( read, search, compare, proxy ) userdn = \"ldap:///all\";)\n",

        "dn:oid=" EXTOP_BULK_IMPORT_START_OID ",cn=features,cn=config\n"
        "objectclass:top\n"
        "objectclass:directoryServerFeature\n"
        "cn: Bulk Import\n",

        "dn:cn=options,cn=features,cn=config\n"
        "objectclass:top\n"
        "objectclass:nsContainer\n"
        "cn:options\n",

        "dn:cn=encryption,cn=config\n"
        "objectclass:top\n"
        "objectclass:nsEncryptionConfig\n"
        "cn:encryption\n"
        "nsSSLSessionTimeout:0\n"
        "nsSSLClientAuth:allowed\n"
        "sslVersionMin:TLS1.0\n",

        "dn:cn=monitor\n"
        "objectclass:top\n"
        "objectclass:extensibleObject\n"
        "cn:monitor\n"
        "aci: (target =\"ldap:///cn=monitor*\")(targetattr != \"aci || connection\")(version 3.0; acl \"monitor\"; allow( read, search, compare ) userdn = \"ldap:///anyone\";)\n",

        "dn:cn=snmp,cn=monitor\n"
        "objectclass:top\n"
        "objectclass:extensibleObject\n"
        "cn:snmp\n",

        "dn:cn=counters,cn=monitor\n"
        "objectclass:top\n"
        "objectclass:extensibleObject\n"
        "cn:counters\n",

        "dn:cn=sasl,cn=config\n"
        "objectclass:top\n"
        "objectclass:nsContainer\n"
        "cn:sasl\n",

        "dn:cn=mapping,cn=sasl,cn=config\n"
        "objectclass:top\n"
        "objectclass:nsContainer\n"
        "cn:mapping\n",

        "dn:cn=SNMP,cn=config\n"
        "objectclass:top\n"
        "objectclass:nsSNMP\n"
        "cn:SNMP\n"
        "nsSNMPEnabled: on\n"};

static int NUM_INTERNAL_ENTRIES = sizeof(internal_entries) / sizeof(internal_entries[0]);

static char *easter_egg_entry =
    {
        "1E14405A150F47341F0E09191B0A1F5A3E13081F190E1508035A2E1F1B1756191447171514"
        "130E1508701518101F190E39161B0909405A0E150A701518101F190E39161B0909405A1508"
        "1D1B1413001B0E1315141B162F14130E701518101F190E39161B0909405A1E13081F190E15"
        "0803570E1F1B17571F020E1F14091318161F571518101F190E70150F405A341F0E09191B0A"
        "1F5A291F190F08130E035A2915160F0E1315140970150F405A341F0E09191B0A1F5A3E1308"
        "1F190E1508035A2E1F1B17701E1F091908130A0E131514405A3E1B0C131E5A3815081F121B"
        "17565A301B190B0F1F1613141F5A3815081F121B17565A3B140E121514035A3C15020D1508"
        "0E12565A3B161511705A5A3D15141E121B161F111B08565A3508161B5A321F1D1B080E0356"
        "5A3415081311155A3215091513565A341B0E121B145A3113141E1F08565A3E1F15145A361B"
        "19111F0356705A5A2E1215171B095A361B19111F03565A281319125A371F1D1D1314091514"
        "565A2D1316165A371508081309565A3F161613150E5A291912161F1D1F161713161912565A"
        "705A5A371B08115A2917130E12565A5A2815185A2D1F160E171B14565A2F161C5A2D1F160E"
        "171B14565A5A39121F090E15145A2D131616131B1709701E1F091908130A0E131514405A3B"
        "141E5A1B16165A0E121F5A150E121F08095A0D12155A121B0C1F5A1D15141F5A181F1C1508"
        "1F5A0F095470705A70707070"};

#define NUM_EASTER_EGG_PHOTOS 3

static const char *easter_egg_photo1 =
    "jpegphoto:: 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"
    "2Q==";

static const char *easter_egg_photo2 =
    "jpegphoto:: /9j/4AAQSkZJRgABAQAAAQABAAD//gBtQ1JFQVRPUjogWFYgVmVyc2lvbiAzLjEwYS"
    "BSZXY6IDEyLzI5Lzk0IChqcC1leHRlbnNpb24gNS4zLjMgKyBQTkcgcGF0Y2ggMS4yZCkgIFF1YWx"
    "pdHkgPSA1MSwgU21vb3RoaW5nID0gMAr/2wBDABALDA4MChAODQ4SERATGCcZGBYWGDAiJBwnOTI8"
    "OzgyNzY/R1pMP0NVRDY3TmtPVV1gZWZlPUtvd25idlpjZWH/2wBDARESEhgVGC4ZGS5hQTdBYWFhY"
    "WFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWH/wAARCAF+AYkDAS"
    "IAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQ"
    "AAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3"
    "ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp"
    "6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAw"
    "EBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJ"
    "BUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RV"
    "VldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6w"
    "sPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDaR28tk5GF3M"
    "T/AL2P5U2AO0sreYHVQShI9Rio7WXMt7I4LEQp07HcR0qvJqIjmFrCoMvTLdB+Fcyi9jf0Lt3HlSg"
    "XcX9OOg4pHWWCBGZHaRiytnpg1Nalz87De3U5/nWjFIs42wnKg8sapQIuznbZpLZkgXO3zS5OcgdO"
    "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";

static const char *easter_egg_photo3 =
    "jpegphoto:: /9j/4AAQSkZJRgABAQAAAQABAAD//gBtQ1JFQVRPUjogWFYgVmVyc2lvbiAzLjEwYS"
    "BSZXY6IDEyLzI5Lzk0IChqcC1leHRlbnNpb24gNS4zLjMgKyBQTkcgcGF0Y2ggMS4yZCkgIFF1YWx"
    "pdHkgPSA4NywgU21vb3RoaW5nID0gMAr/2wBDAAQDAwQDAwQEAwQFBAQFBgoHBgYGBg0JCggKDw0Q"
    "EA8NDw4RExgUERIXEg4PFRwVFxkZGxsbEBQdHx0aHxgaGxr/2wBDAQQFBQYFBgwHBwwaEQ8RGhoaG"
    "hoaGhoaGhoaGhoaGhoaGhoaGhoaGhoaGhoaGhoaGhoaGhoaGhoaGhoaGhoaGhr/wAARCAEZAXcDAS"
    "IAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQ"
    "AAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3"
    "ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp"
    "6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAw"
    "EBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJ"
    "BUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RV"
    "VldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6w"
    "sPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD7+ooooAKKKK"
    "ACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAK"
    "KKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooo"
    "oAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigA"
    "ooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACii"
    "igAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKA"
    "CiiigAoorC8TeM/D3g2ye88VazY6RbKMl7qdU/maAN2ivlPxt+378MPDTSQ+H11HxRcLkA2kPlxZ/"
    "33xke4BrwrxH/wAFJPFVy7r4U8H6Rp0f8LX08ly35LsH86AP0hor8mdR/b0+NF6xNtq+l6aD0FvpU"
    "TY/7+Bqyf8Ahtz46793/CbLj+7/AGNY4/8AROaAP18or8mtO/b0+NFiwN1q2l6kB1FxpUS5/wC/YW"
    "vRfDn/AAUk8U2zovivwdpOox/xPYzyWzfXDbx/KgD9IKK+VfBP7fnwv8TNHDr/APaPhe5fAP2yHfF"
    "n/fQnA9yBX0d4a8ZeH/GNkl54W1iy1a2cZD206v8AyoA3KKKKACiiigAooooAKKKKACiiigAooooA"
    "KKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAK5fx38RfDPw10"
    "WTV/GmrW+l2ig7fMb55D/dRerH6V4v+0d+1v4f+CltNpOiCLXvGUifu7QP+6tc9HmI/9BHJ9utfl9"
    "8QfiX4o+KOuzaz411WbUruQnarHEcS/wB1EHCgegoA+s/jD/wUG1nV3uNN+E1kNHs+V/tG6UPOw9V"
    "Xov618c+JfF2u+MdQe/8AFOrXmrXbkkyXMxfH0B4H4Vi0+KJ55FjhRpJGOFVRkk0DSbdkMoru9K+F"
    "mp3sQkvpo7EMMhSN7fiO351oSfCGcD91qkbH0aEj+tcbxmHi7OR9DT4czarBTjRdvNpP7m7nmlFdr"
    "d/C/XLfPkCC6H+xJg/riqA+H/iEvt/s9h7l1x/OtFiaL1Ul95yTybMqcuWVCX3N/kczRXoFl8JtSm"
    "UNe3cFrn+EAuR/IVePwgbHGrDP/Xv/APZVm8bh07cx2w4azepHmVF/Npfg2eY1t+GfGGveDb9L/wA"
    "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"
    "FFFFABRRRQAUUUUAFFFFABTJYo543jmRZI3GGVhkEehFPooA4+TwXPpEjT+CtQbSiTuazlBktXP+7"
    "1T/AICfwpB4yvdIIj8XaLc2YHBu7NTcwH3O0b1+mD9a7GgjIweRQBnaTr+l67GZNH1C2vlX73kyhi"
    "p9COoPsa0awdV8FeH9alE2oaVbPcL92dU2SL9HXBH4Gs//AIQee1/5A3inX9PX+49yl2v/AJMJIcf"
    "QigDrqK5L+x/GUPFt4q06ZfW80Mu35xzxj9KUad457+I/Dv8A4Ts//wAm0AdZRXJ/2P4xm4ufFWnQ"
    "r62ehlG/OSeQfpTT4Hmuv+Qz4o1/UF7ot0tov/kusZx9SaAN3Vtf0vQow+sahbWSt90TShSx9AOpP"
    "sK58+Mb7Vzs8I6LcXanpeXqm2gHuAfnb6YH1rT0rwXoGiyNLp2lW0c7ffmZN8je5c5JP1Nb3TpQBx"
    "8XgubVpFuPGl+2rMDuWzjHl2qH/c6v/wAC/KuujiSGNY4UWONBhVUYAHoBTqKACiiigAooooAKKKK"
    "ACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAK"
    "KKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooo"
    "oAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigA"
    "ooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACii"
    "igAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKA"
    "CiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAP//Z";

static const char *easter_egg_photos[NUM_EASTER_EGG_PHOTOS + 1];

/*
 * fedse is based the general implementation in dse
 */

static struct dse *pfedse = NULL;

static int check_plugin_path(Slapi_PBlock *pb, Slapi_Entry *entryBefore, Slapi_Entry *e, int *returncode, char *returntext, void *arg);

static void
internal_add_helper(Slapi_Entry *e, int dont_write_file)
{
    int plugin_actions = 0;
    Slapi_PBlock *newpb = slapi_pblock_new();
    Slapi_Operation *op;

    slapi_add_entry_internal_set_pb(newpb, e, NULL,
                                    plugin_get_default_component_id(),
                                    plugin_actions);
    slapi_pblock_set(newpb, SLAPI_TARGET_SDN, (void *)slapi_entry_get_sdn_const(e));
    slapi_pblock_set(newpb, SLAPI_DSE_DONT_WRITE_WHEN_ADDING,
                     (void *)&dont_write_file);
    slapi_pblock_get(newpb, SLAPI_OPERATION, &op);
    operation_set_flag(op, OP_FLAG_ACTION_NOLOG);

    slapi_add_internal_pb(newpb);
    slapi_pblock_destroy(newpb);
}

/*
 * Attempt to initialize the DSE file.  First we attempt to read
 * the file and convert it to the avl tree of DSEs.  If the
 * file doesn't exist, we try to create it and put a minimal
 * root DSE into it.
 *
 * Returns 1 for OK, 0 for Fail.
 */
static int
init_dse_file(const char *configdir, Slapi_DN *config)
{
    int rc = 1; /* OK */

    if (pfedse == NULL) {
        pfedse = dse_new(DSE_FILENAME, DSE_TMPFILE, DSE_BACKFILE, DSE_STARTOKFILE, configdir);
        rc = (pfedse != NULL);
    }
    if (rc) {
        Slapi_PBlock *pb = slapi_pblock_new();
        int dont_write = 1;
        dse_register_callback(pfedse, DSE_OPERATION_READ, DSE_FLAG_PREOP, config,
                              LDAP_SCOPE_SUBTREE, "(objectclass=nsslapdPlugin)",
                              load_plugin_entry, NULL, NULL);
        dse_register_callback(pfedse, DSE_OPERATION_READ, DSE_FLAG_PREOP, config,
                              LDAP_SCOPE_BASE, "(objectclass=*)",
                              load_config_dse, NULL, NULL);

        slapi_pblock_set(pb, SLAPI_CONFIG_DIRECTORY, (void *)configdir);
        /* don't write out the file when reading */
        slapi_pblock_set(pb, SLAPI_DSE_DONT_WRITE_WHEN_ADDING, (void *)&dont_write);
        if (!(rc = dse_read_file(pfedse, pb))) {
            slapi_log_err(SLAPI_LOG_ERR, "init_dse_file",
                          "Could not load config file [%s]\n",
                          DSE_FILENAME);
        }
        slapi_pblock_destroy(pb);
    }
    return rc;
}

void
add_internal_entries(void)
{
    /* add the internal only entries */
    int i;
    for (i = 0; i < NUM_INTERNAL_ENTRIES; i++) {
        Slapi_Entry *e;
        char *p;
        p = slapi_ch_strdup(internal_entries[i]);
        e = slapi_str2entry(p, 0);
        internal_add_helper(e, 0); /* 0 writes file */
        slapi_ch_free((void **)&p);
    }
}


static int
egg_char2nibble(unsigned char c)
{
    return (c < 'A') ? c - '0' : 10 + c - 'A';
}

/* decode in place (output is guaranteed to be smaller than input) */
static void
egg_decode(char *s)
{
    const char *pin;
    char *pout;


    pin = pout = s;
    while (*pin != '\0') {
        *pout = egg_char2nibble(*pin++) << 4;
        *pout |= egg_char2nibble(*pin++);
        *pout ^= 122;
        pout++;
    }
    *pout = '\0';
}

static void
add_easter_egg_entry(void)
{
    Slapi_Entry *e = NULL;
    char *src;

    easter_egg_photos[0] = easter_egg_photo1;
    easter_egg_photos[1] = easter_egg_photo2;
    easter_egg_photos[2] = easter_egg_photo3;
    easter_egg_photos[NUM_EASTER_EGG_PHOTOS] = NULL;

    src = slapi_ch_strdup(easter_egg_entry);
    egg_decode(src); /* twiddle bits */
    e = slapi_str2entry(src, 0);
    if (NULL != e) {
        internal_add_helper(e, 1); /* 1 tells it to not write these entries to the dse file */
    }
    slapi_ch_free((void **)&src);
}

static int
dont_allow_that(Slapi_PBlock *pb __attribute__((unused)),
                Slapi_Entry *entryBefore __attribute__((unused)),
                Slapi_Entry *e __attribute__((unused)),
                int *returncode,
                char *returntext __attribute__((unused)),
                void *arg __attribute__((unused)))
{
    *returncode = LDAP_UNWILLING_TO_PERFORM;
    return SLAPI_DSE_CALLBACK_ERROR;
}

static void
setEntrySSLVersion(Slapi_Entry *entry, char *sslversion, char *newval)
{
    char *v = slapi_entry_attr_get_charptr(entry, sslversion);

    if (v) {
        if (PL_strcasecmp(v, newval)) { /* did not match */
            struct berval bv;
            struct berval *bvals[2];
            bvals[0] = &bv;
            bvals[1] = NULL;
            bv.bv_val = newval;
            bv.bv_len = strlen(bv.bv_val);
            slapi_entry_attr_replace(entry, sslversion, bvals);
        }
        slapi_ch_free_string(&v);
    } else {
        slapi_entry_attr_set_charptr(entry, sslversion, newval);
    }
}

/*This function takes care of the search on the attribute nssslsupportedciphers in cn=encryption,cn=config" entry. This would get the list of supported ciphers from the table in ssl.c and always return that value */
int
search_encryption(Slapi_PBlock *pb __attribute__((unused)),
                  Slapi_Entry *entry,
                  Slapi_Entry *entryAfter __attribute__((unused)),
                  int *returncode __attribute__((unused)),
                  char *returntext __attribute__((unused)),
                  void *arg __attribute__((unused)))
{
    struct berval *vals[2];
    struct berval val;
    char **cipherList = getSupportedCiphers();      /*Get the string array of supported ciphers here */
    char **enabledCipherList = getEnabledCiphers(); /*Get the string array of enabled ciphers here */
    int ssl2, ssl3, tls1;
    char *sslVersionMin = NULL;
    char *sslVersionMax = NULL;
    vals[0] = &val;
    vals[1] = NULL;

    attrlist_delete(&entry->e_attrs, "nsSSLSupportedCiphers");
    while (cipherList && *cipherList) /* iterarate thru each of them and add to the attr value */
    {
        char *cipher = *cipherList;
        val.bv_val = (char *)cipher;
        val.bv_len = strlen(val.bv_val);
        attrlist_merge(&entry->e_attrs, "nsSSLSupportedCiphers", vals);
        cipherList++;
    }

    attrlist_delete(&entry->e_attrs, "nsSSLEnabledCiphers");
    while (enabledCipherList && *enabledCipherList) /* iterarate thru each of them and add to the attr value */
    {
        char *cipher = *enabledCipherList;
        val.bv_val = (char *)cipher;
        val.bv_len = strlen(val.bv_val);
        attrlist_merge(&entry->e_attrs, "nsSSLEnabledCiphers", vals);
        enabledCipherList++;
    }

    if (!getSSLVersionInfo(&ssl2, &ssl3, &tls1)) { /* 0 if the version info is initialized */
        setEntrySSLVersion(entry, "nsSSL2", ssl2 ? "on" : "off");
        setEntrySSLVersion(entry, "nsSSL3", ssl3 ? "on" : "off");
        setEntrySSLVersion(entry, "nsTLS1", tls1 ? "on" : "off");
    }

    if (!getSSLVersionRange(&sslVersionMin, &sslVersionMax)) { /* 0 if the range is initialized or supported */
        setEntrySSLVersion(entry, "sslVersionMin", sslVersionMin);
        setEntrySSLVersion(entry, "sslVersionMax", sslVersionMax);
    }
    slapi_ch_free_string(&sslVersionMin);
    slapi_ch_free_string(&sslVersionMax);

    return SLAPI_DSE_CALLBACK_OK;
}

/*
 * This function protects the easter egg entry from being seen,
 * unless you specifically ask for them.
 */
int
search_easter_egg(Slapi_PBlock *pb,
                  Slapi_Entry *entryBefore,
                  Slapi_Entry *entryAfter __attribute__((unused)),
                  int *returncode __attribute__((unused)),
                  char *returntext __attribute__((unused)),
                  void *arg __attribute__((unused)))
{
    char *fstr = NULL;
    char eggfilter[64];
    PR_snprintf(eggfilter, sizeof(eggfilter), "(objectclass=%s)", EGG_OBJECT_CLASS);
    slapi_pblock_get(pb, SLAPI_SEARCH_STRFILTER, &fstr);
    if (fstr != NULL && strcasecmp(fstr, eggfilter) == 0) {
        static int twiddle = -1;
        char *copy;
        struct berval bvtype = {0, NULL};
        struct berval bv = {0, NULL};
        int freeval = 0;
        struct berval *bvals[2];
        if (twiddle < 0) {
            twiddle = slapi_rand();
        }
        bvals[0] = &bv;
        bvals[1] = NULL;
        copy = slapi_ch_strdup(easter_egg_photos[twiddle % NUM_EASTER_EGG_PHOTOS]);
        if (slapi_ldif_parse_line(copy, &bvtype, &bv, &freeval) < 0) {
            return SLAPI_DSE_CALLBACK_ERROR;
        }
        slapi_entry_attr_delete(entryBefore, "jpegphoto");
        slapi_entry_attr_merge(entryBefore, "jpegphoto", bvals);
        slapi_ch_free((void **)&copy);
        twiddle++;
        /* the memory below was not allocated by the slapi_ch_ functions */
        if (freeval)
            slapi_ch_free_string(&bv.bv_val);
        return SLAPI_DSE_CALLBACK_OK;
    }
    return SLAPI_DSE_CALLBACK_ERROR;
}

int
search_counters(Slapi_PBlock *pb __attribute__((unused)),
                Slapi_Entry *entryBefore,
                Slapi_Entry *e __attribute__((unused)),
                int *returncode __attribute__((unused)),
                char *returntext __attribute__((unused)),
                void *arg __attribute__((unused)))
{
    counters_as_entry(entryBefore);
    return SLAPI_DSE_CALLBACK_OK;
}

int
search_snmp(Slapi_PBlock *pb __attribute__((unused)),
            Slapi_Entry *entryBefore,
            Slapi_Entry *e __attribute__((unused)),
            int *returncode __attribute__((unused)),
            char *returntext __attribute__((unused)),
            void *arg __attribute__((unused)))
{
    snmp_as_entry(entryBefore);
    return SLAPI_DSE_CALLBACK_OK;
}

/*
 * Called from config.c to install the internal backends
 */
int
setup_internal_backends(char *configdir)
{
    int rc = init_schema_dse(configdir);
    Slapi_DN config;

    slapi_sdn_init_ndn_byref(&config, "cn=config");

    if (rc) {
        rc = init_dse_file(configdir, &config);
    }

    if (rc) {
        Slapi_DN monitor;
        Slapi_DN counters;
        Slapi_DN snmp;
        Slapi_DN root;
        Slapi_Backend *be;
        Slapi_DN encryption;
        Slapi_DN saslmapping;
        Slapi_DN plugins;

        slapi_sdn_init_ndn_byref(&monitor, "cn=monitor");
        slapi_sdn_init_ndn_byref(&counters, "cn=counters,cn=monitor");
        slapi_sdn_init_ndn_byref(&snmp, "cn=snmp,cn=monitor");
        slapi_sdn_init_ndn_byref(&root, "");

        slapi_sdn_init_ndn_byref(&encryption, "cn=encryption,cn=config");
        slapi_sdn_init_ndn_byref(&saslmapping, "cn=mapping,cn=sasl,cn=config");
        slapi_sdn_init_ndn_byref(&plugins, "cn=plugins,cn=config");

        /* Search */
        dse_register_callback(pfedse, SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, &config, LDAP_SCOPE_BASE, "(objectclass=*)", read_config_dse, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, &monitor, LDAP_SCOPE_BASE, "(objectclass=*)", monitor_info, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, &root, LDAP_SCOPE_BASE, "(objectclass=*)", read_root_dse, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, &monitor, LDAP_SCOPE_SUBTREE, EGG_FILTER, search_easter_egg, NULL, NULL); /* Egg */
        dse_register_callback(pfedse, SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, &counters, LDAP_SCOPE_BASE, "(objectclass=*)", search_counters, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, &snmp, LDAP_SCOPE_BASE, "(objectclass=*)", search_snmp, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, &encryption, LDAP_SCOPE_BASE, "(objectclass=*)", search_encryption, NULL, NULL);

        /* Modify */
        dse_register_callback(pfedse, SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, &config, LDAP_SCOPE_BASE, "(objectclass=*)", modify_config_dse, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_MODIFY, DSE_FLAG_POSTOP, &config, LDAP_SCOPE_BASE, "(objectclass=*)", postop_modify_config_dse, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, &root, LDAP_SCOPE_BASE, "(objectclass=*)", modify_root_dse, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, &saslmapping, LDAP_SCOPE_SUBTREE, "(objectclass=nsSaslMapping)", sasl_map_config_modify, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_MODIFY, DSE_FLAG_PREOP, &plugins, LDAP_SCOPE_SUBTREE, "(objectclass=nsSlapdPlugin)", check_plugin_path, NULL, NULL);

        /* Delete */
        dse_register_callback(pfedse, SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, &config, LDAP_SCOPE_BASE, "(objectclass=*)", dont_allow_that, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, &monitor, LDAP_SCOPE_BASE, "(objectclass=*)", dont_allow_that, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, &counters, LDAP_SCOPE_BASE, "(objectclass=*)", dont_allow_that, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, &snmp, LDAP_SCOPE_BASE, "(objectclass=*)", dont_allow_that, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, &root, LDAP_SCOPE_BASE, "(objectclass=*)", dont_allow_that, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, &encryption, LDAP_SCOPE_BASE, "(objectclass=*)", dont_allow_that, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_DELETE, DSE_FLAG_PREOP, &saslmapping, LDAP_SCOPE_SUBTREE, "(objectclass=nsSaslMapping)", sasl_map_config_delete, NULL, NULL);

        /* Write */
        dse_register_callback(pfedse, DSE_OPERATION_WRITE, DSE_FLAG_PREOP, &monitor, LDAP_SCOPE_SUBTREE, EGG_FILTER, dont_allow_that, NULL, NULL); /* Egg */

        /* Add */
        dse_register_callback(pfedse, SLAPI_OPERATION_ADD, DSE_FLAG_PREOP, &saslmapping, LDAP_SCOPE_SUBTREE, "(objectclass=nsSaslMapping)", sasl_map_config_add, NULL, NULL);
        dse_register_callback(pfedse, SLAPI_OPERATION_ADD, DSE_FLAG_PREOP, &plugins, LDAP_SCOPE_SUBTREE, "(objectclass=nsSlapdPlugin)", check_plugin_path, NULL, NULL);

        be = be_new_internal(pfedse, "DSE", DSE_BACKEND, &fedse_plugin);
        be_addsuffix(be, &root);
        be_addsuffix(be, &monitor);
        be_addsuffix(be, &config);

        /*
         * Now that the be's are in place, we can
         * setup the mapping tree.
         */

        if (mapping_tree_init()) {
            slapi_log_err(SLAPI_LOG_EMERG, "setup_internal_backends", "Failed to init mapping tree\n");
            exit(1);
        }

        add_internal_entries();

        add_easter_egg_entry();

        slapi_sdn_done(&monitor);
        slapi_sdn_done(&counters);
        slapi_sdn_done(&snmp);
        slapi_sdn_done(&root);
        slapi_sdn_done(&saslmapping);
        slapi_sdn_done(&plugins);
    } else {
        slapi_log_err(SLAPI_LOG_ERR, "setup_internal_backends",
                      "Please edit the file to correct the reported problems"
                      " and then restart the server.\n");
        exit(1);
    }

    slapi_sdn_done(&config);
    return rc;
}

int
fedse_create_startOK(char *filename, char *startokfilename, const char *configdir)
{
    char *realconfigdir = NULL;
    char *dse_filename = NULL;
    char *dse_filestartOK = NULL;
    int rc = -1;

    if (configdir != NULL) {
        realconfigdir = slapi_ch_strdup(configdir);
    } else {
        realconfigdir = config_get_configdir();
    }
    if (realconfigdir != NULL) {
        /* Set the full path name for the config DSE entry */
        if (!strstr(filename, realconfigdir)) {
            dse_filename = slapi_ch_smprintf("%s/%s", realconfigdir, filename);
        } else {
            dse_filename = slapi_ch_strdup(filename);
        }

        if (!strstr(startokfilename, realconfigdir)) {
            dse_filestartOK = slapi_ch_smprintf("%s/%s",
                                                realconfigdir, startokfilename);
        } else {
            dse_filestartOK = slapi_ch_strdup(startokfilename);
        }

        rc = slapi_copy(dse_filename, dse_filestartOK);
        if (rc != 0) {
            slapi_log_err(SLAPI_LOG_ERR, "fedse_create_startOK", "Cannot copy"
                                                                 " DSE file \"%s\" to \"%s\" OS error %d (%s)\n",
                          dse_filename, dse_filestartOK,
                          rc, slapd_system_strerror(rc));
        }

        slapi_ch_free_string(&dse_filename);
        slapi_ch_free_string(&dse_filestartOK);
        slapi_ch_free_string(&realconfigdir);
    }

    return rc;
}

static int
check_plugin_path(Slapi_PBlock *pb __attribute__((unused)),
                  Slapi_Entry *entryBefore,
                  Slapi_Entry *e,
                  int *returncode,
                  char *returntext,
                  void *arg __attribute__((unused)))
{
    /* check for invalid nsslapd-pluginPath */
    char **vals = slapi_entry_attr_get_charray(e, ATTR_PLUGIN_PATH);
    int j = 0;
    int rc = SLAPI_DSE_CALLBACK_OK;

    if (NULL == vals) {
        /* ADD case, entryBefore is used for the new entry */
        vals = slapi_entry_attr_get_charray(entryBefore, ATTR_PLUGIN_PATH);
    }
    for (j = 0; vals && vals[j]; j++) {
        void *handle;
        char *full_path = NULL;
        char *res = NULL;

        if (*vals[j] == '/') { /* absolute path */
            full_path = slapi_get_plugin_name(NULL, vals[j]);
        } else { /* relative path */
            full_path = slapi_get_plugin_name(PLUGINDIR, vals[j]);
        }
        /*
         * See man 3 realpath. We have to pass in NULL here, because we don't
         * know if the library is versioned, it could be *any* length when
         * resolved. The quirk is that this uses malloc, not slapi_ch_malloc,
         * so we need to free res with free() only!
         */
        res = realpath(full_path, NULL);
        if (res) {
            /* If this ever fails, the way to check the error is:
             * gdb -p ns-slapd;
             * br _dlerror_run
             * step until you hit something with (result).
             * print *result
             * You may need to repeat a few times, but eventually you'll get:
             * $5 = {errcode = 0, returned = 0, malloced = true, objname = 0x608000053f40 "/opt/dirsrv/lib/dirsrv/plugins/libhellorust-0.1.0.so", errstring = 0x608000053f20 "undefined symbol: slapi_log_err"}
             * In this case, it means we try to use a symbol that doesn't exist now.
             */
            if ((handle = dlopen(res, RTLD_NOW)) == NULL) {
                *returncode = LDAP_UNWILLING_TO_PERFORM;
                PR_snprintf(returntext, SLAPI_DSE_RETURNTEXT_SIZE, "Invalid plugin path %s - failed to open library", res);
                rc = SLAPI_DSE_CALLBACK_ERROR;
            } else {
                dlclose(handle);
            }
        } else {
            *returncode = LDAP_UNWILLING_TO_PERFORM;
            PR_snprintf(returntext, SLAPI_DSE_RETURNTEXT_SIZE, "Invalid plugin path");
            rc = SLAPI_DSE_CALLBACK_ERROR;
        }
        slapi_ch_free_string(&full_path);
        /* See comment above. Must free res from realpath with free() only! */
        free(res);
    }
    slapi_ch_array_free(vals);

    return rc;
}
