/* SPDX-License-Identifier: Apache-2.0
 * Boot and connection phase of the knob, derived from espOS: Wi-Fi state machine, SignalK token state and stream.
 * Polled, no event handlers, so it cannot get in the way of espOS. The UI maps the phase to a screen and a status text. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KNOB_NET_STARTING = 0, /* espOS not up yet */
    KNOB_NET_PORTAL,       /* no Wi-Fi configured: the setup access point is open */
    KNOB_NET_WIFI,         /* joining Wi-Fi (associating, DHCP, or waiting to retry) */
    KNOB_NET_SERVER,       /* Wi-Fi is up, looking for a SignalK server */
    KNOB_NET_PAIRING,      /* access request sent, waiting for approval in the SignalK admin UI */
    KNOB_NET_TOKEN,        /* have a token, verifying it */
    KNOB_NET_LINK,         /* token works, opening the delta stream */
    KNOB_NET_FAILED,       /* access denied, certificate problem or server error: needs attention or a retry */
    KNOB_NET_ONLINE,       /* delta stream connected */
} knob_net_phase_t;

typedef struct {
    knob_net_phase_t phase;
    char text[16];         /* short upper case name of the phase, for the splash status bar */
    uint8_t progress;      /* 0..100, for the splash progress bar */
    char portal_ssid[33];  /* setup access point name while phase is KNOB_NET_PORTAL */
    char hostname[33];     /* what the knob is called on the network */
    char token_state[16];  /* espOS token state string, for logs */
} knob_net_status_t;

/* Starts the polling task. Call after espos_start(). */
void knob_net_start(void);
/* Thread-safe copy of the latest status. Before knob_net_start() it reports KNOB_NET_STARTING. */
void knob_net_get(knob_net_status_t *out);
