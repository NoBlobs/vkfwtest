/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/*
 * vkfwtest: headless Vulkan workloads that exercise GPU firmware paths and
 * check every result on the CPU. Written from the public Vulkan API only.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

/* ------------------------------------------------------------ results */
enum result {
	R_PASS = 0,
	R_MISMATCH = 1,         /* output differs from the expected values */
	R_TIMEOUT = 2,          /* a fence did not signal in time */
	R_LOST = 3,             /* VK_ERROR_DEVICE_LOST */
	R_SETUP = 4,            /* Vulkan setup error or bad option */
	R_SKIP = 77,            /* not supported on this device */
};

const char *result_name(enum result r);

/* ------------------------------------------------------------ options */
enum priority { PRIO_DEFAULT = 0, PRIO_LOW, PRIO_MEDIUM, PRIO_HIGH };

/* 0 means "the test's default" for every numeric field. */
struct opts {
	uint32_t frames;
	uint32_t width, height;
	uint64_t tris;
	uint32_t iters;
	uint32_t groups;
	uint32_t seed;
	uint32_t timeout_s;
	uint32_t repeat;
	enum priority priority;
	int device;             /* -1: first PowerVR device, else device 0 */
	bool keep_going;
	bool json;
	int verbose;            /* -1 quiet, 0 normal, 1+ verbose */
};

#define OPT(v, def) ((v) ? (v) : (def))

/* ------------------------------------------------------------ Vulkan context */
enum obj_kind {
	OBJ_BUFFER, OBJ_IMAGE, OBJ_VIEW, OBJ_MEMORY, OBJ_RENDER_PASS, OBJ_FRAMEBUFFER,
	OBJ_PIPELINE, OBJ_PIPELINE_LAYOUT, OBJ_SET_LAYOUT, OBJ_DESC_POOL, OBJ_SHADER,
};

/* Non-dispatchable handles are pointers on 64-bit hosts, uint64_t elsewhere. */
#if VK_USE_64_BIT_PTR_DEFINES == 1
#define H2U(h) ((uint64_t)(uintptr_t)(h))
#define U2H(T, u) ((T)(uintptr_t)(u))
#else
#define H2U(h) ((uint64_t)(h))
#define U2H(T, u) ((T)(u))
#endif

constexpr int MAX_OBJS = 256;

struct vk {
	VkInstance inst;
	VkPhysicalDevice phys;
	VkPhysicalDeviceProperties props;
	VkPhysicalDeviceMemoryProperties mem;
	VkDevice dev;
	VkQueue queue;
	uint32_t qfam;
	VkCommandPool pool;
	VkFence fence;
	uint64_t timeout_ns;
	bool lost;
	int verbose;
	struct { enum obj_kind kind; uint64_t h; } objs[MAX_OBJS];
	int nobjs;
};

/* What a test reports besides its result. */
struct report {
	uint32_t frames;
	uint64_t checked;       /* values compared */
	uint64_t mismatches;
	char detail[192];
};

[[nodiscard]] enum result vk_init(struct vk *v, const struct opts *o);
void vk_fini(struct vk *v);
void vk_track(struct vk *v, enum obj_kind kind, uint64_t h);

/* Turn a VkResult into a test result, printing where it failed. */
enum result vk_fail(struct vk *v, VkResult r, const char *what, const char *file, int line);

#define VK_TRY(v, call)                                                         \
	({                                                                      \
		VkResult r_ = (call);                                           \
		r_ == VK_SUCCESS ? R_PASS : vk_fail((v), r_, #call, __FILE__, __LINE__); \
	})
/* Return from the calling function if a Vulkan call fails. */
#define VK_CHECK(v, call)                                                       \
	do {                                                                    \
		enum result e_ = VK_TRY(v, call);                               \
		if (e_ != R_PASS)                                               \
			return e_;                                              \
	} while (0)
#define CHECK(expr)                                                             \
	do {                                                                    \
		enum result e_ = (expr);                                        \
		if (e_ != R_PASS)                                               \
			return e_;                                              \
	} while (0)

/* ------------------------------------------------------------ resources */
struct buf {
	VkBuffer b;
	VkDeviceMemory m;
	VkDeviceSize size;
	uint8_t *map;           /* host-visible buffers only */
};

struct img {
	VkImage i;
	VkDeviceMemory m;
	VkImageView view;
	VkFormat fmt;
	uint32_t w, h, layers;
	VkSampleCountFlagBits samples;
	VkImageAspectFlags aspect;
};

[[nodiscard]] enum result buf_create(struct vk *v, struct buf *b, VkDeviceSize size,
				     VkBufferUsageFlags usage, bool host);
[[nodiscard]] enum result img_create(struct vk *v, struct img *im, uint32_t w, uint32_t h,
				     VkFormat fmt, VkImageUsageFlags usage,
				     VkSampleCountFlagBits samples);
[[nodiscard]] enum result shader_create(struct vk *v, VkShaderModule *m, const uint32_t *code,
					size_t size);

/* One-shot command buffers. */
[[nodiscard]] enum result cmd_begin(struct vk *v, VkCommandBuffer *cb);
[[nodiscard]] enum result cmd_submit_wait(struct vk *v, VkCommandBuffer cb);

void img_barrier(VkCommandBuffer cb, const struct img *im, VkImageLayout from, VkImageLayout to,
		 VkAccessFlags src_access, VkAccessFlags dst_access,
		 VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);
/* Copy a whole 1-sample colour image (TRANSFER_SRC_OPTIMAL) into a host buffer. */
void cmd_copy_to_buffer(VkCommandBuffer cb, const struct img *im, const struct buf *b);

/* A render pass with one colour attachment and an optional depth attachment. */
[[nodiscard]] enum result rp_create(struct vk *v, VkRenderPass *rp, VkFormat color,
				    VkFormat depth);
[[nodiscard]] enum result fb_create(struct vk *v, VkFramebuffer *fb, VkRenderPass rp,
				    const struct img *color, const struct img *depth);

struct gfx_desc {
	VkRenderPass rp;
	VkPipelineLayout layout;
	VkShaderModule vs, fs;
	bool depth;             /* depth test LESS + write */
};
[[nodiscard]] enum result gfx_pipeline(struct vk *v, VkPipeline *p, const struct gfx_desc *d);
[[nodiscard]] enum result layout_create(struct vk *v, VkPipelineLayout *l,
					VkShaderStageFlags pc_stages, uint32_t pc_size,
					const VkDescriptorSetLayout *set);

/* ------------------------------------------------------------ checking */
struct rgba { uint8_t r, g, b, a; };

static inline struct rgba rgba_of(uint32_t x)
{
	return (struct rgba){ x & 0xFF, (x >> 8) & 0xFF, (x >> 16) & 0xFF, (x >> 24) & 0xFF };
}

/* Expected colour of pixel (x, y); ctx is the test's data. */
typedef struct rgba (*expect_fn)(uint32_t x, uint32_t y, const void *ctx);

/*
 * Compare a tightly packed R8G8B8A8 readback with the expected function.
 * Adds to r->checked / r->mismatches and prints the first few differences.
 */
enum result check_rgba(struct vk *v, struct report *r, const char *what, const uint8_t *px,
		       uint32_t w, uint32_t h, expect_fn fn, const void *ctx);

/* The LCG the shaders run: x = x * 1664525 + 1013904223, n times. */
static inline uint32_t lcg(uint32_t x, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		x = x * 1664525u + 1013904223u;
	return x;
}

/* ------------------------------------------------------------ tests */
typedef enum result (*test_fn)(struct vk *v, const struct opts *o, struct report *r);

enum result test_multiframe(struct vk *v, const struct opts *o, struct report *r);
enum result test_preta(struct vk *v, const struct opts *o, struct report *r);
enum result test_precdm(struct vk *v, const struct opts *o, struct report *r);
enum result test_xfer(struct vk *v, const struct opts *o, struct report *r);
enum result test_bigpb(struct vk *v, const struct opts *o, struct report *r);

/* grid.c: the tiled-triangle workload shared by pre-ta and bigpb */
struct grid {
	uint32_t w, h;          /* target size */
	uint32_t cells;         /* cells per side; w and h are multiples of it */
	uint32_t layers;
	uint32_t seed;
	uint32_t final_layer;   /* the layer whose colour must remain */
};
/* Pick cells/layers for about `tris` triangles of cell size >= min_px. */
void grid_plan(struct grid *g, uint32_t w, uint32_t h, uint64_t tris, uint32_t min_px);
uint64_t grid_tris(const struct grid *g);
struct rgba grid_expect(uint32_t x, uint32_t y, const void *ctx);
/* Pipeline drawing the grid; depth = farther layers later, test LESS. */
[[nodiscard]] enum result grid_pipeline(struct vk *v, VkPipeline *p, VkPipelineLayout *l,
					VkRenderPass rp, bool depth);
void grid_draw(VkCommandBuffer cb, VkPipelineLayout l, const struct grid *g, bool depth);

#define LOG(v, lvl, ...)                                                        \
	do {                                                                    \
		if ((v)->verbose >= (lvl))                                      \
			fprintf(stderr, __VA_ARGS__);                           \
	} while (0)
