/*
 * hxm_diag.h - Diagnostics helpers for debug builds.
 *
 * When HXM_DIAG=0, these compile to no-ops and introduce no symbols.
 */

#ifndef HXM_DIAG_H
#define HXM_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "client.h"

typedef struct server server_t;

#if HXM_DIAG
void diag_dump_layer(const server_t* s, layer_t l, const char* tag);
void diag_dump_focus_history(const server_t* s, const char* tag);
void diag_dump_transients(const client_hot_t* hot, const char* tag);
#else
static inline void diag_dump_layer(const server_t* s, layer_t l, const char* tag) {
    (void)s;
    (void)l;
    (void)tag;
}

static inline void diag_dump_focus_history(const server_t* s, const char* tag) {
    (void)s;
    (void)tag;
}

static inline void diag_dump_transients(const client_hot_t* hot, const char* tag) {
    (void)hot;
    (void)tag;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* HXM_DIAG_H */
