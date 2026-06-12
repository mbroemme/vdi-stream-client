/*
 *  input.c -- input handling thread via sdl
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

/* configuration includes. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* internal includes. */
#include "input.h"

static bool
vdi_stream_client__input_queue_command(
    vdi_stream_client__input_context_s *input_context, vdi_stream_client__input_command_e type,
    bool grab_forced
)
{
    Uint32 next;

    if (input_context == NULL || input_context->command_lock == NULL) {
        return false;
    }

    SDL_LockMutex(input_context->command_lock);
    next = (input_context->command_write + 1u) % VDI_STREAM_CLIENT_INPUT_COMMANDS;
    if (next == input_context->command_read) {
        SDL_UnlockMutex(input_context->command_lock);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Input command queue is full\n");
        return false;
    }

    input_context->commands[input_context->command_write].type = type;
    input_context->commands[input_context->command_write].grab_forced = grab_forced;
    input_context->command_write = next;
    SDL_UnlockMutex(input_context->command_lock);
    return true;
}

bool
vdi_stream_client__input_init(
    vdi_stream_client__input_context_s *input_context, struct parsec_context_s *parsec_context,
    vdi_config_s *vdi_config
)
{
    if (input_context == NULL || parsec_context == NULL || vdi_config == NULL) {
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initialize Input\n");
    SDL_memset(input_context, 0, sizeof(*input_context));
    input_context->parsec_context = parsec_context;
    input_context->vdi_config = vdi_config;
    input_context->command_lock = SDL_CreateMutex();
    if (input_context->command_lock == NULL) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Input command mutex creation failed: %s\n",
            SDL_GetError()
        );
        return false;
    }
    return true;
}

void
vdi_stream_client__input_destroy(vdi_stream_client__input_context_s *input_context)
{
    if (input_context == NULL) {
        return;
    }

    if (input_context->command_lock != NULL) {
        SDL_DestroyMutex(input_context->command_lock);
        input_context->command_lock = NULL;
    }
}

bool
vdi_stream_client__input_next_command(
    vdi_stream_client__input_context_s *input_context, vdi_stream_client__input_command_s *command
)
{
    if (command != NULL) {
        SDL_memset(command, 0, sizeof(*command));
    }
    if (input_context == NULL || input_context->command_lock == NULL || command == NULL) {
        return false;
    }

    SDL_LockMutex(input_context->command_lock);
    if (input_context->command_read == input_context->command_write) {
        SDL_UnlockMutex(input_context->command_lock);
        return false;
    }

    *command = input_context->commands[input_context->command_read];
    input_context->command_read =
        (input_context->command_read + 1u) % VDI_STREAM_CLIENT_INPUT_COMMANDS;
    SDL_UnlockMutex(input_context->command_lock);
    return true;
}

static void
vdi_stream_client__input_send_message(
    struct parsec_context_s *parsec_context, const ParsecMessage *pmsg
)
{
    if (pmsg == NULL || pmsg->type == 0 || !vdi_stream_client__context_connected(parsec_context)) {
        return;
    }

    vdi_stream_client__context_set_input_polling(parsec_context, true);
    if (vdi_stream_client__context_connected(parsec_context) &&
        !vdi_stream_client__context_done(parsec_context)) {
        ParsecClientSendMessage(parsec_context->parsec, pmsg);
    }
    vdi_stream_client__context_set_input_polling(parsec_context, false);
}

static bool
vdi_stream_client__input_mouse_buttons_pressed(
    const vdi_stream_client__input_context_s *input_context
)
{
    return (input_context->mouse_buttons &
            (SDL_BUTTON_LMASK | SDL_BUTTON_MMASK | SDL_BUTTON_RMASK)) != 0;
}

static void
vdi_stream_client__input_handle_key_down(
    vdi_stream_client__input_context_s *input_context, const SDL_Event *msg, ParsecMessage *pmsg
)
{
    struct parsec_context_s *parsec_context = input_context->parsec_context;
    bool grab_forced = vdi_stream_client__context_input_grab_forced(parsec_context);

    if (input_context->vdi_config->grab == 1 && !grab_forced &&
        (msg->key.mod & SDL_KMOD_LCTRL) != 0 && (msg->key.mod & SDL_KMOD_LALT) != 0 &&
        !vdi_stream_client__input_mouse_buttons_pressed(input_context)) {
        vdi_stream_client__input_queue_command(
            input_context, VDI_STREAM_CLIENT_INPUT_COMMAND_RELEASE_GRAB, grab_forced
        );
        return;
    }

    if (input_context->vdi_config->grab == 0 && (msg->key.mod & SDL_KMOD_LSHIFT) != 0 &&
        msg->key.key == SDLK_F12) {
        vdi_stream_client__context_set_input_grab_forced(parsec_context, !grab_forced);
        if (!vdi_stream_client__input_queue_command(
                input_context,
                grab_forced ? VDI_STREAM_CLIENT_INPUT_COMMAND_FORCE_GRAB_DISABLE
                            : VDI_STREAM_CLIENT_INPUT_COMMAND_FORCE_GRAB_ENABLE,
                grab_forced
            )) {
            vdi_stream_client__context_set_input_grab_forced(parsec_context, grab_forced);
        }
        return;
    }

    pmsg->type = MESSAGE_KEYBOARD;
    pmsg->keyboard.code = (ParsecKeycode)msg->key.scancode;
    pmsg->keyboard.mod = msg->key.mod;
    pmsg->keyboard.pressed = true;
}

static void
vdi_stream_client__input_handle_event(
    vdi_stream_client__input_context_s *input_context, const SDL_Event *msg
)
{
    struct parsec_context_s *parsec_context = input_context->parsec_context;
    ParsecMessage pmsg = { 0 };

    if (parsec_context->stats_enabled) {
        atomic_fetch_add_explicit(
            &parsec_context->stats_sdl_events, (uint_fast64_t)1, memory_order_relaxed
        );
    }
    vdi_stream_client__context_set_input_local_interaction(parsec_context);
    if (!vdi_stream_client__context_connected(parsec_context)) {
        vdi_stream_client__context_set_input_force_redraw(parsec_context);
    }

    switch (msg->type) {
    case SDL_EVENT_QUIT:
        vdi_stream_client__context_set_input_force_redraw(parsec_context);
        vdi_stream_client__input_queue_command(
            input_context, VDI_STREAM_CLIENT_INPUT_COMMAND_QUIT, false
        );
        break;
    case SDL_EVENT_KEY_UP:
        pmsg.type = MESSAGE_KEYBOARD;
        pmsg.keyboard.code = (ParsecKeycode)msg->key.scancode;
        pmsg.keyboard.mod = msg->key.mod;
        pmsg.keyboard.pressed = false;
        break;
    case SDL_EVENT_KEY_DOWN:
        vdi_stream_client__input_handle_key_down(input_context, msg, &pmsg);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (vdi_stream_client__context_input_relative(parsec_context) &&
            !vdi_stream_client__context_input_relative_mouse(parsec_context)) {
            break;
        }
        pmsg.type = MESSAGE_MOUSE_MOTION;
        pmsg.mouseMotion.relative = vdi_stream_client__context_input_relative_mouse(parsec_context);
        pmsg.mouseMotion.x =
            pmsg.mouseMotion.relative ? (Sint32)msg->motion.xrel : (Sint32)msg->motion.x + 1;
        pmsg.mouseMotion.y =
            pmsg.mouseMotion.relative ? (Sint32)msg->motion.yrel : (Sint32)msg->motion.y + 1;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        input_context->mouse_buttons &= ~SDL_BUTTON_MASK(msg->button.button);
        vdi_stream_client__context_set_input_pressed(parsec_context, false);
        vdi_stream_client__input_queue_command(
            input_context, VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_BUTTON_UP,
            vdi_stream_client__context_input_grab_forced(parsec_context)
        );
        pmsg.type = MESSAGE_MOUSE_BUTTON;
        pmsg.mouseButton.button = msg->button.button;
        pmsg.mouseButton.pressed = false;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        input_context->mouse_buttons |= SDL_BUTTON_MASK(msg->button.button);
        vdi_stream_client__context_set_input_pressed(parsec_context, true);
        vdi_stream_client__input_queue_command(
            input_context, VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_BUTTON_DOWN,
            vdi_stream_client__context_input_grab_forced(parsec_context)
        );
        pmsg.type = MESSAGE_MOUSE_BUTTON;
        pmsg.mouseButton.button = msg->button.button;
        pmsg.mouseButton.pressed = true;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        pmsg.type = MESSAGE_MOUSE_WHEEL;
        pmsg.mouseWheel.x = msg->wheel.x * input_context->vdi_config->speed;
        pmsg.mouseWheel.y = msg->wheel.y * input_context->vdi_config->speed;
        break;
    case SDL_EVENT_CLIPBOARD_UPDATE:
        vdi_stream_client__input_queue_command(
            input_context, VDI_STREAM_CLIENT_INPUT_COMMAND_CLIPBOARD_UPDATE, false
        );
        break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        vdi_stream_client__input_queue_command(
            input_context, VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_ENTER, false
        );
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        vdi_stream_client__input_queue_command(
            input_context, VDI_STREAM_CLIENT_INPUT_COMMAND_MOUSE_LEAVE, false
        );
        break;
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESIZED:
        vdi_stream_client__input_queue_command(
            input_context, VDI_STREAM_CLIENT_INPUT_COMMAND_WINDOW_RESIZED, false
        );
        break;
    default:
        break;
    }

    vdi_stream_client__input_send_message(parsec_context, &pmsg);
}

Sint32
vdi_stream_client__input_thread(void *opaque)
{
    vdi_stream_client__input_context_s *input_context =
        (vdi_stream_client__input_context_s *)opaque;
    SDL_Event events[32];

    while (!vdi_stream_client__context_done(input_context->parsec_context)) {
        int count = SDL_PeepEvents(
            events, (int)(sizeof(events) / sizeof(events[0])), SDL_GETEVENT, SDL_EVENT_FIRST,
            SDL_EVENT_LAST
        );

        if (count <= 0) {
            SDL_Delay(1);
            continue;
        }

        for (int i = 0; i < count; i++) {
            vdi_stream_client__input_handle_event(input_context, &events[i]);
        }
    }

    return VDI_STREAM_CLIENT_SUCCESS;
}
