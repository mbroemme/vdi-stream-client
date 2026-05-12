/*
 *  parsec_ffmpeg.h -- injected FFmpeg decoder for Parsec SDK
 *
 *  Copyright (c) 2020-2026 Maik Broemme <mbroemme@libmpq.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef _PARSEC_FFMPEG_H
#define _PARSEC_FFMPEG_H

#include "parsec.h"

bool vdi_stream_client__parsec_ffmpeg_decoder_enable(struct parsec_context_s *parsec_context, Uint32 *decoder_index, bool request_color444);

#endif /* _PARSEC_FFMPEG_H */
