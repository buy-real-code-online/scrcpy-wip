#include "keyboard_uhid.h"

#include "util/log.h"

/** Downcast key processor to keyboard_uhid */
#define DOWNCAST(KP) container_of(KP, struct sc_keyboard_uhid, key_processor)

/** Downcast uhid_receiver to keyboard_uhid */
#define DOWNCAST_RECEIVER(UR) \
    container_of(UR, struct sc_keyboard_uhid, uhid_receiver)

static void
sc_keyboard_uhid_send_input(struct sc_keyboard_uhid *kb,
                            const struct sc_hid_event *event) {
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_UHID_INPUT;
    msg.uhid_input.id = event->hid_id;

    assert(event->size <= SC_HID_MAX_SIZE);
    memcpy(msg.uhid_input.data, event->data, event->size);
    msg.uhid_input.size = event->size;

    if (!sc_controller_push_msg(kb->controller, &msg)) {
        LOGE("Could not send UHID_INPUT message (key)");
    }
}

static void
sc_keyboard_uhid_synchronize_mod(struct sc_keyboard_uhid *kb) {
    SDL_Keymod sdl_mod = SDL_GetModState();
    uint16_t mod = sc_mods_state_from_sdl(sdl_mod) & (SC_MOD_CAPS | SC_MOD_NUM);
    uint16_t diff = mod ^ kb->device_mod;

    if (diff) {
        // Inherently racy (the HID output reports arrive asynchronously in
        // response to key presses), but will re-synchronize on next key press
        // or HID output anyway
        kb->device_mod = mod;

        struct sc_hid_event hid_event;
        sc_hid_keyboard_event_from_mods(&hid_event, diff);

        LOGV("HID keyboard state synchronized");

        sc_keyboard_uhid_send_input(kb, &hid_event);
    }
}

static void
sc_key_processor_process_key(struct sc_key_processor *kp,
                             const struct sc_key_event *event,
                             uint64_t ack_to_wait) {
    (void) ack_to_wait;

    assert(sc_thread_get_id() == SC_MAIN_THREAD_ID);

    if (event->repeat) {
        // In USB HID protocol, key repeat is handled by the host (Android), so
        // just ignore key repeat here.
        return;
    }

    struct sc_keyboard_uhid *kb = DOWNCAST(kp);

    struct sc_hid_event hid_event;

    // Not all keys are supported, just ignore unsupported keys
    if (sc_hid_keyboard_event_from_key(&kb->hid, &hid_event, event)) {
        if (event->scancode == SC_SCANCODE_CAPSLOCK) {
            kb->device_mod ^= SC_MOD_CAPS;
        } else if (event->scancode == SC_SCANCODE_NUMLOCK) {
            kb->device_mod ^= SC_MOD_NUM;
        } else {
            // Synchronize modifiers (only if the scancode itself does not
            // change the modifiers)
            sc_keyboard_uhid_synchronize_mod(kb);
        }
        sc_keyboard_uhid_send_input(kb, &hid_event);
    }
}

static unsigned
sc_keyboard_uhid_to_sc_mod(uint8_t hid_led) {
    // <https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf>
    // (chapter 11: LED page)
    unsigned mod = 0;
    if (hid_led & 0x01) {
        mod |= SC_MOD_NUM;
    }
    if (hid_led & 0x02) {
        mod |= SC_MOD_CAPS;
    }
    return mod;
}

static void
sc_uhid_receiver_process_output(struct sc_uhid_receiver *receiver,
                                const uint8_t *data, size_t len) {
    assert(sc_thread_get_id() == SC_MAIN_THREAD_ID);

    assert(len);

    // Also check at runtime (do not trust the server)
    if (!len) {
        LOGE("Unexpected empty HID output message");
        return;
    }

    struct sc_keyboard_uhid *kb = DOWNCAST_RECEIVER(receiver);

    uint8_t hid_led = data[0];
    uint16_t device_mod = sc_keyboard_uhid_to_sc_mod(hid_led);
    kb->device_mod = device_mod;
}

bool
sc_keyboard_uhid_init(struct sc_keyboard_uhid *kb,
                      struct sc_controller *controller,
                      struct sc_uhid_devices *uhid_devices) {
    sc_hid_keyboard_init(&kb->hid);

    kb->controller = controller;
    kb->device_mod = 0;

    static const struct sc_key_processor_ops ops = {
        .process_key = sc_key_processor_process_key,
        // Never forward text input via HID (all the keys are injected
        // separately)
        .process_text = NULL,
    };

    // Clipboard synchronization is requested over the same control socket, so
    // there is no need for a specific synchronization mechanism
    kb->key_processor.async_paste = false;
    kb->key_processor.hid = true;
    kb->key_processor.ops = &ops;

    static const struct sc_uhid_receiver_ops uhid_receiver_ops = {
        .process_output = sc_uhid_receiver_process_output,
    };

    kb->uhid_receiver.id = SC_HID_ID_KEYBOARD;
    kb->uhid_receiver.ops = &uhid_receiver_ops;
    sc_uhid_devices_add_receiver(uhid_devices, &kb->uhid_receiver);

    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_UHID_CREATE;
    msg.uhid_create.id = SC_HID_ID_KEYBOARD;
    msg.uhid_create.report_desc = SC_HID_KEYBOARD_REPORT_DESC;
    msg.uhid_create.report_desc_size = SC_HID_KEYBOARD_REPORT_DESC_LEN;
    if (!sc_controller_push_msg(controller, &msg)) {
        LOGE("Could not send UHID_CREATE message (keyboard)");
        return false;
    }

    return true;
}
