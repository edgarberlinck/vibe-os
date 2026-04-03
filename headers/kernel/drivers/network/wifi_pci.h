#ifndef KERNEL_DRIVERS_NETWORK_WIFI_PCI_H
#define KERNEL_DRIVERS_NETWORK_WIFI_PCI_H

#include <stdint.h>

void kernel_wifi_pci_init(void);
int kernel_wifi_pci_present(void);
uint16_t kernel_wifi_pci_vendor_id(void);
uint16_t kernel_wifi_pci_device_id(void);
const char *kernel_wifi_pci_chip_name(void);

#endif