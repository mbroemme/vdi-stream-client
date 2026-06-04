/*
 *  placebo.h -- VA-API zero-copy rendering via libplacebo
 *
 *  Copyright (c) 2026 Maik Broemme <mbroemme@libmpq.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Additional permission under GNU GPL version 3 section 7 is described in
 *  COPYING.EXCEPTION, allowing this program to link with the Parsec SDK.
 */

#ifndef VDI_STREAM_CLIENT_PLACEBO_H
#define VDI_STREAM_CLIENT_PLACEBO_H

#include "parsec.h"

bool vdi_stream_client__placebo_init(struct parsec_context_s *parsec_context);
bool vdi_stream_client__placebo_render(
    struct parsec_context_s *parsec_context, const ParsecFrame *frame, const void *image
);
void vdi_stream_client__placebo_destroy(struct parsec_context_s *parsec_context);

#endif /* VDI_STREAM_CLIENT_PLACEBO_H */
