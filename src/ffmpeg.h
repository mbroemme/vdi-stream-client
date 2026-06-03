/*
 *  ffmpeg.h -- FFmpeg decoder integration for Parsec SDK
 *
 *  Copyright (c) 2020-2026 Maik Broemme <mbroemme@libmpq.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Additional permission under GNU GPL version 3 section 7 is described in
 *  COPYING.EXCEPTION, allowing this program to link with the Parsec SDK.
 */

#ifndef _FFMPEG_H
#define _FFMPEG_H

#include "parsec.h"

bool
vdi_stream_client__parsec_ffmpeg_frame_is_descriptor(const ParsecFrame *frame, const void *image);
bool vdi_stream_client__parsec_ffmpeg_frame_texture_format(
    const ParsecFrame *frame, const void *image, SDL_PixelFormat *pixel_format
);
bool vdi_stream_client__parsec_ffmpeg_frame_update(
    SDL_Texture *texture, const ParsecFrame *frame, const void *image
);
void vdi_stream_client__parsec_ffmpeg_frame_release(const ParsecFrame *frame, const void *image);

bool vdi_stream_client__parsec_ffmpeg_decoder_enable(
    struct parsec_context_s *parsec_context, Uint32 *decoder_index, bool acceleration
);

#endif /* _FFMPEG_H */
