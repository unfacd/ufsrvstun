/**
 * @file utils_console_handlers.c
 * @brief ufsrvstun command-console handlers, registered with the corelib
 *        dispatch tree.
 */

#include "ufsrv_console_handlers.h"

#include <stdio.h>
#include <stdlib.h>

#include <uflib/buffer_descriptor/buffer_descriptor.h>
#include <uflib/version.h>
#include <ufsrv_core/ufsrv_config/ufsrv_config.h>
#include <ufsrvcorelib/ufsrv_console_script/ufsrv_console_script.h>
#include <ufsrvcorelib/version.h>

#include "version.h"

#ifndef APP_NAME_INJECTED
# define APP_NAME_INJECTED "ufsrvstun"
#endif

static BufferDescriptor *
sLiveBuffer(BufferDescriptor *buffer_provided)
{
  if (buffer_provided) return buffer_provided;

  buffer_provided = calloc(1, sizeof(BufferDescriptor));
  if (!buffer_provided) return NULL;

  BufferDescriptorInit(buffer_provided, 256);
  if (!buffer_provided->data) {
    free(buffer_provided);
    return NULL;
  }

  return buffer_provided;
}

static BufferDescriptor *
StunConsoleVersionSelfOnGet(ScriptHandlerContext *ctx)
{
  BufferDescriptor *out = sLiveBuffer(ctx->buffer);
  if (!out) return NULL;

  BufferDescriptorAppendFormatted(out,
      "{\"name\":\"%s\",\"version\":\"%s\",\"internal\":\"%s\",\"compiled\":\"%s\",\"by\":\"%s\"}",
      APP_NAME_INJECTED, UfsrvVersion(), UfsrvVersionInternal(),
      UfsrvCompiledTimestamp(), UfsrvCompiledBy());
  return out;
}

static BufferDescriptor *
StunConsoleLinkedLibsOnGet(ScriptHandlerContext *ctx)
{
  BufferDescriptor *out = sLiveBuffer(ctx->buffer);
  if (!out) return NULL;

  BufferDescriptorAppendFormatted(out, "{\"uflib\":\"%s\",\"ufsrvcorelib\":\"%s\"}",
      UfsrvUfLibVersion(), UfsrvCoreLibVersion());
  return out;
}

void
StunConsoleRegisterHandlers(void)
{
  UfsrvConsoleScriptHandler h = {0};

  h.on_get = StunConsoleVersionSelfOnGet;
  h.methods = UFSRV_CONSOLE_METHOD_GET;
  UfsrvConsoleScriptRegister("/internals/version/self", "server binary version", &h);

  h.on_get = StunConsoleLinkedLibsOnGet;
  h.methods = UFSRV_CONSOLE_METHOD_GET;
  UfsrvConsoleScriptRegister("/internals/version/linked_libs", "linked uf** library versions", &h);
}
