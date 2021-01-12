/*
 *  engine-parsec.h -- parsec default types and defines
 *
 *  Copyright (c) 2020 Maik Broemme <mbroemme@libmpq.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _ENGINE_PARSEC_H
#define _ENGINE_PARSEC_H

/* define audio defaults. */
#define VDI_AUDIO_CHANNELS		2
#define VDI_AUDIO_SAMPLE_RATE		48000
#define VDI_AUDIO_FRAMES_PER_PACKET	960

/* define parsec messages. */
#define PARSEC_CLIPBOARD_MSG		7

/* parsec event loop. */
int32_t vdi_stream_client__event_loop(vdi_config_s *vdi_config);

#endif /* _ENGINE_PARSEC_H */
