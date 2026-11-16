/*
 * meshtastic.h — Constantes y utilidades de Meshtastic compartidas por el
 * decodificador y el modulador.
 *
 * Capa de radio (https://meshtastic.org/docs/overview/radio-settings/):
 *   presets de módem (SF / BW / CR) y, en la UE_868, LongFast en 869,525 MHz;
 *   preámbulo de 16 símbolos (RadioInterface.h: preambleLengthDefault = 16);
 *   palabra de sincronía 0x2B (valor conocido del proyecto; la documentación
 *   consultada no la lista).
 * Cabecera de 16 bytes en claro (RadioInterface.h, PacketHeader):
 *   to(4 LE) from(4 LE) id(4 LE) flags(1) channel_hash(1) next_hop(1) relay(1)
 *   flags: bits 0..2 hop_limit, bit 3 want_ack, bit 4 via_mqtt, bits 5..7 hop_start.
 * Cifrado (CryptoEngine.cpp): AES-CTR, nonce de 16 bytes =
 *   id(8 bytes LE, u64) | from(4 LE) | 0000 y contador gran-endian en los 4
 *   últimos bytes (CTR::setCounterSize(4)).
 * Clave: el PSK "AQ==" (un byte 0x01) significa "clave por defecto con índice
 * 1": se copia defaultpsk (16 bytes) y se suma (índice−1) al último byte
 * (Channels.cpp). El hash de canal (1 byte) es XOR(nombre) ^ XOR(clave); para
 * "LongFast" con la clave por defecto vale 8 (comprobado en tests/test_aes.c).
 *
 * AGENTS.md §0.12: solo se descifra con esta clave pública por defecto.
 */
#ifndef BLADERF_LAB_MESHTASTIC_H
#define BLADERF_LAB_MESHTASTIC_H

#include <stddef.h>
#include <stdint.h>

extern const uint8_t MESH_DEFAULT_KEY[16];

typedef struct {
    const char *name;
    int sf, bw_hz, cr; /* cr 1 = 4/5 … 4 = 4/8 */
} mesh_preset_t;

const mesh_preset_t *mesh_preset_find(const char *name);
uint8_t mesh_channel_hash(const char *name, const uint8_t *key, int keylen);
/* Cifra/descifra (CTR es simétrico) in situ con AES-128. */
void mesh_crypt(const uint8_t *key, uint32_t from, uint32_t id, uint8_t *data, int n);

#define MESH_HDR_LEN 16
#define MESH_PORT_TEXT 1
#define MESH_PORT_POSITION 3
#define MESH_PORT_NODEINFO 4
#define MESH_PORT_TELEMETRY 67

#endif
