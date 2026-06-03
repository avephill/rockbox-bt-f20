/*
 * btstack_config.h for Rockbox on EROS Q Native / Surfans F20.
 *
 * Minimum config for HCI + classic inquiry — no pairing, no A2DP yet.
 * Expand this as we enable profiles.
 */
#ifndef _BTSTACK_CONFIG_H
#define _BTSTACK_CONFIG_H

/* Classic BT support (required for inquiry / A2DP) */
#define ENABLE_CLASSIC

/* Drive A2DP codec selection from the app instead of btstack's built-in
 * auto-config. The default auto-config (a2dp.c, #ifndef this flag) hardcodes
 * SBC and ignores every other codec, so an advertised AAC endpoint would never
 * be negotiated. With explicit config on, bt-service.c's a2dp_packet_handler
 * collects per-codec capabilities and chooses AAC (preferred) or SBC (fallback)
 * at A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE. NOTE: this disables the
 * auto-SBC path GLOBALLY, so the app MUST also select SBC for SBC-only sinks. */
#define ENABLE_A2DP_EXPLICIT_CONFIG

/* H4 transport over UART is our only link */
#define HAVE_HCI_TRANSPORT_H4

/* Embedded time provider — we implement hal_time_ms() returning ms */
#define HAVE_EMBEDDED_TIME_MS

/* Disable features we're not using */
/* No BLE for now (enables SM + crypto otherwise) */
/* No logging to stdout / file */

/* Sizing: one active connection during bring-up is fine */
#define MAX_NR_HCI_CONNECTIONS              2
#define MAX_NR_L2CAP_SERVICES               2
#define MAX_NR_L2CAP_CHANNELS               4
#define MAX_NR_RFCOMM_MULTIPLEXERS          1
#define MAX_NR_RFCOMM_SERVICES              1
#define MAX_NR_RFCOMM_CHANNELS              2
#define MAX_NR_BNEP_SERVICES                1
#define MAX_NR_BNEP_CHANNELS                1
#define MAX_NR_WHITELIST_ENTRIES            1
#define MAX_NR_SERVICE_RECORD_ITEMS         4
#define MAX_NR_SM_LOOKUP_ENTRIES            1
#define MAX_NR_GATT_CLIENTS                 1
#define MAX_NR_GATT_SUBCLIENTS              1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS       2
#define MAX_NR_AVDTP_CONNECTIONS            1
#define MAX_NR_AVRCP_CONNECTIONS            2
#define MAX_NR_LE_DEVICE_DB_ENTRIES         0
#define MAX_NR_LE_AUDIO_CIGS                0
#define MAX_NR_LE_AUDIO_CISES               0
#define MAX_NR_BASS_CLIENTS                 0
#define MAX_NR_BASS_SOURCES                 0
#define MAX_NR_BASS_SERVERS                 0
#define MAX_NR_BASS_SERVER_SOURCES          0

/* HCI buffer sizes */
#define HCI_ACL_PAYLOAD_SIZE                (1024 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE        6

/* Persistent link keys via TLV (see bt-tlv.c → /.rockbox/bt_keys.dat). */
#define NVM_NUM_DEVICE_DB_ENTRIES           8
#define NVM_NUM_LINK_KEYS                   8

#endif /* _BTSTACK_CONFIG_H */
