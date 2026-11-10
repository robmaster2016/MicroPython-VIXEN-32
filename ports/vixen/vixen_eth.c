/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026 Robert S. (aka Robmaster) */
/*
 * VIXEN-32 Ethernet for MicroPython: network.LAN on the core's own MAC.
 *
 * The core drives the MEGA65's KSZ8081 PHY over RMII and does the framing in
 * hardware (Chipset Reference, chapter 11): preamble, CRC, and a destination
 * filter that passes our MAC and broadcast (no multicast). Software sees one
 * RX window holding the oldest unread frame (FCS already stripped) and one TX
 * window. lwIP (extmod/modlwip.c) runs on top of it, which gives the `socket`
 * module, DHCP, DNS -- and with mbedtls `ssl`, so `mip` and `requests` work.
 *
 * The kernel's own network stack (SHIFT+POUND in BASIC) is NOT used: it is a
 * stop-and-wait client with two connections, and it only moves frames while
 * someone calls knet_poll(), which this port never does. The two stacks would
 * fight over the one RX window otherwise.
 *
 * THE PACKET BUFFERS TAKE 32-BIT ACCESSES ONLY. A byte store does not reach
 * them as one byte; whole words are assembled in a RAM bounce buffer and moved
 * with word loads/stores. Byte n of a frame is bits [8*(n%4)+7 : 8*(n%4)] of
 * word n/4 (little-endian, verified in rtl/mega65/vih_eth.sv).
 *
 * Nothing here runs from an interrupt. vixen_eth_poll() drains the receiver
 * and runs lwIP's timers; it is called from uart_poll() (the VM hook, input
 * waits, time.sleep()) and from modlwip's blocking loops
 * (MICROPY_PY_LWIP_POLL_HOOK). The hardware holds four frames, so a long C
 * computation (a TLS handshake's bignum work) can drop frames -- TCP resends.
 */

#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/modnetwork.h"

#if MICROPY_PY_LWIP

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#include "vih_regs.h"

#define ETH_REG(off)        (*(volatile uint32_t *)(0x3A000000u + (off)))
#define ETH_CTRL            ETH_REG(0x260)   /* W: [0] enable [1] promisc [3] RX late */
#define ETH_STATUS          ETH_REG(0x264)   /* R: [0] link [1] RX waiting [2] TX busy */
#define ETH_RXLEN           ETH_REG(0x268)
#define ETH_RXPOP           ETH_REG(0x26C)
#define ETH_TXLEN           ETH_REG(0x270)   /* the write starts the transmission */
#define ETH_MAC_LO          ETH_REG(0x274)
#define ETH_MAC_HI          ETH_REG(0x278)
#define ETH_DROPS           ETH_REG(0x27C)
#define ETH_CRCERR          ETH_REG(0x288)

#define ETH_CTRL_ENABLE     0x1u
#define ETH_CTRL_RX_LATE    0x8u
#define ETH_ST_LINK         0x1u
#define ETH_ST_RXREADY      0x2u
#define ETH_ST_TXBUSY       0x4u

#define ETH_RX_WIN          ((volatile uint32_t *)0x3A100000u)
#define ETH_TX_WIN          ((volatile uint32_t *)0x3A100800u)
#define ETH_FRAME_MAX       1536u
#define ETH_FRAME_MIN       60u      /* the core does not pad short frames */

/* ETH_CTRL bit 3 picks which RMII clock edge samples the PHY. Which one is
 * right depends on board delays, so it is found at run time: frames sampled
 * on the wrong edge all fail their CRC. Until the first good frame arrives,
 * every RX_LATE_ERRS CRC errors flip the edge. No time window: on a quiet
 * network the only frames may be DHCP replies, seconds apart. */
#define RX_LATE_ERRS        2u

#define LINK_POLL_MS        100u

typedef struct _vixen_eth_t {
    struct netif netif;
    struct dhcp dhcp_struct;
    bool active;
    bool rx_late;
    bool rx_late_locked;        /* a good frame arrived: the edge is right */
    uint32_t rx_late_crc0;
    uint32_t link_t;
    uint32_t rx_frames;
    uint32_t tx_frames;
} vixen_eth_t;

static vixen_eth_t vx_eth;

/* Bounce buffer for whole-word moves through the packet windows. */
static uint32_t eth_frame[ETH_FRAME_MAX / 4u];

/* modlwip brackets its lwIP calls with these; the poll stays out meanwhile. */
volatile uint32_t vixen_lwip_lock;

static bool lwip_started;

/******************************************************************************/
// lwIP glue

u32_t sys_now(void) {
    return mp_hal_ticks_ms();
}

static void eth_set_ctrl(void) {
    ETH_CTRL = ETH_CTRL_ENABLE | (vx_eth.rx_late ? ETH_CTRL_RX_LATE : 0u);
}

static err_t eth_netif_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    uint32_t len = p->tot_len;
    if (len > ETH_FRAME_MAX) {
        return ERR_BUF;
    }
    uint8_t *frame = (uint8_t *)eth_frame;
    pbuf_copy_partial(p, frame, len, 0);
    if (len < ETH_FRAME_MIN) {
        memset(frame + len, 0, ETH_FRAME_MIN - len);
        len = ETH_FRAME_MIN;
    }

    // The previous frame must be out of the TX window first. The busy flag
    // crosses from the 50 MHz side, so it is waited on after each start too
    // (below) -- otherwise the next frame could overwrite one still going out.
    for (uint32_t n = 0; (ETH_STATUS & ETH_ST_TXBUSY) && n < 100000u; ++n) {
    }

    for (uint32_t i = 0; i < (len + 3u) / 4u; ++i) {
        ETH_TX_WIN[i] = eth_frame[i];
    }
    ETH_TXLEN = len;
    for (uint32_t n = 0; !(ETH_STATUS & ETH_ST_TXBUSY) && n < 200u; ++n) {
    }
    vx_eth.tx_frames += 1;
    return ERR_OK;
}

static err_t eth_netif_init(struct netif *netif) {
    netif->linkoutput = eth_netif_output;
    netif->output = etharp_output;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    // No checksum offload: lwIP computes and checks everything in software.
    NETIF_SET_CHECKSUM_CTRL(netif, NETIF_CHECKSUM_ENABLE_ALL);
    return ERR_OK;
}

void vixen_entropy_add(uint32_t v);

static void eth_rx_one(void) {
    uint32_t len = ETH_RXLEN & 0x7FFu;
    if (len < 14u || len > ETH_FRAME_MAX) {
        ETH_RXPOP = 1;
        return;
    }
    for (uint32_t i = 0; i < (len + 3u) / 4u; ++i) {
        eth_frame[i] = ETH_RX_WIN[i];
    }
    ETH_RXPOP = 1;
    vx_eth.rx_frames += 1;
    /* arrival time and a few header/payload words feed os.urandom */
    vixen_entropy_add(len ^ eth_frame[3] ^ eth_frame[(len - 4u) / 4u]);
    vx_eth.rx_late_locked = true;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p == NULL) {
        return;
    }
    pbuf_take(p, eth_frame, len);
    if (vx_eth.netif.input(p, &vx_eth.netif) != ERR_OK) {
        pbuf_free(p);
    }
}

static void eth_rx_late_check(void) {
    if (vx_eth.rx_late_locked) {
        return;
    }
    uint32_t crc = ETH_CRCERR & 0xFFFFu;
    if (((crc - vx_eth.rx_late_crc0) & 0xFFFFu) >= RX_LATE_ERRS) {
        vx_eth.rx_late = !vx_eth.rx_late;
        eth_set_ctrl();
        vx_eth.rx_late_crc0 = crc;
    }
}

static void eth_link_check(uint32_t now) {
    if (now - vx_eth.link_t < LINK_POLL_MS) {
        return;
    }
    vx_eth.link_t = now;
    bool link = (ETH_STATUS & ETH_ST_LINK) != 0;
    if (link && !netif_is_link_up(&vx_eth.netif)) {
        // lwIP restarts DHCP itself: a fresh link may be another network.
        netif_set_link_up(&vx_eth.netif);
    } else if (!link && netif_is_link_up(&vx_eth.netif)) {
        netif_set_link_down(&vx_eth.netif);
    }
}

void vixen_eth_poll(void) {
    if (!vx_eth.active || vixen_lwip_lock != 0) {
        return;
    }
    vixen_lwip_lock += 1;
    // At most the four frames the hardware can hold, so a flood cannot keep
    // the caller (the console, the VM) from running.
    for (int i = 0; i < 4 && (ETH_STATUS & ETH_ST_RXREADY); ++i) {
        eth_rx_one();
    }
    uint32_t now = mp_hal_ticks_ms();
    eth_rx_late_check();
    eth_link_check(now);
    sys_check_timeouts();
    vixen_lwip_lock -= 1;
}

static void eth_read_mac(uint8_t *mac) {
    uint32_t lo = ETH_MAC_LO;
    uint32_t hi = ETH_MAC_HI;
    mac[0] = lo;
    mac[1] = lo >> 8;
    mac[2] = lo >> 16;
    mac[3] = lo >> 24;
    mac[4] = hi;
    mac[5] = hi >> 8;
}

static void eth_write_mac(const uint8_t *mac) {
    ETH_MAC_LO = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8)
        | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    ETH_MAC_HI = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
}

static void eth_start(void) {
    if (vx_eth.active) {
        return;
    }
    if (!lwip_started) {
        lwip_init();
        lwip_started = true;
    }

    // Whatever MAC the core holds: the reset value 02:56:49:58:45:4E, the one
    // the kernel set if BASIC had the network on, or one from config(mac=).
    vx_eth.netif.hwaddr_len = 6;
    eth_read_mac(vx_eth.netif.hwaddr);

    vx_eth.rx_late_locked = false;
    vx_eth.rx_late_crc0 = ETH_CRCERR & 0xFFFFu;
    eth_set_ctrl();
    // Anything left in the RX slots belongs to whoever ran before us.
    for (int i = 0; i < 4 && (ETH_STATUS & ETH_ST_RXREADY); ++i) {
        ETH_RXPOP = 1;
    }

    ip_addr_t ip, mask, gw, dns;
    IP4_ADDR(&ip, 0, 0, 0, 0);
    IP4_ADDR(&mask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);
    IP4_ADDR(&dns, 8, 8, 8, 8);

    vixen_lwip_lock += 1;
    vx_eth.netif.name[0] = 'e';
    vx_eth.netif.name[1] = '0';
    netif_add(&vx_eth.netif, &ip, &mask, &gw, &vx_eth, eth_netif_init, ethernet_input);
    netif_set_hostname(&vx_eth.netif, mod_network_hostname_data);
    netif_set_default(&vx_eth.netif);
    netif_set_up(&vx_eth.netif);
    dns_setserver(0, &dns);
    dhcp_set_struct(&vx_eth.netif, &vx_eth.dhcp_struct);
    dhcp_start(&vx_eth.netif);
    if (ETH_STATUS & ETH_ST_LINK) {
        netif_set_link_up(&vx_eth.netif);
    }
    vx_eth.link_t = mp_hal_ticks_ms();
    vixen_lwip_lock -= 1;

    vx_eth.active = true;
}

static void eth_stop(void) {
    if (!vx_eth.active) {
        return;
    }
    vixen_lwip_lock += 1;
    dhcp_stop(&vx_eth.netif);
    netif_set_down(&vx_eth.netif);
    netif_remove(&vx_eth.netif);
    vx_eth.netif.ip_addr.addr = 0;
    vx_eth.netif.flags = 0;
    vixen_lwip_lock -= 1;
    vx_eth.active = false;
    ETH_CTRL = 0;   // PHY back into reset: nothing on the wire
}

// 0 link down, 1 link up / not configured, 2 configuring, 3 got an address
// (the same codes as network.LAN on other ports).
static int eth_link_status(void) {
    struct netif *netif = &vx_eth.netif;
    if (vx_eth.active && (netif->flags & (NETIF_FLAG_UP | NETIF_FLAG_LINK_UP))
        == (NETIF_FLAG_UP | NETIF_FLAG_LINK_UP)) {
        return netif->ip_addr.addr != 0 ? 3 : 2;
    }
    return (ETH_STATUS & ETH_ST_LINK) ? 1 : 0;
}

/******************************************************************************/
// MicroPython bindings: network.LAN

typedef struct _network_lan_obj_t {
    mp_obj_base_t base;
} network_lan_obj_t;

extern const mp_obj_type_t network_lan_type;
static const network_lan_obj_t network_lan_eth0 = { { &network_lan_type } };

static void network_lan_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    uint32_t a = vx_eth.netif.ip_addr.addr;
    mp_printf(print, "<LAN status=%d ip=%u.%u.%u.%u>", eth_link_status(),
        a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
}

static mp_obj_t network_lan_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    // One interface; it survives a soft reset (lwIP keeps its address).
    mod_network_register_nic(MP_OBJ_FROM_PTR(&network_lan_eth0));
    return MP_OBJ_FROM_PTR(&network_lan_eth0);
}

static mp_obj_t network_lan_active(size_t n_args, const mp_obj_t *args) {
    if (n_args == 1) {
        return mp_obj_new_bool(vx_eth.active);
    }
    if (mp_obj_is_true(args[1])) {
        eth_start();
    } else {
        eth_stop();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_active_obj, 1, 2, network_lan_active);

static mp_obj_t network_lan_isconnected(mp_obj_t self_in) {
    return mp_obj_new_bool(eth_link_status() == 3);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_lan_isconnected_obj, network_lan_isconnected);

static mp_obj_t network_lan_ifconfig(size_t n_args, const mp_obj_t *args) {
    return mod_network_nic_ifconfig(&vx_eth.netif, n_args - 1, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_ifconfig_obj, 1, 2, network_lan_ifconfig);

static mp_obj_t network_lan_ipconfig(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    return mod_network_nic_ipconfig(&vx_eth.netif, n_args - 1, args + 1, kwargs);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_lan_ipconfig_obj, 1, network_lan_ipconfig);

static mp_obj_t network_lan_status(size_t n_args, const mp_obj_t *args) {
    if (n_args == 1) {
        return MP_OBJ_NEW_SMALL_INT(eth_link_status());
    }
    switch (mp_obj_str_get_qstr(args[1])) {
        case MP_QSTR_stats: {
            // (frames received, frames sent, hardware drops, CRC errors)
            mp_obj_t t[4] = {
                mp_obj_new_int_from_uint(vx_eth.rx_frames),
                mp_obj_new_int_from_uint(vx_eth.tx_frames),
                MP_OBJ_NEW_SMALL_INT(ETH_DROPS & 0xFFFFu),
                MP_OBJ_NEW_SMALL_INT(ETH_CRCERR & 0xFFFFu),
            };
            return mp_obj_new_tuple(4, t);
        }
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown status param"));
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_status_obj, 1, 2, network_lan_status);

static mp_obj_t network_lan_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (kwargs->used == 0) {
        if (n_args != 2) {
            mp_raise_TypeError(MP_ERROR_TEXT("must query one param"));
        }
        switch (mp_obj_str_get_qstr(args[1])) {
            case MP_QSTR_mac: {
                uint8_t mac[6];
                eth_read_mac(mac);
                return mp_obj_new_bytes(mac, 6);
            }
            case MP_QSTR_rx_late:
                return mp_obj_new_bool(vx_eth.rx_late);
            default:
                mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
        }
    }

    if (n_args != 1) {
        mp_raise_TypeError(MP_ERROR_TEXT("can't specify pos and kw args"));
    }
    for (size_t i = 0; i < kwargs->alloc; ++i) {
        if (!MP_MAP_SLOT_IS_FILLED(kwargs, i)) {
            continue;
        }
        mp_map_elem_t *e = &kwargs->table[i];
        switch (mp_obj_str_get_qstr(e->key)) {
            case MP_QSTR_mac: {
                mp_buffer_info_t buf;
                mp_get_buffer_raise(e->value, &buf, MP_BUFFER_READ);
                if (buf.len != 6) {
                    mp_raise_ValueError(MP_ERROR_TEXT("invalid buffer length"));
                }
                if (vx_eth.active) {
                    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("set mac before active(True)"));
                }
                eth_write_mac(buf.buf);
                break;
            }
            case MP_QSTR_rx_late:
                // Fix the RMII sampling edge by hand instead of auto-detecting.
                vx_eth.rx_late = mp_obj_is_true(e->value);
                vx_eth.rx_late_locked = true;
                if (vx_eth.active) {
                    eth_set_ctrl();
                }
                break;
            default:
                mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_lan_config_obj, 1, network_lan_config);

static const mp_rom_map_elem_t network_lan_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&network_lan_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&network_lan_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&network_lan_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipconfig), MP_ROM_PTR(&network_lan_ipconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&network_lan_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&network_lan_config_obj) },
};
static MP_DEFINE_CONST_DICT(network_lan_locals_dict, network_lan_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    network_lan_type,
    MP_QSTR_LAN,
    MP_TYPE_FLAG_NONE,
    make_new, network_lan_make_new,
    print, network_lan_print,
    locals_dict, &network_lan_locals_dict
    );

#endif // MICROPY_PY_LWIP
