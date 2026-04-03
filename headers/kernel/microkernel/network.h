#ifndef KERNEL_MICROKERNEL_NETWORK_H
#define KERNEL_MICROKERNEL_NETWORK_H

#include <stdint.h>
#include <sys/socket.h>

struct mk_message;

enum mk_network_capability_flags {
    MK_NETWORK_CAPS_QUERY_ONLY = 1u << 0,
    MK_NETWORK_CAPS_LOOPBACK_READY = 1u << 1,
    MK_NETWORK_CAPS_BSD_SOCKET_ABI = 1u << 2,
    MK_NETWORK_CAPS_DRIVER_EXTRACTION_PENDING = 1u << 3,
    MK_NETWORK_CAPS_L2_DEVICE_PRESENT = 1u << 4,
    MK_NETWORK_CAPS_WIFI_PRESENT = 1u << 5,
    MK_NETWORK_CAPS_WIFI_SCAN_READY = 1u << 6
};

enum mk_network_family_bits {
    MK_NETWORK_FAMILY_UNIX = 1u << 0,
    MK_NETWORK_FAMILY_INET = 1u << 1,
    MK_NETWORK_FAMILY_INET6 = 1u << 2
};

enum mk_network_socket_type_bits {
    MK_NETWORK_SOCKET_STREAM = 1u << 0,
    MK_NETWORK_SOCKET_DGRAM = 1u << 1,
    MK_NETWORK_SOCKET_RAW = 1u << 2
};

enum mk_network_limits {
    MK_NETWORK_WIFI_SCAN_MAX_APS = 6u
};

struct mk_network_socket_request {
    uint32_t domain;
    uint32_t type;
    uint32_t protocol;
};

struct mk_network_name_request {
    int32_t handle;
    uint32_t address_length;
    uint32_t transfer_id;
};

struct mk_network_io_request {
    int32_t handle;
    int32_t flags;
    uint32_t size;
    uint32_t transfer_id;
    uint32_t address_length;
    uint32_t address_transfer_id;
};

struct mk_network_option_request {
    int32_t handle;
    int32_t level;
    int32_t option_name;
    uint32_t value_size;
    uint32_t transfer_id;
};

struct mk_network_result {
    int32_t value;
};

struct mk_network_info {
    uint32_t flags;
    uint32_t supported_families;
    uint32_t supported_socket_types;
    uint32_t max_sockets;
    uint32_t max_packet_size;
    uint16_t wifi_vendor_id;
    uint16_t wifi_device_id;
    char wifi_chip_name[48];
};

struct mk_network_wifi_ap {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t encrypted;
    uint8_t reserved0;
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t reserved3;
};

struct mk_network_wifi_scan_result {
    uint32_t count;
    struct mk_network_wifi_ap aps[MK_NETWORK_WIFI_SCAN_MAX_APS];
};

void mk_network_service_init(void);
int mk_network_service_ready(void);
int mk_network_service_get_info(struct mk_network_info *info);
int mk_network_service_wifi_scan(struct mk_network_wifi_scan_result *result);
int mk_network_service_last_request(struct mk_message *message);

#endif
