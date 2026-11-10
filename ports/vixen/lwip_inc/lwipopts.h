/* SPDX-License-Identifier: MIT */
#ifndef MICROPY_INCLUDED_VIXEN_LWIP_LWIPOPTS_H
#define MICROPY_INCLUDED_VIXEN_LWIP_LWIPOPTS_H

#include <stdint.h>

#define LWIP_NETIF_EXT_STATUS_CALLBACK  1

#define LWIP_IPV6                       0

#define LWIP_RAND() vixen_random_u32()

// lwIP's heap and pools are plain .bss in the chip-RAM window, so memory is
// not the limit. The hardware is: it holds only FOUR received frames, and
// while a TLS handshake does its bignum work nothing drains them. A receive
// window of four full segments means a peer can never have more in flight
// than the core can hold -- a larger one only turns into drops and resends.
#define MEM_SIZE                        (32 * 1024)
#define PBUF_POOL_SIZE                  (16)
#define TCP_MSS                         (1460)
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define MEMP_NUM_TCP_SEG                (32)

// Include common lwIP configuration.
#include "extmod/lwip-include/lwipopts_common.h"

uint32_t vixen_random_u32(void);

#endif // MICROPY_INCLUDED_VIXEN_LWIP_LWIPOPTS_H
