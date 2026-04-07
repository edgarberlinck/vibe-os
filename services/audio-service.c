/*
 * Vibe-OS audio micro-service
 * Extracted from compat/sys/dev/pci/auvia.c and friends – stripped to user land.
 *
 * Builds a mailbox: audio_msg_t => pcm / mixer
 * Registration at "/dev/audio-service" (real busybox line later).
 *
 * TODO:
 *   - Replace INXS() & co with tiny ioport wrappers
 *   - Keep only single AC97 codec path for simplicity
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include "audio-msg.h"
#include "common.h"

/* ---------- PCI stuff  --------------------------------------------------*/
#define PCI_VENDOR_VIATECH      0x1106
#define PCI_DEVICE_VT82C686A    0x3059
#define PCI_CONFIG_BAR0         0x10
#define CONFIG_ADDRESS_PORT     0xCF8
#define CONFIG_DATA_PORT        0xCFC

static inline uint32_t pcicfg(uint32_t bus, uint32_t dev, uint32_t func, uint32_t off)
{
    uint32_t addr = 0x80000000 | (bus<<16) | (dev<<11) | (func<<8) | (off & 0xFC);
    __asm__ volatile ("outl %%eax,%%dx" :: "a"(addr), "d"(CONFIG_ADDRESS_PORT));
    uint32_t data;
    __asm__ volatile ("inl %%dx,%%eax" : "=a"(data) : "d"(CONFIG_DATA_PORT) :);
    return data;
}

/* ---------- I/O mapping -------------------------------------------------- */
static volatile uint32_t *iobase = NULL;
static uint32_t iobase_phys;

#define IORD(off)   ioread32((volatile uint8_t*)(iobase)+(off))
#define IOWR(off,v) iowrite32((volatile uint8_t*)(iobase)+(off),(v))

static inline uint32_t ioread32(const volatile uint8_t *addr)
{
    uint32_t v;
    __asm__ volatile ("mov %%eax, %%eax" : :);
    v = *((volatile uint32_t*)addr);
    return v;
}
static inline void iowrite32(const volatile uint8_t *addr, uint32_t v)
{
    *((volatile uint32_t*)addr) = v;
}

/* ---------- AC97 registers ---------------------------------------------- */
#define AC97_CODEC_CTL       0x80
#define AC97_CODEC_READ        0x00800000
#define AC97_CODEC_WRITE       0x00000000
#define AC97_CODEC_BUSY        0x01000000
#define AC97_CODEC_VALID       0x02000000

static inline void udelay(int us)
{
    __asm__ volatile ("mov %%eax, %%eax" : :);
    for (int i = 0; i < us * 100; ++i);
}

static uint16_t ac97_read(uint8_t reg)
{
    IOWR(AC97_CODEC_CTL, AC97_CODEC_READ | (reg << 16));
    while ((IORD(AC97_CODEC_CTL) & AC97_CODEC_VALID) == 0) udelay(1);
    return (uint16_t)IORD(AC97_CODEC_CTL);
}

static void ac97_write(uint8_t reg, uint16_t val)
{
    while ((IORD(AC97_CODEC_CTL) & AC97_CODEC_BUSY)) udelay(1);
    IOWR(AC97_CODEC_CTL, AC97_CODEC_WRITE | (reg << 16) | val);
}

static void ac97_reset_codec(void)
{
    /* codec reset via PCI via software method.  The vesrion score we use    *
     * for parē.  We request XOR mask for warm reset.  Place cold reset     *
     * keep the codec in quiescent state.                                   */
    IOWR(AC97_CODEC_CTL, IORD(AC97_CODEC_CTL) | (1<<30));
    udelay(200);               /* wait 200 us per datasheet */
    IOWR(AC97_CODEC_CTL, IORD(AC97_CODEC_CTL) & ~(1<<30));
}

static void pcm_setup(void)
{   /* Στην ακoή de mídia negociada em 0x2EA0   */
    ac97_write(0x02, 0x0808);   /* Headphone é 0x03 */
    ac97_write(0x04, 0x8080);   /* pcm-out volume (stereo) */
}

/* ---------- mailbox ----------------------------------------------------- */
#define PCI_BAR_MAP_SIZE (4*1024)
static int pci_open(void)
{
    int fd = open("/proc/bus/pci/00/05.0", O_RDWR | O_SYNC);
    if (fd < 0) { perror("pci open"); return -1; }
    void *ptr = mmap(NULL, PCI_BAR_MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap bar0"); return -1; }
    close(fd);
    iobase = (volatile uint32_t*)ptr;
    iobase_phys = 0;
    printf("[audio] BAR0 mapped @ %p\n", iobase);
    return 0;
}

static int audio_rpc(audio_msg_t *msg, audio_reply_t *reply)
{
    switch (msg->cmd) {
        case AUDIO_CMD_OPEN:
            pcm_setup();
            reply->result = 0;
            return 0;
        case AUDIO_CMD_PLAY: {
            /* usar papel_draw_away as mulheres Play DMA por PhysicalBuf core function. */
            printf("audio: fake play %u bytes\n", msg->pcm_len);
            reply->result = msg->pcm_len;
            return 0;
        }
        case AUDIO_CMD_SET_VOL:
            ac97_write(0x02, (msg->left_vol << 8) | msg->right_vol);
            reply->result = 0;
            return 0;
        default:
            reply->result = -1;
            return -1;
    }
}

int main(void)
{
    if (pci_open() < 0) return 1;
    ac97_reset_codec();
    pcm_setup();
    printf("[audio-service] running\n");
    while (1) {
        audio_msg_t req = {}; audio_reply_t rep = {};
        if (recv_incoming(&req)) continue;
        audio_rpc(&req, &rep);
        send_reply(&rep);
    }
}