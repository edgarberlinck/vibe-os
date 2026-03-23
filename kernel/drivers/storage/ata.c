#include <kernel/drivers/storage/ata.h>
#include <kernel/drivers/storage/block_device.h>
#include <kernel/hal/io.h>
#include <kernel/kernel_string.h>

#define ATA_PRIMARY_IO 0x1F0u
#define ATA_PRIMARY_CTRL 0x3F6u

#define ATA_REG_DATA (ATA_PRIMARY_IO + 0u)
#define ATA_REG_FEATURES (ATA_PRIMARY_IO + 1u)
#define ATA_REG_SECCOUNT0 (ATA_PRIMARY_IO + 2u)
#define ATA_REG_LBA0 (ATA_PRIMARY_IO + 3u)
#define ATA_REG_LBA1 (ATA_PRIMARY_IO + 4u)
#define ATA_REG_LBA2 (ATA_PRIMARY_IO + 5u)
#define ATA_REG_HDDEVSEL (ATA_PRIMARY_IO + 6u)
#define ATA_REG_COMMAND (ATA_PRIMARY_IO + 7u)
#define ATA_REG_STATUS (ATA_PRIMARY_IO + 7u)

#define ATA_CMD_READ_PIO 0x20u
#define ATA_CMD_WRITE_PIO 0x30u
#define ATA_CMD_CACHE_FLUSH 0xE7u
#define ATA_CMD_IDENTIFY 0xECu

#define ATA_SR_ERR 0x01u
#define ATA_SR_DRQ 0x08u
#define ATA_SR_DF 0x20u
#define ATA_SR_DRDY 0x40u
#define ATA_SR_BSY 0x80u

#define ATA_TIMEOUT 100000u
#define ATA_MBR_PARTITION_OFFSET 446u
#define ATA_MBR_SIGNATURE_OFFSET 510u
#define ATA_STORAGE_PARTITION_INDEX 1u

static int g_ata_ready = 0;
static uint32_t g_ata_total_sectors = 0u;
static uint32_t g_storage_partition_start_lba = 0u;
static uint32_t g_storage_partition_sector_count = 0u;

static int ata_read_sector(uint32_t lba, uint8_t *buf);
static int ata_write_sector(uint32_t lba, const uint8_t *buf);
static int ata_block_read(void *context, uint32_t lba, uint8_t *buf);
static int ata_block_write(void *context, uint32_t lba, const uint8_t *buf);

static uint32_t ata_read_u32_le(const uint8_t *src) {
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status == 0xFFu) {
            return -1;
        }
        if ((status & ATA_SR_BSY) == 0u) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_data_ready(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status == 0xFFu) {
            return -1;
        }
        if ((status & ATA_SR_ERR) != 0u || (status & ATA_SR_DF) != 0u) {
            return -1;
        }
        if ((status & ATA_SR_BSY) == 0u &&
            (status & ATA_SR_DRQ) != 0u &&
            (status & ATA_SR_DRDY) != 0u) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_command_done(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; ++i) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status == 0xFFu) {
            return -1;
        }
        if ((status & ATA_SR_ERR) != 0u || (status & ATA_SR_DF) != 0u) {
            return -1;
        }
        if ((status & ATA_SR_BSY) == 0u) {
            return 0;
        }
    }
    return -1;
}

static void ata_select_lba(uint32_t lba) {
    outb(ATA_REG_HDDEVSEL, (uint8_t)(0xE0u | ((lba >> 24) & 0x0Fu)));
    io_wait();
}

static int ata_identify(void) {
    uint16_t identify_data[256];

    ata_select_lba(0u);
    outb(ATA_PRIMARY_CTRL, 0u);
    outb(ATA_REG_SECCOUNT0, 0u);
    outb(ATA_REG_LBA0, 0u);
    outb(ATA_REG_LBA1, 0u);
    outb(ATA_REG_LBA2, 0u);
    outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(ATA_REG_STATUS) == 0u) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    if (inb(ATA_REG_LBA1) != 0u || inb(ATA_REG_LBA2) != 0u) {
        return -1;
    }
    if (ata_wait_data_ready() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; ++i) {
        identify_data[i] = inw(ATA_REG_DATA);
    }
    g_ata_total_sectors = ((uint32_t)identify_data[61] << 16) | (uint32_t)identify_data[60];
    if (g_ata_total_sectors == 0u) {
        return -1;
    }
    return 0;
}

static void ata_detect_storage_partition(void) {
    uint8_t mbr[KERNEL_PERSIST_SECTOR_SIZE];
    const uint8_t *entry;
    uint32_t start_lba;
    uint32_t sector_count;

    g_storage_partition_start_lba = 0u;
    g_storage_partition_sector_count = g_ata_total_sectors;

    if (ata_read_sector(0u, mbr) != 0) {
        return;
    }
    if (mbr[ATA_MBR_SIGNATURE_OFFSET] != 0x55u ||
        mbr[ATA_MBR_SIGNATURE_OFFSET + 1] != 0xAAu) {
        return;
    }

    entry = &mbr[ATA_MBR_PARTITION_OFFSET + (ATA_STORAGE_PARTITION_INDEX * 16u)];
    start_lba = ata_read_u32_le(entry + 8);
    sector_count = ata_read_u32_le(entry + 12);
    if (start_lba == 0u || sector_count == 0u) {
        return;
    }
    if (start_lba >= g_ata_total_sectors) {
        return;
    }
    if (sector_count > (g_ata_total_sectors - start_lba)) {
        sector_count = g_ata_total_sectors - start_lba;
    }

    g_storage_partition_start_lba = start_lba;
    g_storage_partition_sector_count = sector_count;
}

static int ata_read_logical_sector(uint32_t lba, uint8_t *buf) {
    if (lba >= g_storage_partition_sector_count) {
        return -1;
    }
    return ata_read_sector(g_storage_partition_start_lba + lba, buf);
}

static int ata_write_logical_sector(uint32_t lba, const uint8_t *buf) {
    if (lba >= g_storage_partition_sector_count) {
        return -1;
    }
    return ata_write_sector(g_storage_partition_start_lba + lba, buf);
}

static int ata_read_sector(uint32_t lba, uint8_t *buf) {
    if (buf == 0 || lba >= 0x10000000u) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    ata_select_lba(lba);
    outb(ATA_REG_FEATURES, 0u);
    outb(ATA_REG_SECCOUNT0, 1u);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFFu));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFFu));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFFu));
    outb(ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (ata_wait_data_ready() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; ++i) {
        uint16_t value = inw(ATA_REG_DATA);
        buf[(i * 2) + 0] = (uint8_t)(value & 0xFFu);
        buf[(i * 2) + 1] = (uint8_t)((value >> 8) & 0xFFu);
    }
    return 0;
}

static int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    if (buf == 0 || lba >= 0x10000000u) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    ata_select_lba(lba);
    outb(ATA_REG_FEATURES, 0u);
    outb(ATA_REG_SECCOUNT0, 1u);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFFu));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFFu));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFFu));
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    if (ata_wait_data_ready() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; ++i) {
        uint16_t value = (uint16_t)buf[(i * 2) + 0] |
                         ((uint16_t)buf[(i * 2) + 1] << 8);
        outw(ATA_REG_DATA, value);
    }

    outb(ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_command_done();
}

void kernel_storage_init(void) {
    kernel_block_device_reset();
    g_ata_ready = ata_identify() == 0;
    if (!g_ata_ready) {
        return;
    }
    ata_detect_storage_partition();
    if (kernel_block_device_register_primary("ata",
                                             0,
                                             g_storage_partition_sector_count,
                                             g_storage_partition_start_lba,
                                             ata_block_read,
                                             ata_block_write) != 0) {
        g_ata_ready = 0;
    }
}

static int ata_block_read(void *context, uint32_t lba, uint8_t *buf) {
    (void)context;
    if (!g_ata_ready) {
        return -1;
    }
    return ata_read_logical_sector(lba, buf);
}

static int ata_block_write(void *context, uint32_t lba, const uint8_t *buf) {
    (void)context;
    if (!g_ata_ready) {
        return -1;
    }
    return ata_write_logical_sector(lba, buf);
}
