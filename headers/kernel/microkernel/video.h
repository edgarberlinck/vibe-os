#ifndef KERNEL_MICROKERNEL_VIDEO_H
#define KERNEL_MICROKERNEL_VIDEO_H

#include <include/userland_api.h>
#include <stdint.h>

struct process;

#define MK_VIDEO_INLINE_TEXT_MAX 240u
#define MK_VIDEO_INLINE_BLIT8_MAX 224u
#define MK_VIDEO_PALETTE_BYTES 768u

struct mk_video_color_request {
    uint32_t color;
};

struct mk_video_rect_request {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t color;
};

struct mk_video_text_request {
    int32_t x;
    int32_t y;
    uint32_t color;
    uint32_t text_length;
    uint32_t transfer_id;
};

struct mk_video_text_inline_request {
    int32_t x;
    int32_t y;
    uint32_t color;
    uint32_t text_length;
    char text[MK_VIDEO_INLINE_TEXT_MAX];
};

struct mk_video_blit8_request {
    int32_t src_width;
    int32_t src_height;
    int32_t dst_x;
    int32_t dst_y;
    int32_t scale;
    uint32_t byte_count;
    uint32_t transfer_id;
};

struct mk_video_blit8_inline_request {
    int32_t src_width;
    int32_t src_height;
    int32_t dst_x;
    int32_t dst_y;
    int32_t scale;
    uint32_t byte_count;
    uint8_t pixels[MK_VIDEO_INLINE_BLIT8_MAX];
};

struct mk_video_blit8_stretch_present_request {
    int32_t src_width;
    int32_t src_height;
    int32_t dst_x;
    int32_t dst_y;
    int32_t dst_width;
    int32_t dst_height;
    uint32_t byte_count;
    uint32_t transfer_id;
};

struct mk_video_blit8_stretch_request {
    int32_t src_width;
    int32_t src_height;
    int32_t dst_x;
    int32_t dst_y;
    int32_t dst_width;
    int32_t dst_height;
    uint32_t byte_count;
    uint32_t transfer_id;
};

struct mk_video_blit8_stretch_inline_request {
    int32_t src_width;
    int32_t src_height;
    int32_t dst_x;
    int32_t dst_y;
    int32_t dst_width;
    int32_t dst_height;
    uint32_t byte_count;
    uint8_t pixels[MK_VIDEO_INLINE_BLIT8_MAX];
};

struct mk_video_blit8_stretch_present_inline_request {
    int32_t src_width;
    int32_t src_height;
    int32_t dst_x;
    int32_t dst_y;
    int32_t dst_width;
    int32_t dst_height;
    uint32_t byte_count;
    uint8_t pixels[MK_VIDEO_INLINE_BLIT8_MAX];
};

struct mk_video_mode_request {
    uint32_t width;
    uint32_t height;
};

struct mk_video_palette_request {
    uint32_t transfer_id;
    uint32_t byte_count;
};

struct mk_video_present_request {
    uint32_t mode;
};

struct mk_video_u32_request {
    uint32_t value;
};

struct mk_video_result {
    int32_t value;
};

struct mk_video_present_reply {
    int32_t value;
    uint32_t sequence;
};

struct mk_video_mode_reply {
    int32_t value;
    struct video_mode mode;
};

struct mk_video_caps_reply {
    int32_t value;
    struct video_capabilities caps;
};

void mk_video_service_init(void);
int mk_video_service_clear(uint8_t color);
int mk_video_service_rect(int x, int y, int w, int h, uint8_t color);
int mk_video_service_text(int x, int y, uint8_t color, const char *text);
int mk_video_service_flip(void);
int mk_video_service_flip_mode(uint32_t mode);
int mk_video_service_present_submit(uint32_t mode, uint32_t *sequence_out);
int mk_video_service_set_present_policy(uint32_t policy);
int mk_video_service_set_present_copy_override(uint32_t kind);
int mk_video_service_leave_graphics(void);
int mk_video_service_set_mode(uint32_t width, uint32_t height);
int mk_video_service_set_palette(const uint8_t *rgb_triplets);
int mk_video_service_get_palette(uint8_t *rgb_triplets);
int mk_video_service_blit8(const uint8_t *src, int src_w, int src_h, int dst_x, int dst_y, int scale);
int mk_video_service_blit8_transfer(uint32_t transfer_id, uint32_t byte_count,
                                    int src_w, int src_h, int dst_x, int dst_y, int scale);
int mk_video_service_blit8_present(const uint8_t *src, int src_w, int src_h,
                                   int dst_x, int dst_y, int scale);
int mk_video_service_blit8_present_transfer(uint32_t transfer_id, uint32_t byte_count,
                                            int src_w, int src_h, int dst_x, int dst_y, int scale);
int mk_video_service_blit8_stretch(const uint8_t *src, int src_w, int src_h,
                                   int dst_x, int dst_y, int dst_w, int dst_h);
int mk_video_service_blit8_stretch_transfer(uint32_t transfer_id, uint32_t byte_count,
                                            int src_w, int src_h,
                                            int dst_x, int dst_y, int dst_w, int dst_h);
int mk_video_service_blit8_stretch_present(const uint8_t *src, int src_w, int src_h,
                                           int dst_x, int dst_y, int dst_w, int dst_h);
int mk_video_service_blit8_stretch_present_transfer(uint32_t transfer_id, uint32_t byte_count,
                                                    int src_w, int src_h,
                                                    int dst_x, int dst_y, int dst_w, int dst_h);
int mk_video_service_get_info(struct video_mode *mode);
int mk_video_service_get_caps(struct video_capabilities *caps);
int mk_video_service_subscribe(struct process *subscriber);
int mk_video_service_event_receive(struct process *subscriber,
                                   struct mk_video_event *event,
                                   uint32_t timeout_ticks);

#endif
