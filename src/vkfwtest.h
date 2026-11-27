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
	bool require_preempt;   /* prio-*: fail unless the high-priority probes overtake */
	int verbose;            /* -1 quiet, 0 normal, 1+ verbose */
	const char *dump;       /* directory for PPM images of mismatches */
	uint32_t test_timeout_s;        /* whole-test limit (0: none) */
	bool keep_device;       /* run the tests on one device */
	uint32_t idle_ms;       /* idle-resume: idle time (0: autosuspend delay + margin) */
	uint32_t bench;         /* run each test N times and print timings */
	bool robust;            /* enable robustBufferAccess (internal) */
	uint32_t max_api;       /* --api: highest version to ask for and use (0: no cap) */
};

#define OPT(v, def) ((v) ? (v) : (def))

/* ------------------------------------------------------------ Vulkan context */
enum obj_kind {
	OBJ_BUFFER, OBJ_IMAGE, OBJ_VIEW, OBJ_MEMORY, OBJ_RENDER_PASS, OBJ_FRAMEBUFFER,
	OBJ_PIPELINE, OBJ_PIPELINE_LAYOUT, OBJ_SET_LAYOUT, OBJ_DESC_POOL, OBJ_SHADER,
	OBJ_SAMPLER, OBJ_QUERY_POOL, OBJ_EVENT, OBJ_SEMAPHORE, OBJ_BUFFER_VIEW,
};

/* Non-dispatchable handles are pointers on 64-bit hosts, uint64_t elsewhere. */
#if VK_USE_64_BIT_PTR_DEFINES == 1
#define H2U(h) ((uint64_t)(uintptr_t)(h))
#define U2H(T, u) ((T)(uintptr_t)(u))
#else
#define H2U(h) ((uint64_t)(h))
#define U2H(T, u) ((T)(u))
#endif

constexpr int MAX_OBJS = 1024;

struct vk {
	VkInstance inst;
	VkPhysicalDevice phys;
	VkPhysicalDeviceProperties props;
	VkPhysicalDeviceMemoryProperties mem;
	VkDevice dev;
	VkQueue queue;          /* queues[0] */
	VkQueue queues[2];
	uint32_t nqueues;
	uint32_t qfam;
	VkCommandPool pool;
	VkFence fence;
	uint64_t timeout_ns;
	bool lost;
	bool has_output_layer;  /* VK_EXT_shader_viewport_index_layer enabled */
	bool has_precise_occlusion;     /* occlusionQueryPrecise enabled */
	bool has_multidraw;             /* multiDrawIndirect enabled */
	VkPhysicalDeviceFeatures feat;  /* the base features enabled */
	bool has_multiview, has_timeline;
	bool has_push_desc, has_dyn_rendering, has_ds_resolve, has_ext_fence_fd;
	bool has_sparse_queue;
	uint32_t inst_api;      /* instance version we asked for (never more than offered) */
	uint32_t api;           /* min(instance, device) version: what tests may use */
	/* 1.3 / 1.4 features, by core version or extension, only when present */
	bool has_sync2, has_local_read, has_index8, has_divisor, has_divisor_zero;
	bool has_load_none, has_store_none;
	VkPhysicalDeviceSubgroupProperties subgroup;
	const char *dump;               /* --dump directory, or null */
	const char *test;               /* name of the running test (dump file names) */
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

typedef enum result (*test_fn)(struct vk *v, const struct opts *o, struct report *r);

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
/* A 2D array image (view type 2D_ARRAY when layers > 1). */
[[nodiscard]] enum result img_create_layers(struct vk *v, struct img *im, uint32_t w,
					    uint32_t h, uint32_t layers, VkFormat fmt,
					    VkImageUsageFlags usage);
/* A 3D image viewed as a 2D array of `depth` layers (Vulkan 1.1); R_SKIP below 1.1. */
[[nodiscard]] enum result img_create_3d_array(struct vk *v, struct img *im, uint32_t w,
					      uint32_t h, uint32_t depth, VkFormat fmt,
					      VkImageUsageFlags usage);
[[nodiscard]] enum result shader_create(struct vk *v, VkShaderModule *m, const uint32_t *code,
					size_t size);

/* One-shot command buffers. */
[[nodiscard]] enum result cmd_begin(struct vk *v, VkCommandBuffer *cb);
[[nodiscard]] enum result cmd_submit_wait(struct vk *v, VkCommandBuffer cb);
/* Submit without waiting, signalling *fence (created here); then wait for it. */
[[nodiscard]] enum result cmd_submit_async(struct vk *v, VkCommandBuffer cb, VkFence *fence);
[[nodiscard]] enum result fence_wait(struct vk *v, VkFence fence, uint64_t timeout_ns);

void img_barrier(VkCommandBuffer cb, const struct img *im, VkImageLayout from, VkImageLayout to,
		 VkAccessFlags src_access, VkAccessFlags dst_access,
		 VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);
/* Copy a whole 1-sample colour image (TRANSFER_SRC_OPTIMAL) into a host buffer. */
void cmd_copy_to_buffer(VkCommandBuffer cb, const struct img *im, const struct buf *b);
/* The same for one layer of an array image (colour or depth), at byte offset `off`. */
void cmd_copy_layer_to_buffer(VkCommandBuffer cb, const struct img *im, uint32_t layer,
			      const struct buf *b, VkDeviceSize off);

/*
 * A render pass with one subpass: `colors` colour attachments (0 = 1), each
 * with an optional 1-sample resolve attachment, and an optional depth/stencil
 * attachment. Attachment order: colours, resolves, depth.
 */
struct rp_desc {
	VkFormat color;
	uint32_t colors;
	VkSampleCountFlagBits samples;  /* 0 = 1 */
	bool color_load;        /* LOAD (from TRANSFER_DST_OPTIMAL) instead of CLEAR */
	bool resolve;
	VkFormat depth;         /* VK_FORMAT_UNDEFINED: none */
	bool depth_load;        /* LOAD depth/stencil (from DEPTH_STENCIL_ATTACHMENT_OPTIMAL) */
	bool stencil;           /* the depth format has stencil: clear/load and store it too */
};
[[nodiscard]] enum result rp_build(struct vk *v, VkRenderPass *rp, const struct rp_desc *d);
[[nodiscard]] enum result fb_create_views(struct vk *v, VkFramebuffer *fb, VkRenderPass rp,
					  const VkImageView *views, uint32_t n, uint32_t w,
					  uint32_t h, uint32_t layers);
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
	uint32_t subpass;
	/* optional; zero/null keeps the defaults above */
	const VkPipelineVertexInputStateCreateInfo *vertex_input;
	const VkPrimitiveTopology *topology;    /* null: triangle list */
	uint32_t colors;                        /* colour attachments, 0 = 1 */
	VkSampleCountFlagBits samples;          /* 0 = 1 */
	bool blend_add;                         /* dst + src (ONE, ONE) on every attachment */
	const VkCompareOp *depth_op;            /* null: LESS */
	bool depth_no_write;
	const VkStencilOpState *stencil;        /* null: no stencil test */
	const VkColorComponentFlags *write_masks;       /* per attachment; null: all */
	const void *pnext;                      /* VkGraphicsPipelineCreateInfo.pNext */
};
[[nodiscard]] enum result gfx_pipeline(struct vk *v, VkPipeline *p, const struct gfx_desc *d);
/* A compute pipeline whose set 0 binding 0 is the storage buffer `ssbo`. */
[[nodiscard]] enum result compute_pipeline(struct vk *v, const uint32_t *code, size_t size,
					   uint32_t pc_size, const struct buf *ssbo,
					   VkPipelineLayout *l, VkPipeline *p, VkDescriptorSet *set);
double now_ms(void);
/* Descriptor set layout + pool + one set for `n` bindings. */
[[nodiscard]] enum result desc_create(struct vk *v, const VkDescriptorSetLayoutBinding *b,
				      uint32_t n, VkDescriptorSetLayout *dsl, VkDescriptorSet *set);
[[nodiscard]] enum result sampler_create(struct vk *v, const VkSamplerCreateInfo *ci,
					 VkSampler *s);
/* Run a test on a device of its own; adds its counters to r. */
enum result run_fresh(const struct opts *o, test_fn fn, struct report *r);
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

/*
 * The LCG the shaders run: x = x * 1664525 + 1013904223, n times. The n-fold
 * composition of the affine map is computed by squaring, so checking a job of
 * millions of iterations costs O(log n) per value.
 */
static inline uint32_t lcg(uint32_t x, uint32_t n)
{
	uint32_t a = 1664525u, c = 1013904223u;         /* f(x) = a x + c */
	uint32_t ra = 1u, rc = 0u;                      /* result map, starts as identity */

	while (n) {
		if (n & 1u) {                           /* result = f o result */
			rc = a * rc + c;
			ra = a * ra;
		}
		c = a * c + c;                          /* f = f o f */
		a = a * a;
		n >>= 1;
	}
	return ra * x + rc;
}

/* The same, allowing each channel to differ by up to `tol` (filtering, sRGB). */
enum result check_rgba_tol(struct vk *v, struct report *r, const char *what, const uint8_t *px,
			   uint32_t w, uint32_t h, uint32_t tol, expect_fn fn, const void *ctx);
/* Compare only every `step`-th pixel (long jobs with costly expectations). */
enum result check_rgba_step(struct vk *v, struct report *r, const char *what, const uint8_t *px,
			    uint32_t w, uint32_t h, uint32_t step, expect_fn fn, const void *ctx);

/* ------------------------------------------------------------ tests */

enum result test_multiframe(struct vk *v, const struct opts *o, struct report *r);
enum result test_preta(struct vk *v, const struct opts *o, struct report *r);
enum result test_precdm(struct vk *v, const struct opts *o, struct report *r);
enum result test_xfer(struct vk *v, const struct opts *o, struct report *r);
enum result test_bigpb(struct vk *v, const struct opts *o, struct report *r);
enum result test_input_attach(struct vk *v, const struct opts *o, struct report *r);
enum result test_rta(struct vk *v, const struct opts *o, struct report *r);
enum result test_hwr_lockup(struct vk *v, const struct opts *o, struct report *r);
enum result test_prio_3d(struct vk *v, const struct opts *o, struct report *r);
enum result test_prio_ta(struct vk *v, const struct opts *o, struct report *r);
enum result test_prio_cdm(struct vk *v, const struct opts *o, struct report *r);
enum result test_texture(struct vk *v, const struct opts *o, struct report *r);
enum result test_zs_load(struct vk *v, const struct opts *o, struct report *r);
enum result test_msaa(struct vk *v, const struct opts *o, struct report *r);
enum result test_mrt(struct vk *v, const struct opts *o, struct report *r);
enum result test_sizes(struct vk *v, const struct opts *o, struct report *r);
enum result test_vertex(struct vk *v, const struct opts *o, struct report *r);
enum result test_indirect(struct vk *v, const struct opts *o, struct report *r);
enum result test_queries(struct vk *v, const struct opts *o, struct report *r);
enum result test_formats(struct vk *v, const struct opts *o, struct report *r);
enum result test_stress(struct vk *v, const struct opts *o, struct report *r);
enum result test_soak(struct vk *v, const struct opts *o, struct report *r);
enum result test_churn(struct vk *v, const struct opts *o, struct report *r);
enum result test_hwr_fault(struct vk *v, const struct opts *o, struct report *r);
enum result test_load_store_none(struct vk *v, const struct opts *o, struct report *r);
enum result test_sync2(struct vk *v, const struct opts *o, struct report *r);
enum result test_vertex_ext(struct vk *v, const struct opts *o, struct report *r);
enum result test_local_read(struct vk *v, const struct opts *o, struct report *r);
enum result test_secondary(struct vk *v, const struct opts *o, struct report *r);
enum result test_multiview(struct vk *v, const struct opts *o, struct report *r);
enum result test_raster(struct vk *v, const struct opts *o, struct report *r);
enum result test_resources(struct vk *v, const struct opts *o, struct report *r);
enum result test_compute_dims(struct vk *v, const struct opts *o, struct report *r);
enum result test_subgroups(struct vk *v, const struct opts *o, struct report *r);
enum result test_buffers(struct vk *v, const struct opts *o, struct report *r);
enum result test_ds_copy(struct vk *v, const struct opts *o, struct report *r);
enum result test_blit_mips(struct vk *v, const struct opts *o, struct report *r);
enum result test_image_copy(struct vk *v, const struct opts *o, struct report *r);
enum result test_texture2(struct vk *v, const struct opts *o, struct report *r);
enum result test_sync(struct vk *v, const struct opts *o, struct report *r);
enum result test_memory(struct vk *v, const struct opts *o, struct report *r);
enum result test_robust(struct vk *v, const struct opts *o, struct report *r);
enum result test_idle_resume(struct vk *v, const struct opts *o, struct report *r);
enum result test_hwr_frag(struct vk *v, const struct opts *o, struct report *r);
enum result test_hwr_3d_fault(struct vk *v, const struct opts *o, struct report *r);
enum result test_sparse(struct vk *v, const struct opts *o, struct report *r);

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
