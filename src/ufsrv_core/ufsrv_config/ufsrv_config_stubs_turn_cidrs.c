#include <stdlib.h>
#include <string.h>

#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/utils_collection_str.h>
#include <ufsrv_core/ufsrv_config/ufsrv_config.h>

#include "ufsrvstun_config.h"
#include "ufsrv_config_stubs_as_string_priv.h"

static void
SetCidrs(enum UfsrvConfigField field, const UfsrvConfigValue *value, UfsrvConfigSource source)
{
  UfsrvConfigValue *slot = UfsrvConfigConfFileValuesInstance(field);
  const char *blob = value->stored_value.blob_val;
  size_t count = blob ? value->collection_sz : 0;
  size_t bytes = blob ? value->type_allocation_sz : 0;
  CollectionDescriptor *list = NULL;
  if (bytes <= SIZE_MAX - sizeof(*list) &&
      count <= (SIZE_MAX - sizeof(*list) - bytes) / sizeof(*list->collection)) {
    list = calloc(1, sizeof(*list) + count * sizeof(*list->collection) + bytes);
  }
  if (list) {
    list->collection_sz = count;
    list->collection = (collection_t **)(list + 1);
    char *text = (char *)(list->collection + count);
    if (bytes) memcpy(text, blob, bytes);
    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
      const char *end = memchr(text + offset, '\0', bytes - offset);
      if (!end) break;
      list->collection[i] = text + offset;
      offset = (size_t)(end - text) + 1;
    }
    if (offset != bytes || (count && !list->collection[count - 1])) {
      free(list);
      list = NULL;
    }
  }
  free(slot->stored_value.blob_val);
  slot->stored_value.blob_val = list;
  slot->source = source;
}

void
UfsrvConfigSetStunTurnPeerAllowCidrs(UfsrvConfigItem *item, UfsrvConfigSource source)
{
  SetCidrs(UFSRV_CONFIG_FIELD_STUN_TURN_PEER_ALLOW_CIDRS, &item->value, source);
}

void
UfsrvConfigSetStunTurnPeerDenyCidrs(UfsrvConfigItem *item, UfsrvConfigSource source)
{
  SetCidrs(UFSRV_CONFIG_FIELD_STUN_TURN_PEER_DENY_CIDRS, &item->value, source);
}

static BufferDescriptor *
CidrsAsString(UfsrvConfigValue *value, BufferDescriptor *buffer)
{
  if (!value || !buffer) return NULL;
  char *json = value->stored_value.blob_val ? CollectionFromStringDescribeAsJson(value->stored_value.blob_val) : NULL;
  int result = json ? BufferDescriptorAppendFormatted(buffer, "%s", json) : -1;
  free(json);
  if (result == 0) return buffer;
  BufferDescriptorRelease(buffer);
  return NULL;
}

BufferDescriptor *
UfsrvConfigStunTurnPeerAllowCidrsAsString_CollectionDescriptor(int field, UfsrvConfigValue *value, BufferDescriptor *buffer)
{
  (void)field;
  return CidrsAsString(value, buffer);
}

BufferDescriptor *
UfsrvConfigStunTurnPeerDenyCidrsAsString_CollectionDescriptor(int field, UfsrvConfigValue *value, BufferDescriptor *buffer)
{
  (void)field;
  return CidrsAsString(value, buffer);
}
