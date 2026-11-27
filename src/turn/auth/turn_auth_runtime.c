/**
 * @file turn_auth_runtime.c
 * @brief Load encrypted standalone TURN credentials at startup.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <openssl/crypto.h>
#include <uflib/utils_hex.h>
#include <ufsrv_core/ufsrv_config/ufsrv_config_secret.h>

#include "turn/auth/turn_auth_runtime.h"
#include "ufsrvstun_config.h"

enum { MAX_CREDENTIAL_ENVELOPE_BYTES = 1024 * 1024 };

static void
ClearFree(char *value, size_t length)
{
  if (value) OPENSSL_cleanse(value, length);
  free(value);
}

static bool
DecryptText(const char *envelope, const unsigned char key[SECRET_KEY_BYTES], char **text, size_t *length)
{
  if (strncmp(envelope, SECRET_PREFIX, SECRET_PREFIX_LEN) != 0) return false;
  const char *ciphertext = strchr(envelope + SECRET_PREFIX_LEN, ':');
  if (!ciphertext) return false;
  ciphertext++;
  const char *end = strchr(ciphertext, ':');
  if (!end) return false;
  *length = (size_t)(end - ciphertext) / 2;
  return SecretConfigDecrypt(envelope, key, text) == 0 && memchr(*text, '\0', *length) == NULL;
}

static bool
ReadServerKey(unsigned char key[SECRET_KEY_BYTES])
{
  const char *credentials = getenv("CREDENTIALS_DIRECTORY");
  if (!credentials || !*credentials) return SecretConfigReadKeyFromCredential(key, UfsrvConfigUfsrvSecretKeyStore()) ==
    0;

  char      path[4096];
  const int length = snprintf(path, sizeof(path), "%s/%s", credentials, SECRET_CREDENTIAL_ID);
  return length > 0 && (size_t)length < sizeof(path) && SecretConfigReadKeyFromCredential(key, path) == 0;
}

static char *
ReadCredentialEnvelope(void)
{
  const char *file = UfsrvConfigStunTurnCredentialFile();
  char        path[4096];
  if (*file != '/') {
    const int length = snprintf(path, sizeof(path), "%s/etc/%s", UfsrvConfigConfigDir(), file);
    if (length < 0 || (size_t)length >= sizeof(path)) return NULL;
    file = path;
  }

  FILE *stream = fopen(file, "rb");
  if (!stream) return NULL;
  char *envelope = NULL;
  if (fseek(stream, 0, SEEK_END) == 0) {
    const long length = ftell(stream);
    if (length > 0 && length <= MAX_CREDENTIAL_ENVELOPE_BYTES && fseek(stream, 0, SEEK_SET) == 0) {
      envelope = malloc((size_t)length + 1);
      if (envelope && fread(envelope, 1, (size_t)length, stream) == (size_t)length && !
        memchr(envelope, '\0', (size_t)length) && fgetc(stream) == EOF && !ferror(stream)) {
        size_t end = (size_t)length;
        while (end && (envelope[end - 1] == '\n' || envelope[end - 1] == '\r')) end--;
        envelope[end] = '\0';
      }
      else {
        free(envelope);
        envelope = NULL;
      }
    }
  }
  fclose(stream);
  return envelope;
}

static bool
ReadHex(const char *value, uint8_t *out, size_t bytes)
{
  if (strlen(value) != bytes * 2) return false;
  for (size_t i = 0; i < bytes; i++) {
    char hi, lo;
    if (!hexchr2bin(value[i * 2], &hi) || !hexchr2bin(value[i * 2 + 1], &lo)) return false;
    out[i] = (uint8_t)(((unsigned char)hi << 4) | (unsigned char)lo);
  }
  return true;
}

/*! @brief Temporary key buffers wiped once the standalone store copies them. */
typedef struct TurnProvisioningKeys
{
  uint8_t md5[TURN_MD5_KEY_LENGTH];       ///< Optional MD5-derived key.
  uint8_t sha256[TURN_SHA256_KEY_LENGTH]; ///< Optional SHA-256-derived key.
} TurnProvisioningKeys;

static bool
ParseCredential(char *line, TurnProvisionedCredential *record, TurnProvisioningKeys *keys)
{
  char *username  = strsep(&line, "\t");
  char *principal = strsep(&line, "\t");
  char *md5       = strsep(&line, "\t");
  char *sha256    = strsep(&line, "\t");
  char *expiry    = strsep(&line, "\t");
  if (!*username || !principal || !md5 || !sha256 || !expiry || line) return false;

  const size_t principal_length = strlen(principal) / 2;
  if (!principal_length || principal_length > sizeof(record->principal.bytes) || !ReadHex(
    principal, record->principal.bytes, principal_length)) return false;
  record->principal.length = principal_length;
  if (strcmp(md5, "-") != 0) {
    if (!ReadHex(md5, keys->md5, sizeof(keys->md5))) return false;
    record->md5_key = keys->md5;
  }
  if (strcmp(sha256, "-") != 0) {
    if (!ReadHex(sha256, keys->sha256, sizeof(keys->sha256))) return false;
    record->sha256_key = keys->sha256;
  }
  if (!record->md5_key && !record->sha256_key) return false;

  if (!*expiry || expiry[strspn(expiry, "0123456789")] != '\0') return false;
  errno              = 0;
  uintmax_t expires  = strtoumax(expiry, NULL, 10);
  record->expires_at = (time_t)expires;
  if (errno == ERANGE || record->expires_at < 0 || (uintmax_t)record->expires_at != expires) return false;

  record->username = username;
  record->realm    = UfsrvConfigStunTurnRealm();
  return true;
}

static TurnCredentialStore *
LoadStore(char *text)
{
  size_t lines = 1;
  for (const char *p = text; *p; p++) if (*p == '\n') lines++;
  TurnProvisionedCredential *records = calloc(lines, sizeof(*records));
  TurnProvisioningKeys *     keys    = OPENSSL_zalloc(lines * sizeof(*keys));
  TurnCredentialStore *      store   = NULL;
  if (!records || !keys) goto cleanup;

  size_t count = 0;
  char * line;
  while ((line = strsep(&text, "\n"))) {
    size_t length = strlen(line);
    if (length && line[length - 1] == '\r') line[--length] = '\0';
    if (!length || *line == '#') continue;
    if (!ParseCredential(line, &records[count], &keys[count])) goto cleanup;
    count++;
  }
  if (count) store = TurnCredentialStoreCreate(records, count);

cleanup:
  OPENSSL_clear_free(keys, lines * sizeof(*keys));
  free(records);
  return store;
}

void
StunTurnAuthDestroy(StunTurnAuthRuntime *runtime)
{
  TurnAuthDestroy(runtime->auth);
  TurnCredentialStoreDestroy(runtime->standalone);
  TurnUnfacdCredentialDestroy(runtime->unfacd);
  *runtime = (StunTurnAuthRuntime)
  {
    0
  };
}

bool
StunTurnAuthLoad(StunTurnAuthRuntime *runtime)
{
  *runtime = (StunTurnAuthRuntime)
  {
    0
  };
  if (!UfsrvConfigStunTurnIsEnabled()) return true;

  unsigned char          server_key[SECRET_KEY_BYTES]         = {0};
  uint8_t                nonce_key[32]                        = {0};
  uint8_t                realm_key[TURN_UNFACD_SECRET_LENGTH] = {0};
  char *                 nonce_text                           = NULL;
  size_t                 nonce_length                         = 0;
  char *                 credential_envelope                  = NULL;
  char *                 credential_text                      = NULL;
  size_t                 credential_length                    = 0;
  const char *           realm                                = UfsrvConfigStunTurnRealm();
  TurnCredentialProvider provider;

  if (!ReadServerKey(server_key) || !DecryptText(UfsrvConfigStunTurnNonceSecret(), server_key, &nonce_text,
                                                 &nonce_length) || !ReadHex(nonce_text, nonce_key, sizeof(nonce_key))) {
    syslog(LOG_ERR, "Could not load encrypted TURN nonce secret");
    goto cleanup;
  }

  if (strcmp(UfsrvConfigStunTurnCredentialProvider(), "unfacd") == 0) {
    if (!DecryptText(UfsrvConfigStunTurnRealmCredentialSecret(), server_key, &credential_text, &credential_length) || !
      ReadHex(credential_text, realm_key, sizeof(realm_key)) || !(runtime->unfacd = TurnUnfacdCredentialCreate(
        realm, realm_key, UfsrvConfigStunTurnMaxCredentialLifetimeSeconds()))) {
      syslog(LOG_ERR, "Could not load encrypted Unfacd TURN realm credential secret");
      goto cleanup;
    }
    provider = TurnUnfacdCredentialProvider(runtime->unfacd);
  }
  else {
    credential_envelope = ReadCredentialEnvelope();
    if (!credential_envelope || !DecryptText(credential_envelope, server_key, &credential_text, &credential_length) || !
      (runtime->standalone = LoadStore(credential_text))) {
      syslog(LOG_ERR, "Could not load encrypted TURN credential records");
      goto cleanup;
    }
    provider = TurnCredentialStoreProvider(runtime->standalone);
  }

  runtime->auth = TurnAuthCreate(realm, nonce_key, sizeof(nonce_key), UfsrvConfigStunTurnNonceLifetimeSeconds(),
                                 provider);

cleanup:
  OPENSSL_cleanse(server_key, sizeof(server_key));
  OPENSSL_cleanse(nonce_key, sizeof(nonce_key));
  OPENSSL_cleanse(realm_key, sizeof(realm_key));
  ClearFree(nonce_text, nonce_length);
  ClearFree(credential_text, credential_length);
  free(credential_envelope);
  if (runtime->auth) return true;
  StunTurnAuthDestroy(runtime);
  return false;
}
