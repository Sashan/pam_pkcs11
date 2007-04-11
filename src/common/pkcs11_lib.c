/*
 * PKCS #11 PAM Login Module
 * Copyright (C) 2003 Mario Strasser <mast@gmx.net>,
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * $Id$
 */

#define __PKCS11_LIB_C__

/*
 * common includes
 */
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "debug.h"
#include "error.h"
#include "cert_info.h"
#include "pkcs11_lib.h"


/*
 * this functions is completely common between both implementation.
 */
int pkcs11_pass_login(pkcs11_handle_t *h, int nullok)
{
  int rv;
  char *pin;

  /* get password */
  pin =getpass("PIN for token: ");
#ifndef DEBUG_HIDE_PASSWORD
  DBG1("PIN = [%s]", pin);
#endif
  /* for safety reasons, clean PIN string from memory asap */

  /* check password length */
  if (!nullok && strlen(pin) == 0) {
    memset(pin, 0, strlen(pin));
    free(pin);
    set_error("Empty passwords not allowed");
    return -1;
  }

  /* perform pkcs #11 login */
  rv = pkcs11_login(h, pin);
  memset(pin, 0, strlen(pin));
  free(pin);
  if (rv != 0) {
    /* DBG1("pkcs11_login() failed: %s", get_error()); */
    return -1;
  }
  return 0;
}

#ifdef HAVE_NSS
/*
 * Using NSS to find the manage the PKCS #11 modules
 */
#include "nss.h"
#include "cert.h"
#include "secmod.h"
#include "pk11pub.h"
#include "cert_st.h"
#include "secutil.h"
#include "cryptohi.h"
#include "ocsp.h"
#include <unistd.h>
#include <errno.h>

#include "cert_vfy.h"

struct pkcs11_handle_str {
  SECMODModule *module;
  PRBool	is_user_module;
  PK11SlotInfo *slot;
  cert_object_t **certs;
  int cert_count;
};

static int app_has_NSS = 0;


char *
password_passthrough(PK11SlotInfo *slot, PRBool retry, void *arg)
{
  /* give up if 1) no password was supplied, or 2) the password has already
   * been rejected once by this token. */
  if (retry || (arg == NULL)) {
    return NULL;
  }
  return PL_strdup((char *)arg);
}


int crypto_init(cert_policy *policy) {
  SECStatus rv;

  DBG("Initializing NSS ...");
  if (NSS_IsInitialized()) {
    app_has_NSS = 1;
    /* we should save the app's password function */
    PK11_SetPasswordFunc(password_passthrough);
    DBG("...  NSS is initialized");
    return 0;
  }
  if (policy->nss_dir) {
    /* initialize with read only databases */
    DBG1("Initializing NSS ... database=%s", policy->nss_dir);
    rv = NSS_Init(policy->nss_dir);
  } else {
    /* not database secified */
    DBG("Initializing NSS ... with no db");
    rv = NSS_NoDB_Init(NULL);
  }

  if (rv != SECSuccess) {
    DBG1("NSS_Initialize faile: %s", SECU_Strerror(PR_GetError()));
    return -1;
  }
  /* register a callback */
  PK11_SetPasswordFunc(password_passthrough);

  if (policy->ocsp_policy == OCSP_ON) {
    CERT_EnableOCSPChecking(CERT_GetDefaultCertDB());
  }
  DBG("...  NSS Complete");
  return 0;
}


static SECMODModule *find_module_by_library(char *pkcs11_module) 
{
  SECMODModule *module = NULL;
  SECMODModuleList *modList = SECMOD_GetDefaultModuleList();

  /* threaded applications should also acquire the
   * DefaultModuleListLock */
  DBG("Looking up module in list");
  for ( ; modList; modList = modList->next) {
    char *dllName = modList->module->dllName;
    DBG2("modList = 0x%x next = 0x%x\n", modList, modList->next);
    DBG1("dllName= %s \n", dllName ? dllName : "<null>");
    if (dllName && strcmp(dllName,pkcs11_module) == 0) {
        module = SECMOD_ReferenceModule(modList->module);
        break;
    }
  }
  return module;
}

/*
 * NSS allows you to load a specific module. If the user specified a module
 * to load, load it, otherwize select on of the standard modules from the
 * secmod.db list. 
 */
int load_pkcs11_module(char *pkcs11_module, pkcs11_handle_t **hp)
{
  pkcs11_handle_t *h = (pkcs11_handle_t *)calloc(sizeof(pkcs11_handle_t),1);
  SECMODModule *module = NULL;
#define SPEC_TEMPLATE "library=\"%s\" name=\"SmartCard\""
  char *moduleSpec = NULL;

  if (!pkcs11_module || (strcasecmp(pkcs11_module,"any module") == 0)) {
    h->is_user_module = PR_FALSE;
    h->module = NULL;
    *hp = h;
    return 0;
  }

  /* found it, use the existing module */
  module = find_module_by_library(pkcs11_module);
  if (module) {
    h->is_user_module = PR_FALSE;
    h->module = module;
    *hp = h;
    return 0;
  }

  /* specified module is not already loaded, load it now */	
  moduleSpec = (char *)malloc(sizeof(SPEC_TEMPLATE) + strlen(pkcs11_module));
  if (!moduleSpec) {
    DBG1("Malloc failed when allocating module spec", strerror(errno));
    free (h);
    return -1;
  }
  sprintf(moduleSpec,SPEC_TEMPLATE, pkcs11_module);
  DBG2("loading Module explictly, moduleSpec=<%s> module=%s",
                                                moduleSpec, pkcs11_module);
  module = SECMOD_LoadUserModule(moduleSpec, NULL, 0);
  free(moduleSpec);
  if ((!module) || !module->loaded) {
    DBG1("Failed to load SmartCard software %s", SECU_Strerror(PR_GetError()));
    free (h);
    if (module) {
      SECMOD_DestroyModule(module);
    }
    return -1;
  }
  h->is_user_module = PR_TRUE;
  h->module = module;
  *hp = h;
  DBG("load module complete");
  return 0;
}

int init_pkcs11_module(pkcs11_handle_t *h, int flag)
{
  return 0; /* NSS initialized the module on load */
}

int find_slot_by_number(pkcs11_handle_t *h, int slot_num, unsigned int *slotID)
{
  SECMODModule *module = h->module;
  int i;

  /* if module is null, 
   * any of the PKCS #11 modules specified in the system config
   * is available, find one */
  if (module == NULL) {
    PK11SlotList *list;
    PK11SlotListElement *le;
    PK11SlotInfo *slot = NULL;

    /* find a slot, we haven't specifically selected a module,
     * so find an appropriate one. */
    /* get them all */
    list = PK11_GetAllTokens(CKM_INVALID_MECHANISM, PR_FALSE, PR_TRUE, NULL);
    if (list == NULL) {
	return -1;
    }
    for (le = list->head; le; le = le->next) {
      CK_SLOT_INFO slInfo;
      SECStatus rv;

      slInfo.flags = 0;
      rv = PK11_GetSlotInfo(le->slot, &slInfo);
      if (rv == SECSuccess && (slInfo.flags & CKF_REMOVABLE_DEVICE)) {
	slot = PK11_ReferenceSlot(le->slot);
	module = SECMOD_ReferenceModule(PK11_GetModule(le->slot));
	break;
      }
    }
    PK11_FreeSlotList(list);
    if (slot == NULL) {
	return -1;
    }
    h->slot = slot;
    h->module = module;
    *slotID = PK11_GetSlotID(slot);
    return 0;
  }

  /*
   * we're configured with a specific module, look for a present slot
   * on that module. */
  if (slot_num == 0) {
    /* threaded applications should also acquire the
     * DefaultModuleListLock */
    for (i=0; i < module->slotCount; i++) {
      if (module->slots[i] && PK11_IsPresent(module->slots[i])) {
        h->slot = PK11_ReferenceSlot(module->slots[i]);
        *slotID = PK11_GetSlotID(h->slot);
        return 0;
      }
    }
  }
  /* we're configured for a specific module and token, see if it's present */
  slot_num--;
  if (slot_num >= 0 && slot_num < module->slotCount && module->slots &&
      module->slots[i] && PK11_IsPresent(module->slots[i])) {
    h->slot = PK11_ReferenceSlot(module->slots[i]);
    *slotID = PK11_GetSlotID(h->slot);
    return 0;
  }
  return -1;
}

void release_pkcs11_module(pkcs11_handle_t *h) 
{
  SECStatus rv;
  close_pkcs11_session(h);
  if (h->is_user_module) {
    rv = SECMOD_UnloadUserModule(h->module);
    if (rv != SECSuccess) {
      DBG1("Unloading UserModule failed: %s", SECU_Strerror(PR_GetError()));
    }
  }

  if (h->module) {
    SECMOD_DestroyModule(h->module);
  }
  memset(h, 0, sizeof(pkcs11_handle_t));
  free(h);

  /* if we initialized NSS, then we need to shut it down */
  if (!app_has_NSS) {
    rv = NSS_Shutdown();
    if (rv != SECSuccess) {
      DBG1("NSS Shutdown Failed: %s", SECU_Strerror(PR_GetError()));
    }
  }
}

int open_pkcs11_session(pkcs11_handle_t *h, unsigned int slot_num)
{
  /* NSS manages the sessions under the covers, use this function to
   * select a slot */
  if (h->slot != NULL) {
    /* we've already selected the slot */
    if (PK11_GetSlotID(h->slot) == slot_num) {
	return 0;
    }
    /* the slot we've selected isn't the one we want to open */
    PK11_FreeSlot(h->slot);
    h->slot = NULL;
  }

  /* look the slot up */
  h->slot = SECMOD_LookupSlot(h->module->moduleID, slot_num);
  if (h->slot == NULL) {
    return -1;
  }

  /* make sure it is present */
  if (!PK11_IsPresent(h->slot)) {
    PK11_FreeSlot(h->slot);
    h->slot = NULL;
    return -1;
  }
  return 0;
}

int pkcs11_login(pkcs11_handle_t *h, char *password)
{
  SECStatus rv;

  if (h->slot == NULL) {
    DBG("Login failed: No Slot selected");
    return -1;
  }
  rv = PK11_Authenticate(h->slot, PR_FALSE, password);
  if (rv != SECSuccess) {
    DBG1("Login failed: %s", SECU_Strerror(PR_GetError()));
  }
  return (rv == SECSuccess) ? 0 : -1;
}

int close_pkcs11_session(pkcs11_handle_t *h)
{
  if (h->slot) {
    PK11_Logout(h->slot);
    PK11_FreeSlot(h->slot);
    h->slot = NULL;
  }
  if (h->certs) {
    CERT_DestroyCertArray((CERTCertificate **)h->certs, h->cert_count);
    h->certs = NULL;
    h->cert_count = 0;
  }
  return 0;
}

const char *get_slot_label(pkcs11_handle_t *h)
{
  if (!h->slot) {
    return NULL;
  }
  return PK11_GetTokenName(h->slot);
}

cert_object_t **get_certificate_list(pkcs11_handle_t *h, int *count)
{
  CERTCertList * certList;
  CERTCertListNode *node;
  cert_object_t **certs;
  int certCount = 0;
  int certIndex = 0;
  SECStatus rv;

  if (!h->slot) {
    return NULL;
  }
  if (h->certs) {
    *count = h->cert_count;
    return h->certs;
  }

  certList = PK11_ListCertsInSlot(h->slot);
  if (!certList) {
    DBG1("Couldn't get Certs from token: %s", SECU_Strerror(PR_GetError()));
    return NULL;
  }

  /* only want signing certs */
  rv = CERT_FilterCertListByUsage(certList,  certUsageSSLClient, PR_FALSE);
  if (rv != SECSuccess) {
      CERT_DestroyCertList(certList);
      DBG1("Couldn't filter out email certs: %s", 
				SECU_Strerror(PR_GetError()));
      return NULL;
  }

  /* only user certs have keys */
  rv = CERT_FilterCertListForUserCerts(certList);
  if (rv != SECSuccess) {
    CERT_DestroyCertList(certList);
    DBG1("Couldn't filter out user certs: %s", SECU_Strerror(PR_GetError()));
    return NULL;
  }

  /* convert the link list from NSS to the array used by pam_pkcs11 */
  for (node = CERT_LIST_HEAD(certList); !CERT_LIST_END(node,certList); 
						node = CERT_LIST_NEXT(node)) {
	if (node->cert) {
	    DBG3("cert %d: found (%s), \"%s\"", certCount,
		node->cert->nickname, node->cert->subjectName);
	    certCount++;
    }
  }

  if (certCount == 0) {
    CERT_DestroyCertList(certList);
    DBG("no certs found found");
    return NULL;
  }

  certs = (cert_object_t **)malloc(sizeof(cert_object_t *)*certCount);
  if (certs == NULL) {
    return NULL;
  }

  for (node = CERT_LIST_HEAD(certList); !CERT_LIST_END(node,certList); 
                                         node = CERT_LIST_NEXT(node)) {
    if (node->cert) {
      certs[certIndex++] = (cert_object_t *)CERT_DupCertificate(node->cert);
      if (certIndex == certCount) {
        break;
      }
    }
  }
  CERT_DestroyCertList(certList);
  h->certs = certs;
  h->cert_count = certIndex;

  *count = certIndex;
  return certs;
}

int get_private_key(pkcs11_handle_t *h, cert_object_t *cert) {
  /* all certs returned from NSS are user certs, and the private key
   * has already been identified */
  return 0;
}

const X509 *get_X509_certificate(cert_object_t *cert)
{
  return (CERTCertificate *)cert;
}

int sign_value(pkcs11_handle_t *h, cert_object_t *cert, CK_BYTE *data,
	      CK_ULONG length, CK_BYTE **signature, CK_ULONG *signature_length)
{
  SECOidTag algtag;
  SECKEYPrivateKey *key;
  SECItem result;
  SECStatus rv;

  if (h->slot == NULL) {
    return -1;
  }

  /* get the key */
  key = PK11_FindPrivateKeyFromCert(h->slot, (CERTCertificate *)cert, NULL);
  if (key == NULL) {
    DBG1("Couldn't Find key for Cert: %s", SECU_Strerror(PR_GetError()));
    return -1;
  }

  /* get the oid */
  algtag = SEC_GetSignatureAlgorithmOidTag(key->keyType, SEC_OID_SHA1);

  /* sign the data */
  rv = SEC_SignData(&result, data, length, key, algtag);
  SECKEY_DestroyPrivateKey(key);
  if (rv != SECSuccess) {
    DBG1("Signature failed: %s", SECU_Strerror(PR_GetError()));
    return -1;
  }

  *signature = (CK_BYTE *)result.data;
  *signature_length = result.len;
  return 0;
}

int get_random_value(unsigned char *data, int length) 
{
  SECStatus rv = PK11_GenerateRandom(data,length);
  if (rv != SECSuccess) {
    DBG1("couldn't generate random number: %s", SECU_Strerror(PR_GetError()));
  }
  return (rv == SECSuccess) ? 0 : -1;
}

#include "nspr.h"

struct tuple_str {
    PRErrorCode	 errNum;
    const char * errString;
};

typedef struct tuple_str tuple_str;

#define ER2(a,b)   {a, b},
#define ER3(a,b,c) {a, c},

#include "secerr.h"
#include "sslerr.h"

const tuple_str errStrings[] = {
/* keep this list in asceding order of error numbers */
#include "SSLerrs.h"
#include "SECerrs.h"
#include "NSPRerrs.h"
};

const PRInt32 numStrings = sizeof(errStrings) / sizeof(tuple_str);

/* Returns a UTF-8 encoded constant error string for "errNum".
 * Returns NULL of errNum is unknown.
 */
const char *
SECU_Strerror(PRErrorCode errNum) 
{
  PRInt32 low  = 0;
  PRInt32 high = numStrings - 1;
  PRInt32 i;
  PRErrorCode num;
  static int initDone;

  /* make sure table is in ascending order.
   * binary search depends on it.
   */
  if (!initDone) {
    PRErrorCode lastNum = ((PRInt32)0x80000000);
    for (i = low; i <= high; ++i) {
      num = errStrings[i].errNum;
      if (num <= lastNum) {
        fprintf(stderr, 
                "sequence error in error strings at item %d\n"
                "error %d (%s)\n"
                "should come after \n"
                "error %d (%s)\n",
                i, lastNum, errStrings[i-1].errString, 
                num, errStrings[i].errString);
      }
      lastNum = num;
    }
    initDone = 1;
  }

  /* Do binary search of table. */
  while (low + 1 < high) {
    i = (low + high) / 2;
    num = errStrings[i].errNum;
    if (errNum == num) 
      return errStrings[i].errString;
    if (errNum < num)
      high = i;
    else 
      low = i;
  }
  if (errNum == errStrings[low].errNum)
    return errStrings[low].errString;
  if (errNum == errStrings[high].errNum)
    return errStrings[high].errString;
  return NULL;
}

#else
#include "cert_st.h"
#include <openssl/x509.h>
#include <openssl/err.h>

#include "rsaref/pkcs11.h"


struct cert_object_str {
  CK_KEY_TYPE key_type;
  CK_CERTIFICATE_TYPE type;
  CK_BYTE *id;
  CK_ULONG id_length;
  CK_OBJECT_HANDLE private_key;
  X509 *x509;
};

typedef struct {
  CK_SLOT_ID id;
  CK_BBOOL token_present;
  CK_UTF8CHAR label[33];
} slot_t;

struct pkcs11_handle_str {
  void *module_handle;
  CK_FUNCTION_LIST_PTR fl;
  slot_t *slots;
  CK_ULONG slot_count;
  CK_SESSION_HANDLE session;
  cert_object_t **certs;
  int cert_count;
  int current_slot;
};


int crypto_init(cert_policy *policy)
{
  /* arg is ignored for OPENSSL */
  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();
}

int load_pkcs11_module(char *module, pkcs11_handle_t **hp)
{
  int rv;
  struct stat module_stat;
  CK_C_GetFunctionList C_GetFunctionList;
  pkcs11_handle_t *h;

  DBG1("PKCS #11 module = [%s]", module);
  /* reset pkcs #11 handle */
  
  h = (pkcs11_handle_t *)calloc(sizeof(pkcs11_handle_t), 1);
  if (h == NULL) {
    set_error("pkcs11_handle_t malloc failed: %s", strerror(errno));
    return -1;
  }

  /* check module permissions */
  rv = stat(module, &module_stat);
  if (rv < 0) {
    set_error("stat() failed: %s", strerror(errno));
    free(h);
    return -1;
  }
  DBG3("module permissions: uid = %d, gid = %d, mode = %o",
      module_stat.st_uid, module_stat.st_gid, module_stat.st_mode & 0777);
  if (module_stat.st_mode & S_IWGRP || module_stat.st_mode & S_IWOTH
      || module_stat.st_uid != 0 || module_stat.st_gid != 0) {
    set_error("the pkcs #11 module MUST be owned by root and MUST NOT "
              "be writeable by the group or others");
    free(h);
    return -1;
  }
  /* load module */
  DBG1("loading module %s", module);
  h->module_handle = dlopen(module, RTLD_NOW);
  if (h->module_handle == NULL) {
    set_error("dlopen() failed: %s", dlerror());
    free(h);
    return -1;
  }
  /* try to get the function list */
  DBG("getting function list");
  C_GetFunctionList = (CK_C_GetFunctionList)dlsym(h->module_handle, "C_GetFunctionList");
  if (C_GetFunctionList == NULL) {
    set_error("dlsym() failed: %s", dlerror());
    free(h);
    return -1;
  }
  rv = C_GetFunctionList(&h->fl);
  if (rv != CKR_OK) {
    set_error("C_GetFunctionList() failed: %x", rv);
    free(h);
    return -1;
  }
  *hp = h;
  return 0;
}

int init_pkcs11_module(pkcs11_handle_t *h,int flag)
{
  int rv;
  CK_ULONG i, j;
  CK_SLOT_ID_PTR slots;
  CK_INFO info;
  CK_C_INITIALIZE_ARGS initArgs;
  /* 
   Set up arguments to allow native threads 
   According with pkcs#11v2.20, must set all pointers to null
   and flags CKF_OS_LOCKING_OK
  */
  initArgs.CreateMutex = NULL;
  initArgs.DestroyMutex = NULL;
  initArgs.LockMutex = NULL;
  initArgs.UnlockMutex = NULL;
  initArgs.flags = CKF_OS_LOCKING_OK;
  initArgs.CreateMutex = NULL;

  /* initialise the module */
  if (flag) rv = h->fl->C_Initialize((CK_VOID_PTR) &initArgs);
  else      rv = h->fl->C_Initialize(NULL);
  if (rv != CKR_OK) {
    set_error("C_Initialize() failed: %x", rv);
    return -1;
  }

  rv = h->fl->C_GetInfo(&info);
  if (rv != CKR_OK) {
    set_error("C_GetInfo() failed: %x", rv);
    return -1;
  }
  /* show some information about the module */
  DBG("module information:");
  DBG2("- version: %hhd.%hhd", info.cryptokiVersion.major, info.cryptokiVersion.minor);
  DBG1("- manufacturer: %.32s", info.manufacturerID);
  DBG1("- flags: %04lx", info.flags);
  DBG1("- library description: %.32s", info.libraryDescription);
  DBG2("- library version: %hhd.%hhd", info.libraryVersion.major, info.libraryVersion.minor);
  /* get a list of all slots */
  rv = h->fl->C_GetSlotList(FALSE, NULL, &h->slot_count);
  if (rv != CKR_OK) {
    set_error("C_GetSlotList() failed: %x", rv);
    return -1;
  }
  DBG1("number of slots (a): %d", h->slot_count);
  if (h->slot_count == 0) {
    set_error("there are no slots available");
    return -1;
  }
  slots = malloc(h->slot_count * sizeof(CK_SLOT_ID));
  if (slots == NULL) {
    set_error("not enough free memory available");
    return -1;
  }
  h->slots = malloc(h->slot_count * sizeof(slot_t));
  if (h->slots == NULL) {
    free(slots);
    set_error("not enough free memory available");
    return -1;
  }
  memset(h->slots, 0, h->slot_count * sizeof(slot_t));
  rv = h->fl->C_GetSlotList(FALSE, slots, &h->slot_count);
  if (rv != CKR_OK) {
    free(slots);
    set_error("C_GetSlotList() failed: %x", rv);
    return -1;
  }
  DBG1("number of slots (b): %d", h->slot_count);
  /* show some information about the slots/tokens and setup slot info */
  for (i = 0; i < h->slot_count; i++) {
    CK_SLOT_INFO sinfo;
    CK_TOKEN_INFO tinfo;

    DBG1("slot %d:", i + 1);
    rv = h->fl->C_GetSlotInfo(slots[i], &sinfo);
    if (rv != CKR_OK) {
      free(slots);
      set_error("C_GetSlotInfo() failed: %x", rv);
      return -1;
    }
    h->slots[i].id = slots[i];
    DBG1("- description: %.64s", sinfo.slotDescription);
    DBG1("- manufacturer: %.32s", sinfo.manufacturerID);
    DBG1("- flags: %04lx", sinfo.flags);
    if (sinfo.flags & CKF_TOKEN_PRESENT) {
      DBG("- token:");
      rv = h->fl->C_GetTokenInfo(slots[i], &tinfo);
      if (rv != CKR_OK) {
        free(slots);
        set_error("C_GetTokenInfo() failed: %x", rv);
        return -1;
      }
      DBG1("  - label: %.32s", tinfo.label);
      DBG1("  - manufacturer: %.32s", tinfo.manufacturerID);
      DBG1("  - model: %.16s", tinfo.model);
      DBG1("  - serial: %.16s", tinfo.serialNumber);
      DBG1("  - flags: %04lx", tinfo.flags);
      h->slots[i].token_present = TRUE;
      memcpy(h->slots[i].label, tinfo.label, 32);
      for (j = 31; h->slots[i].label[j] == ' '; j--) h->slots[i].label[j] = 0;
    }
  }
  free(slots);
  return 0;
}

void release_pkcs11_module(pkcs11_handle_t *h)
{
  /* finalise pkcs #11 module */
  if (h->fl != NULL)
    h->fl->C_Finalize(NULL);
  /* unload the module */
  if (h->module_handle != NULL)
    dlclose(h->module_handle);
  /* release all allocated memory */
  if (h->slots != NULL)
    free(h->slots);
  memset(h, 0, sizeof(pkcs11_handle_t));
  free(h);
}

int find_slot_by_number(pkcs11_handle_t *h, int slot_num, unsigned int *slot)
{
   /* zero means find the best slot */
   if (slot_num == 0) {
	for (slot_num = 0; slot_num < h->slot_count && 
				!h->slots[slot_num].token_present; slot_num++);
   } else {
	/* otherwize it's an index into the slot table  (it is *NOT* the slot
	 * id!).... */
	slot_num--;
   }
   if ((slot_num >= h->slot_count) || (!h->slots[slot_num].token_present)) {
	return -1;
   }
   *slot = slot_num;
   return 0;
}
	

int open_pkcs11_session(pkcs11_handle_t *h, unsigned int slot)
{
  int rv;

  DBG1("opening a new PKCS #11 session for slot %d", slot + 1);
  if (slot >= h->slot_count) {
    set_error("invalid slot number %d", slot);
    return -1;
  } 
  /* open a readonly user-session */
  rv = h->fl->C_OpenSession(h->slots[slot].id, CKF_SERIAL_SESSION, NULL, NULL, &h->session);
  if (rv != CKR_OK) {
    set_error("C_OpenSession() failed: %x", rv);
    return -1;
  }
  h->current_slot = slot;
  return 0;
}

int pkcs11_login(pkcs11_handle_t *h, char *password)
{
  int rv;

  DBG("login as user CKU_USER");
  rv = h->fl->C_Login(h->session, CKU_USER, (unsigned char*)password, strlen(password));
  if (rv != CKR_OK) {
    set_error("C_Login() failed: %x", rv);
    return -1;
  }
  return 0;
}

static void free_certs(cert_object_t **certs, int cert_count)
{
  int i;

  for (i = 0; i < cert_count; i++) {
    if (!certs[i]) {
	continue;
    }
    if (certs[i]->x509 != NULL)
      X509_free(certs[i]->x509);
    if (certs[i]->id != NULL)
      free(certs[i]->id);
    free(certs[i]);
  }
  free(certs);
}

int close_pkcs11_session(pkcs11_handle_t *h)
{
  int rv, i;

  /* close user-session */
  DBG("logout user");
  rv = h->fl->C_Logout(h->session);
  if (rv != CKR_OK && rv != CKR_USER_NOT_LOGGED_IN) {
    set_error("C_Logout() failed: %x", rv);
    return -1;
  }
  DBG("closing the PKCS #11 session");
  rv = h->fl->C_CloseSession(h->session);
  if (rv != CKR_OK) {
    set_error("C_CloseSession() failed: %x", rv);
    return -1;
  }
  DBG("releasing keys and certificates");
  if (h->certs != NULL) {
    free_certs(h->certs, h->cert_count);
    h->certs = NULL;
    h->cert_count = 0;
  }
  return 0;
}

/* get a list of certificates */
cert_object_t **get_certificate_list(pkcs11_handle_t *h, int *ncerts) 
{
  CK_BYTE *id_value;
  CK_BYTE *cert_value;
  CK_OBJECT_HANDLE object;
  CK_ULONG object_count;
  X509 *x509;
  cert_object_t **certs = NULL;
  int rv;
  
  CK_OBJECT_CLASS cert_class = CKO_CERTIFICATE;
  CK_CERTIFICATE_TYPE cert_type = CKC_X_509;
  CK_ATTRIBUTE cert_template[] = {
    {CKA_CLASS, &cert_class, sizeof(CK_OBJECT_CLASS)}
    ,
    {CKA_CERTIFICATE_TYPE, &cert_type, sizeof(CK_CERTIFICATE_TYPE)}
    ,
    {CKA_ID, NULL, 0}
    ,
    {CKA_VALUE, NULL, 0}
  };

  if (h->certs) {
    *ncerts = h->cert_count;
    return h->certs;
  }

  rv = h->fl->C_FindObjectsInit(h->session, cert_template, 2);
  if (rv != CKR_OK) {
    set_error("C_FindObjectsInit() failed: %x", rv);
    return NULL;
  }
  while(1) {
    /* look for certificates */
    rv = h->fl->C_FindObjects(h->session, &object, 1, &object_count);
    if (rv != CKR_OK) {
      set_error("C_FindObjects() failed: %x", rv);
      goto getlist_error;
    }
    if (object_count == 0) break; /* no more certs */

    /* Cert found, read */

    /* pass 1: get cert id */

    /* retrieve cert object id length */
    cert_template[2].pValue = NULL;
    cert_template[2].ulValueLen = 0;
    rv = h->fl->C_GetAttributeValue(h->session, object, cert_template, 3);
    if (rv != CKR_OK) {
        set_error("CertID length: C_GetAttributeValue() failed: %x", rv);
        goto getlist_error;
    }
    /* allocate enought space */
    id_value = malloc(cert_template[2].ulValueLen);
    if (id_value == NULL) {
        set_error("CertID malloc(%d): not enough free memory available", cert_template[2].ulValueLen);
        goto getlist_error;
    }
    /* read cert id into allocated space */
    cert_template[2].pValue = id_value;
    rv = h->fl->C_GetAttributeValue(h->session, object, cert_template, 3);
    if (rv != CKR_OK) {
        free(id_value);
        set_error("CertID value: C_GetAttributeValue() failed: %x", rv);
        goto getlist_error;
    }

    /* pass 2: get certificate */

    /* retrieve cert length */
      cert_template[3].pValue = NULL;
      rv = h->fl->C_GetAttributeValue(h->session, object, cert_template, 4);
      if (rv != CKR_OK) {
        set_error("Cert Length: C_GetAttributeValue() failed: %x", rv);
        goto getlist_error;
      }
    /* allocate enought space */
      cert_value = malloc(cert_template[3].ulValueLen);
      if (cert_value == NULL) {
        set_error("Cert Length malloc(%d): not enough free memory available", cert_template[3].ulValueLen);
        goto getlist_error;
      }
    /* read certificate into allocated space */
      cert_template[3].pValue = cert_value;
      rv = h->fl->C_GetAttributeValue(h->session, object, cert_template, 4);
      if (rv != CKR_OK) {
        free(cert_value);
        set_error("Cert Value: C_GetAttributeValue() failed: %x", rv);
        goto getlist_error;
      }

    /* Pass 3: store certificate */

    /* convert to X509 data structure */
      x509 = d2i_X509(NULL, (CK_BYTE **)&cert_template[3].pValue, cert_template[3].ulValueLen);
      if (x509 == NULL) {
        free(id_value);
        free(cert_value);
        set_error("d2i_x509() failed: %s", ERR_error_string(ERR_get_error(), NULL));
        goto getlist_error;
      }
    /* finally add certificate to chain */
    certs= realloc(h->certs,(h->cert_count+1) * sizeof(cert_object_t *));
    if (!certs) {
        free(id_value);
        X509_free(x509);
	set_error("realloc() not space to re-size cert table");
        goto getlist_error;
    }
    h->certs=certs;
    DBG1("Saving Certificate #%d:", h->cert_count + 1);
    certs[h->cert_count] = NULL;
    DBG1("- type: %02x", cert_type);
    DBG1("- id:   %02x", id_value[0]);
    h->certs[h->cert_count] = (cert_object_t *)calloc(sizeof(cert_object_t),1);
    if (h->certs[h->cert_count] == NULL) {
	free(id_value);
        X509_free(x509);
	set_error("malloc() not space to allocate cert object");
        goto getlist_error;
    }
    h->certs[h->cert_count]->type = cert_type;
    h->certs[h->cert_count]->id   = id_value;
    h->certs[h->cert_count]->id_length = cert_template[2].ulValueLen;
    h->certs[h->cert_count]->x509 = x509;
    h->certs[h->cert_count]->private_key = CK_INVALID_HANDLE;
    h->certs[h->cert_count]->key_type = 0;
    ++h->cert_count;

  } /* end of while(1) */

  /* release FindObject Sesion */
  rv = h->fl->C_FindObjectsFinal(h->session);
  if (rv != CKR_OK) {
    set_error("C_FindObjectsFinal() failed: %x", rv);
    free_certs(certs, h->cert_count);
    certs = NULL;
    h->certs = NULL;
    h->cert_count = 0;
    return NULL;
  }

  *ncerts = h->cert_count;

  /* arriving here means that's all right */
  DBG1("Found %d certificates in token",h->cert_count);
  return h->certs;

  /* some error arrived: clean as possible, and return fail */
getlist_error:
  rv = h->fl->C_FindObjectsFinal(h->session);
  if (rv != CKR_OK) {
    set_error("C_FindObjectsFinal() failed: %x", rv);
  }
  free_certs(h->certs, h->cert_count);
  h->certs = NULL;
  h->cert_count = 0;
  return NULL;
}

/* retrieve the private key associated with a given certificate */
int get_private_key(pkcs11_handle_t *h, cert_object_t *cert) {
  CK_OBJECT_CLASS key_class = CKO_PRIVATE_KEY;
  CK_BBOOL key_sign = CK_TRUE;
  CK_KEY_TYPE key_type = CKK_RSA; /* default, should be properly set */
  CK_ATTRIBUTE key_template[] = {
    {CKA_CLASS, &key_class, sizeof(key_class)}
    ,
    {CKA_SIGN, &key_sign, sizeof(key_sign)}
    ,
    {CKA_KEY_TYPE, &key_type, sizeof(key_type)}
    ,
    {CKA_ID, NULL, 0}
  };
  CK_OBJECT_HANDLE object;
  CK_ULONG object_count;
  CK_BYTE *key_id;
  cert_object_t *keys;
  int rv;

  if (cert->private_key != CK_INVALID_HANDLE) {
     /* we've alrady found the private key for this certificate */
     return 0;
  }

  key_template[3].pValue = cert->id;
  key_template[3].ulValueLen = cert->id_length;
  rv = h->fl->C_FindObjectsInit(h->session, key_template, 2);
  if (rv != CKR_OK) {
    set_error("C_FindObjectsInit() failed: %x", rv);
    return -1;
  }
  rv = h->fl->C_FindObjects(h->session, &object, 1, &object_count);
  if (rv != CKR_OK) {
    set_error("C_FindObjects() failed: %x", rv);
    goto get_privkey_failed;
  }
  if (object_count <= 0) {
      /* cert without prk: perhaps CA or CA-chain cert */
      set_error("No private key found for cert: %x", rv);
      goto get_privkey_failed;
  }

  /* and finally release Find session */
  rv = h->fl->C_FindObjectsFinal(h->session);
  if (rv != CKR_OK) {
    set_error("C_FindObjectsFinal() failed: %x", rv);
    return -1;
  }

  cert->private_key = object;
  cert->key_type = CKK_RSA;

  return 0;

get_privkey_failed:
  rv = h->fl->C_FindObjectsFinal(h->session);
  if (rv != CKR_OK) {
    set_error("C_FindObjectsFinal() failed: %x", rv);
  }
  return -1;
}

const char *get_slot_label(pkcs11_handle_t *h)
{
  return h->slots[h->current_slot].label;
}

const X509 *get_X509_certificate(cert_object_t *cert)
{
  return cert->x509;
}

int sign_value(pkcs11_handle_t *h, cert_object_t *cert, CK_BYTE *data, 
	CK_ULONG length, CK_BYTE **signature, CK_ULONG *signature_length)
{
  int rv;
  CK_BYTE hash[15 + SHA_DIGEST_LENGTH] =
      "\x30\x21\x30\x09\x06\x05\x2b\x0e\x03\x02\x1a\x05\x00\x04\x14";
  CK_MECHANISM mechanism = { 0, NULL, 0 };


  if (get_private_key(h, cert) == -1) {
    set_error("Couldn't find private key for certificate");
    return -1;
  } 

  /* set mechanism */
  switch (cert->key_type) {
    case CKK_RSA:
      mechanism.mechanism = CKM_RSA_PKCS;
      break;
    default:
      set_error("unsupported key type %d", cert->type);
      return -1;
  }
  /* compute hash-value */
  SHA1(data, length, &hash[15]);
  DBG5("hash[%d] = [...:%02x:%02x:%02x:...:%02x]", sizeof(hash),
      hash[15], hash[16], hash[17], hash[sizeof(hash) - 1]);
  /* sign the token */
  rv = h->fl->C_SignInit(h->session, &mechanism, cert->private_key);
  if (rv != CKR_OK) {
    set_error("C_SignInit() failed: %x", rv);
    return -1;
  }
  *signature = NULL;
  *signature_length = 128;
  while (*signature == NULL) {
    *signature = malloc(*signature_length);
    if (*signature == NULL) {
      set_error("not enough free memory available");
      return -1;
    }
    rv = h->fl->C_Sign(h->session, hash, sizeof(hash), *signature, signature_length);
    if (rv == CKR_BUFFER_TOO_SMALL) {
      /* increase signature length as long as it it to short */
      free(*signature);
      *signature = NULL;
      *signature_length *= 2;
      DBG1("increased signature buffer-length to %d", *signature_length);
    } else if (rv != CKR_OK) {
      free(*signature);
      *signature = NULL;
      set_error("C_Sign() failed: %x", rv);
      return -1;
    }
  }
  DBG5("signature[%d] = [%02x:%02x:%02x:...:%02x]", *signature_length,
      (*signature)[0], (*signature)[1], (*signature)[2], (*signature)[*signature_length - 1]);
  return 0;
}

int get_random_value(unsigned char *data, int length)
{
  static const char *random_device = "/dev/urandom";
  int rv, fh, l;

  DBG2("reading %d random bytes from %s", length, random_device);
  fh = open(random_device, O_RDONLY);
  if (fh == -1) {
    set_error("open() failed: %s", strerror(errno));
    return -1;
  }

  l = 0;
  while (l < length) {
    rv = read(fh, data + l, length - l);
    if (rv <= 0) {
      close(fh);
      set_error("read() failed: %s", strerror(errno));
      return -1;
    }
    l += rv;
  }
  close(fh);
  DBG5("random-value[%d] = [%02x:%02x:%02x:...:%02x]", length, data[0],
      data[1], data[2], data[length - 1]);
  return 0;
}
#endif /* HAVE_NSS */