/*
 * Copyright (C) 2016 Gustavo Sverzut Barbieri
 * Copyright (c) 2021 Comcast Cable Communications Management, LLC
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * https://opensource.org/licenses/MIT
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cb.h"
#include "header.h"
#include "utils.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
static size_t _header_cb(const char*, size_t, size_t, void*);
static void _check_accept(CWS*, const char*, size_t);
static void _check_protocol(CWS*, const char*, size_t);
static void _check_upgrade(CWS*, const char*, size_t);
static void _check_connection(CWS*, const char*, size_t);

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

void header_init(CWS *priv)
{
    curl_easy_setopt(priv->easy, CURLOPT_HEADERFUNCTION, _header_cb);
    curl_easy_setopt(priv->easy, CURLOPT_HEADERDATA, priv);
}


/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/

static size_t _header_cb(const char *buffer, size_t count, size_t nitems, void *data)
{
    CWS *priv = data;
    long http_status = -1;
    long http_version = -1;
    const size_t len = count * nitems;
    const struct header_checker {
        const char *prefix;
        void (*check)(CWS *priv, const char *suffix, size_t suffixlen);
    } header_checkers[] = {
        {"Sec-WebSocket-Accept:",   _check_accept},
        {"Sec-WebSocket-Protocol:", _check_protocol},
        {"Connection:",             _check_connection},
        {"Upgrade:",                _check_upgrade},
        {NULL, NULL}
    };

    curl_easy_getinfo(priv->easy, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_getinfo(priv->easy, CURLINFO_HTTP_VERSION,  &http_version);

    /* If we are being redirected, let curl do that for us. */
    if (300 <= http_status && http_status <= 399) {
        priv->redirection = true;
        return len;
    } else {
        priv->redirection = false;
    }

    /* Only accept `HTTP/1.1 101 Switching Protocols` */
    if ((101 != http_status) || (CURL_HTTP_VERSION_1_1 != http_version)) {
        return 0;
    }

    /* We've reached the end of the headers. */
    if (len == 2 && memcmp(buffer, "\r\n", 2) == 0) {
        if (!priv->accepted) {
            priv->dispatching++;
            cb_on_close(priv, 1011, "server didn't accept the websocket upgrade", SIZE_MAX);
            priv->dispatching--;
            _cws_cleanup(priv);
            return 0;
        } else {
            priv->dispatching++;
            cb_on_connect(priv, priv->ws_protocols_received);
            priv->dispatching--;
            _cws_cleanup(priv);
            return len;
        }
    }

    if (cws_has_prefix(buffer, len, "HTTP/")) {
        priv->accepted = false;
        priv->upgraded = false;
        priv->connection_websocket = false;
        if (priv->ws_protocols_received) {
            free(priv->ws_protocols_received);
            priv->ws_protocols_received = NULL;
        }
        return len;
    }

    for (const struct header_checker *itr = header_checkers; itr->prefix != NULL; itr++) {
        if (cws_has_prefix(buffer, len, itr->prefix)) {
            size_t prefixlen = strlen(itr->prefix);
            size_t valuelen = len - prefixlen;
            const char *value = buffer + prefixlen;
            value = cws_trim(value, &valuelen);
            itr->check(priv, value, valuelen);
        }
    }

    return len;
}

static void _check_accept(CWS *priv, const char *buffer, size_t len)
{
    priv->accepted = false;

    if (len != strlen(priv->expected_key_header)) {
        if (priv->cfg.verbose) {
            fprintf(stderr,
                    "< websocket header 'Sec-WebSocket-Accept' "
                    "expected %zu bytes, got %zu '%.*s'",
                    strlen(priv->expected_key_header) - 1, len, (int)len, buffer);
        }
        return;
    }

    if (memcmp(priv->expected_key_header, buffer, len) != 0) {
        if (priv->cfg.verbose) {
            fprintf(stderr,
                    "< websocket header 'Sec-WebSocket-Accept' "
                    "invalid accept key '%.*s', expected '%.*s'",
                    (int)len, buffer, (int)len, priv->expected_key_header);
        }
        return;
    }

    priv->accepted = true;
}


static void _check_protocol(CWS *priv, const char *buffer, size_t len)
{
    if (priv->ws_protocols_received) {
        free(priv->ws_protocols_received);
    }

    priv->ws_protocols_received = cws_strndup(buffer, len);
}


static void _check_upgrade(CWS *priv, const char *buffer, size_t len)
{
    priv->connection_websocket = false;

    if (cws_strncasecmp(buffer, "websocket", len)) {
        if (priv->cfg.verbose) {
            fprintf(stderr,
                    "< websocket header error 'Upgrade: %.*s'. "
                    "Expected 'Upgrade: websocket'",
                    (int)len, buffer);
        }
        return;
    }

    priv->connection_websocket = true;
}


static void _check_connection(CWS *priv, const char *buffer, size_t len)
{
    priv->upgraded = false;

    if (cws_strncasecmp(buffer, "upgrade", len)) {
        if (priv->cfg.verbose) {
            fprintf(stderr,
                    "< websocket header error 'Connection: %.*s'. "
                    "Expected 'Connection: upgrade'",
                    (int)len, buffer);
        }
        return;
    }

    priv->upgraded = true;
}


