/*
 *  input.h -- input handling thread via sdl
 *
 *  Copyright (c) 2026 Maik Broemme <mbroemme@libmpq.org>
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
 *
 *  Additional permission under GNU GPL version 3 section 7 is described in
 *  COPYING.EXCEPTION, allowing this program to link with the Parsec SDK.
 */

#ifndef VDI_STREAM_CLIENT_INPUT_H
#define VDI_STREAM_CLIENT_INPUT_H

#include "client.h"
#include "parsec.h"

#define VDI_STREAM_CLIENT_INPUT_COMMANDS 64

typedef enum vdi_stream_client__input_command_e
{
    VDI_STREAM_CLIENT_INPUT_COMMAND_QUIT = 1,
    VDI_STREAM_CLIENT_INPUT_COMMAND_RELEASE_GRAB,
    VDI_STREAM_CLIENT_INPUT_COMMAND_FORCE_GRAB_ENABLE,
    VDI_STREAM_CLIENT_INPUT_COMMAND_FORCE_GRAB_DISABLE,
    VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_BUTTON_DOWN,
    VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_BUTTON_UP,
    VDI_STREAM_CLIENT_INPUT_COMMAND_CLIPBOARD_UPDATE,
    VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_ENTER,
    VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_LEAVE,
    VDI_STREAM_CLIENT_INPUT_COMMAND_WINDOW_RESIZED,
} vdi_stream_client__input_command_e;

typedef struct vdi_stream_client__input_command_s
{
    vdi_stream_client__input_command_e type;
    bool grab_forced;
} vdi_stream_client__input_command_s;

typedef struct vdi_stream_client__input_context_s
{
    struct parsec_context_s *parsec_context;
    vdi_config_s *vdi_config;
    SDL_Mutex *command_lock;
    vdi_stream_client__input_command_s commands[VDI_STREAM_CLIENT_INPUT_COMMANDS];
    Uint32 command_read;
    Uint32 command_write;
    SDL_MouseButtonFlags mouse_buttons;
} vdi_stream_client__input_context_s;

bool vdi_stream_client__input_init(
    vdi_stream_client__input_context_s *input_context, struct parsec_context_s *parsec_context,
    vdi_config_s *vdi_config
);
void vdi_stream_client__input_destroy(vdi_stream_client__input_context_s *input_context);
bool vdi_stream_client__input_next_command(
    vdi_stream_client__input_context_s *input_context, vdi_stream_client__input_command_s *command
);
Sint32 vdi_stream_client__input_thread(void *opaque);

#endif /* VDI_STREAM_CLIENT_INPUT_H */
