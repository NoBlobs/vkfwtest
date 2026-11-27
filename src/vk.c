// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Headless Vulkan scaffolding: instance, device, resources that are destroyed
 * in reverse order at the end of each test, submission with a timeout and
 * the readback checker.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vkfwtest.h"

const char *result_name(enum result r)
{
	switch (r) {
	case R_PASS: return "PASS";
	case R_MISMATCH: return "FAIL";
	case R_TIMEOUT: return "TIMEOUT";
	case R_LOST: return "LOST";
	case R_SETUP: return "ERROR";
	case R_SKIP: return "SKIP";
	}
	return "?";
}

enum result vk_fail(struct vk *v, VkResult r, const char *what, const char *file, int line)
{
	if (v->verbose >= 0)
		fprintf(stderr, "vkfwtest: %s:%d: %s -> VkResult %d\n", file, line, what, (int)r);
	switch (r) {
	case VK_ERROR_DEVICE_LOST:
		v->lost = true;
		return R_LOST;
	case VK_TIMEOUT:
		return R_TIMEOUT;
	case VK_ERROR_FEATURE_NOT_PRESENT:
	case VK_ERROR_FORMAT_NOT_SUPPORTED:
	case VK_ERROR_EXTENSION_NOT_PRESENT:
		return R_SKIP;
	default:
		return R_SETUP;
	}
}

void vk_track(struct vk *v, enum obj_kind kind, uint64_t h)
{
	if (v->nobjs >= MAX_OBJS) {
		fprintf(stderr, "vkfwtest: object table full, leaking one object\n");
		return;
	}
	v->objs[v->nobjs].kind = kind;
	v->objs[v->nobjs].h = h;
	v->nobjs++;
}

/* ------------------------------------------------------------ init / fini */
static bool has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
	for (uint32_t i = 0; i < n; i++)
		if (!strcmp(e[i].extensionName, name))
			return true;
	return false;
}

static VkResult create_device(struct vk *v, const struct opts *o, bool with_prio)
{
	float prio = 1.0f;
	VkDeviceQueueGlobalPriorityCreateInfoKHR gp = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR,
	};
	float prios[2] = { prio, prio };
	VkDeviceQueueCreateInfo q = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = v->qfam,
		.queueCount = v->nqueues,
		.pQueuePriorities = prios,
	};
	const char *ext[24];
	uint32_t next = 0, n = 0;
	VkExtensionProperties *e;

	vkEnumerateDeviceExtensionProperties(v->phys, nullptr, &n, nullptr);
	e = calloc(n ? n : 1, sizeof(*e));
	if (!e)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	vkEnumerateDeviceExtensionProperties(v->phys, nullptr, &n, e);
	v->has_output_layer = has_ext(e, n, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
	if (v->has_output_layer)
		ext[next++] = VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME;
	/* extensions that depend on VK_KHR_get_physical_device_properties2: 1.1 only */
	v->has_push_desc = v->api >= VK_API_VERSION_1_1 &&
			   has_ext(e, n, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	if (v->has_push_desc)
		ext[next++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
	bool ext_dyn = v->api >= VK_API_VERSION_1_1 && v->api < VK_API_VERSION_1_3 &&
		       has_ext(e, n, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

	if (ext_dyn)
		ext[next++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
	v->has_ds_resolve = v->api >= VK_API_VERSION_1_2;
	if (!v->has_ds_resolve && v->api >= VK_API_VERSION_1_1 &&
	    has_ext(e, n, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) &&
	    has_ext(e, n, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) {
		ext[next++] = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
		ext[next++] = VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME;
		v->has_ds_resolve = true;
	}
	if (v->api >= VK_API_VERSION_1_1 && has_ext(e, n, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME)) {
		VkPhysicalDeviceExternalFenceInfo efi = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
			.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
		};
		VkExternalFenceProperties efp = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES,
		};
		vkGetPhysicalDeviceExternalFenceProperties(v->phys, &efi, &efp);
		if ((efp.externalFenceFeatures &
		     (VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT |
		      VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT)) ==
		    (VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT |
		     VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT)) {
			ext[next++] = VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME;
			v->has_ext_fence_fd = true;
		}
	}
	/* 1.3 / 1.4 items: core at that version, else the extension when offered */
	bool core13 = v->api >= VK_API_VERSION_1_3, core14 = false;
	bool x_sync2 = false, x_lr = false, x_i8 = false, x_div = false;

#ifdef VK_API_VERSION_1_4
	core14 = v->api >= VK_API_VERSION_1_4;
#endif
	if (!core13 && v->api >= VK_API_VERSION_1_1 &&
	    has_ext(e, n, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
		ext[next++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
		x_sync2 = true;
	}
#ifdef VK_KHR_dynamic_rendering_local_read
	if (!core14 && (core13 || ext_dyn) &&
	    has_ext(e, n, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME)) {
		ext[next++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
		x_lr = true;
	}
#endif
#ifdef VK_KHR_index_type_uint8
	if (!core14 && v->api >= VK_API_VERSION_1_1) {
		if (has_ext(e, n, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME))
			ext[next++] = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME, x_i8 = true;
		else if (has_ext(e, n, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME))
			ext[next++] = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, x_i8 = true;
	}
#endif
#ifdef VK_KHR_vertex_attribute_divisor
	if (!core14 && v->api >= VK_API_VERSION_1_1) {
		if (has_ext(e, n, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
			ext[next++] = VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, x_div = true;
		else if (has_ext(e, n, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
			ext[next++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, x_div = true;
	}
#endif
	/* attachment STORE_OP_NONE is core in 1.3; LOAD_OP_NONE in 1.4 */
	v->has_store_none = core13;
	v->has_load_none = core14;
#ifdef VK_KHR_load_store_op_none
	if (!core14 && has_ext(e, n, VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME)) {
		ext[next++] = VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME;
		v->has_load_none = v->has_store_none = true;
	} else
#endif
	if (!core14 && has_ext(e, n, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME)) {
		ext[next++] = VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME;
		v->has_load_none = v->has_store_none = true;
	}
	if (with_prio) {
		if (has_ext(e, n, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME))
			ext[next++] = VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
		else if (has_ext(e, n, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME))
			ext[next++] = VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
		else {
			free(e);
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}
		gp.globalPriority = o->priority == PRIO_LOW ? VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR :
				    o->priority == PRIO_HIGH ? VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR :
							       VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
		q.pNext = &gp;
	}
	free(e);

	/* the optional features some tests use, when the device has them */
	VkPhysicalDeviceFeatures have, want = { 0 };

	vkGetPhysicalDeviceFeatures(v->phys, &have);
	want.occlusionQueryPrecise = have.occlusionQueryPrecise;
	want.multiDrawIndirect = have.multiDrawIndirect;
	want.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
	want.vertexPipelineStoresAndAtomics = have.vertexPipelineStoresAndAtomics;
	want.depthClamp = have.depthClamp;
	want.depthBiasClamp = have.depthBiasClamp;
	want.depthBounds = have.depthBounds;
	want.wideLines = have.wideLines;
	want.largePoints = have.largePoints;
	want.textureCompressionETC2 = have.textureCompressionETC2;
	want.textureCompressionASTC_LDR = have.textureCompressionASTC_LDR;
	want.imageCubeArray = have.imageCubeArray;
	want.sampleRateShading = have.sampleRateShading;
	want.samplerAnisotropy = have.samplerAnisotropy;
	want.sparseBinding = have.sparseBinding;
	want.sparseResidencyBuffer = have.sparseResidencyBuffer;
	want.robustBufferAccess = o->robust && have.robustBufferAccess;
	v->has_precise_occlusion = have.occlusionQueryPrecise;
	v->has_multidraw = have.multiDrawIndirect;
	v->feat = want;

	/*
	 * Feature structs, queried and enabled only for what the device offers
	 * (its version, or an extension enabled above). Nothing here is required:
	 * a 1.0 device gets no chain at all.
	 */
	VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
	VkPhysicalDeviceMultiviewFeatures fmv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES };
	VkPhysicalDeviceDynamicRenderingFeatures fdr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
	VkPhysicalDeviceSynchronization2Features fs2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
#ifdef VK_KHR_dynamic_rendering_local_read
	VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR };
#endif
#ifdef VK_KHR_index_type_uint8
	VkPhysicalDeviceIndexTypeUint8FeaturesKHR fi8 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR };
#endif
#ifdef VK_KHR_vertex_attribute_divisor
	VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR fdv = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR };
#endif
	/* the same structs again, for enabling */
	VkPhysicalDeviceVulkan12Features e12 = { .sType = f12.sType };
	VkPhysicalDeviceVulkan11Features e11 = { .sType = f11.sType };
	VkPhysicalDeviceMultiviewFeatures emv = { .sType = fmv.sType };
	VkPhysicalDeviceDynamicRenderingFeatures edr = { .sType = fdr.sType };
	VkPhysicalDeviceSynchronization2Features es2 = { .sType = fs2.sType };
#ifdef VK_KHR_dynamic_rendering_local_read
	VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR elr = { .sType = flr.sType };
#endif
#ifdef VK_KHR_index_type_uint8
	VkPhysicalDeviceIndexTypeUint8FeaturesKHR ei8 = { .sType = fi8.sType };
#endif
#ifdef VK_KHR_vertex_attribute_divisor
	VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR edv = { .sType = fdv.sType };
#endif
	void *q_head = nullptr, *e_head = nullptr;
	bool use_dr = core13 || ext_dyn, use_s2 = core13 || x_sync2;
	bool use_lr = core14 || x_lr, use_i8 = core14 || x_i8, use_dv = core14 || x_div;

	(void)use_lr;
	(void)use_i8;
	(void)use_dv;
#define QCHAIN(st) do { (st).pNext = q_head; q_head = &(st); } while (0)
#define ECHAIN(st) do { (st).pNext = e_head; e_head = &(st); } while (0)
	if (v->api >= VK_API_VERSION_1_1) {
		if (v->api >= VK_API_VERSION_1_2) {
			QCHAIN(f11);
			QCHAIN(f12);
		} else {
			QCHAIN(fmv);
		}
		if (use_dr)
			QCHAIN(fdr);
		if (use_s2)
			QCHAIN(fs2);
#ifdef VK_KHR_dynamic_rendering_local_read
		if (use_lr)
			QCHAIN(flr);
#endif
#ifdef VK_KHR_index_type_uint8
		if (use_i8)
			QCHAIN(fi8);
#endif
#ifdef VK_KHR_vertex_attribute_divisor
		if (use_dv)
			QCHAIN(fdv);
#endif
		VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
						 .pNext = q_head };

		vkGetPhysicalDeviceFeatures2(v->phys, &f2);
		if (v->api >= VK_API_VERSION_1_2) {
			e11.multiview = f11.multiview;
			e12.timelineSemaphore = f12.timelineSemaphore;
			v->has_multiview = f11.multiview;
			v->has_timeline = f12.timelineSemaphore;
			ECHAIN(e11);
			ECHAIN(e12);
		} else {
			emv.multiview = fmv.multiview;
			v->has_multiview = fmv.multiview;
			ECHAIN(emv);
		}
		if (use_dr && fdr.dynamicRendering) {
			edr.dynamicRendering = VK_TRUE;
			v->has_dyn_rendering = true;
			ECHAIN(edr);
		}
		if (use_s2 && fs2.synchronization2) {
			es2.synchronization2 = VK_TRUE;
			v->has_sync2 = true;
			ECHAIN(es2);
		}
#ifdef VK_KHR_dynamic_rendering_local_read
		if (use_lr && v->has_dyn_rendering && flr.dynamicRenderingLocalRead) {
			elr.dynamicRenderingLocalRead = VK_TRUE;
			v->has_local_read = true;
			ECHAIN(elr);
		}
#endif
#ifdef VK_KHR_index_type_uint8
		if (use_i8 && fi8.indexTypeUint8) {
			ei8.indexTypeUint8 = VK_TRUE;
			v->has_index8 = true;
			ECHAIN(ei8);
		}
#endif
#ifdef VK_KHR_vertex_attribute_divisor
		if (use_dv && fdv.vertexAttributeInstanceRateDivisor) {
			edv.vertexAttributeInstanceRateDivisor = VK_TRUE;
			edv.vertexAttributeInstanceRateZeroDivisor = fdv.vertexAttributeInstanceRateZeroDivisor;
			v->has_divisor = true;
			v->has_divisor_zero = fdv.vertexAttributeInstanceRateZeroDivisor;
			ECHAIN(edv);
		}
#endif
	}
#undef QCHAIN
#undef ECHAIN
	void *chain = e_head;
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = chain,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &q,
		.enabledExtensionCount = next,
		.ppEnabledExtensionNames = ext,
		.pEnabledFeatures = &want,
	};
	return vkCreateDevice(v->phys, &dci, nullptr, &v->dev);
}

enum result vk_init(struct vk *v, const struct opts *o)
{
	/*
	 * Ask for the version the loader offers (1.0 if it predates
	 * vkEnumerateInstanceVersion), never more: a 1.0-only driver then still
	 * works. If the driver refuses, retry with 1.0.
	 */
	uint32_t inst_ver = VK_API_VERSION_1_0;
	PFN_vkEnumerateInstanceVersion enum_ver =
		(PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(nullptr,
								      "vkEnumerateInstanceVersion");

	if (enum_ver && enum_ver(&inst_ver) != VK_SUCCESS)
		inst_ver = VK_API_VERSION_1_0;
	if (o->max_api && inst_ver > o->max_api)
		inst_ver = o->max_api;         /* --api: behave as a driver of that version */
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkfwtest",
		.apiVersion = inst_ver,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkPhysicalDevice *devs;
	uint32_t n = 0;
	int pick = -1;

	memset(v, 0, sizeof(*v));
	v->verbose = o->verbose;
	v->dump = o->dump;
	v->timeout_ns = (uint64_t)OPT(o->timeout_s, 60) * 1000000000ull;
	VkResult ir = vkCreateInstance(&ici, nullptr, &v->inst);

	if (ir == VK_ERROR_INCOMPATIBLE_DRIVER && inst_ver != VK_API_VERSION_1_0) {
		inst_ver = app.apiVersion = VK_API_VERSION_1_0;
		ir = vkCreateInstance(&ici, nullptr, &v->inst);
	}
	VK_CHECK(v, ir);
	v->inst_api = inst_ver;

	VK_CHECK(v, vkEnumeratePhysicalDevices(v->inst, &n, nullptr));
	if (!n) {
		fprintf(stderr, "vkfwtest: no Vulkan device\n");
		return R_SKIP;
	}
	devs = calloc(n, sizeof(*devs));
	if (!devs)
		return R_SETUP;
	vkEnumeratePhysicalDevices(v->inst, &n, devs);
	if (o->device >= 0) {
		if ((uint32_t)o->device >= n) {
			fprintf(stderr, "vkfwtest: device %d does not exist (%" PRIu32 " found)\n",
				o->device, n);
			free(devs);
			return R_SETUP;
		}
		pick = o->device;
	} else {
		for (uint32_t i = 0; i < n && pick < 0; i++) {
			VkPhysicalDeviceProperties p;

			vkGetPhysicalDeviceProperties(devs[i], &p);
			if (strstr(p.deviceName, "PowerVR"))
				pick = (int)i;
		}
		if (pick < 0)
			pick = 0;
	}
	v->phys = devs[pick];
	free(devs);
	vkGetPhysicalDeviceProperties(v->phys, &v->props);
	vkGetPhysicalDeviceMemoryProperties(v->phys, &v->mem);
	/* what tests may use: the lower of the instance and device versions */
	v->api = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(v->props.apiVersion),
				     VK_API_VERSION_MINOR(v->props.apiVersion), 0);
	if (v->api > v->inst_api)
		v->api = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(v->inst_api),
					     VK_API_VERSION_MINOR(v->inst_api), 0);
	LOG(v, 1, "vkfwtest: Vulkan %u.%u (instance %u.%u, device %u.%u)\n",
	    VK_API_VERSION_MAJOR(v->api), VK_API_VERSION_MINOR(v->api),
	    VK_API_VERSION_MAJOR(v->inst_api), VK_API_VERSION_MINOR(v->inst_api),
	    VK_API_VERSION_MAJOR(v->props.apiVersion), VK_API_VERSION_MINOR(v->props.apiVersion));
	if (v->api >= VK_API_VERSION_1_1) {
		VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
						   .pNext = &v->subgroup };

		v->subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
		vkGetPhysicalDeviceProperties2(v->phys, &p2);
	}
	LOG(v, 1, "vkfwtest: device %d: %s\n", pick, v->props.deviceName);

	/* one queue family with graphics and compute (transfer is implied) */
	VkQueueFamilyProperties qf[16];
	uint32_t nq = 16;

	vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, qf);
	v->qfam = UINT32_MAX;
	for (uint32_t i = 0; i < nq; i++)
		if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
		    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
			v->qfam = i;
			v->nqueues = qf[i].queueCount >= 2 ? 2 : 1;
			v->has_sparse_queue = (qf[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
			break;
		}
	if (v->qfam == UINT32_MAX) {
		fprintf(stderr, "vkfwtest: no graphics+compute queue family\n");
		return R_SKIP;
	}

	VkResult r = VK_ERROR_INITIALIZATION_FAILED;

	if (o->priority != PRIO_DEFAULT) {
		r = create_device(v, o, true);
		if (r != VK_SUCCESS)
			LOG(v, 0, "vkfwtest: priority %d not available (VkResult %d), using the default\n",
			    (int)o->priority, (int)r);
	}
	if (r != VK_SUCCESS)
		VK_CHECK(v, create_device(v, o, false));
	for (uint32_t i = 0; i < v->nqueues; i++)
		vkGetDeviceQueue(v->dev, v->qfam, i, &v->queues[i]);
	v->queue = v->queues[0];

	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = v->qfam,
	};
	VK_CHECK(v, vkCreateCommandPool(v->dev, &pci, nullptr, &v->pool));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VK_CHECK(v, vkCreateFence(v->dev, &fci, nullptr, &v->fence));
	return R_PASS;
}

void vk_fini(struct vk *v)
{
	if (v->dev) {
		if (!v->lost)
			vkDeviceWaitIdle(v->dev);
		for (int i = v->nobjs - 1; i >= 0; i--) {
			uint64_t h = v->objs[i].h;

			switch (v->objs[i].kind) {
			case OBJ_BUFFER: vkDestroyBuffer(v->dev, U2H(VkBuffer, h), nullptr); break;
			case OBJ_IMAGE: vkDestroyImage(v->dev, U2H(VkImage, h), nullptr); break;
			case OBJ_VIEW: vkDestroyImageView(v->dev, U2H(VkImageView, h), nullptr); break;
			case OBJ_MEMORY: vkFreeMemory(v->dev, U2H(VkDeviceMemory, h), nullptr); break;
			case OBJ_RENDER_PASS:
				vkDestroyRenderPass(v->dev, U2H(VkRenderPass, h), nullptr);
				break;
			case OBJ_FRAMEBUFFER:
				vkDestroyFramebuffer(v->dev, U2H(VkFramebuffer, h), nullptr);
				break;
			case OBJ_PIPELINE: vkDestroyPipeline(v->dev, U2H(VkPipeline, h), nullptr); break;
			case OBJ_PIPELINE_LAYOUT:
				vkDestroyPipelineLayout(v->dev, U2H(VkPipelineLayout, h), nullptr);
				break;
			case OBJ_SET_LAYOUT:
				vkDestroyDescriptorSetLayout(v->dev, U2H(VkDescriptorSetLayout, h), nullptr);
				break;
			case OBJ_DESC_POOL:
				vkDestroyDescriptorPool(v->dev, U2H(VkDescriptorPool, h), nullptr);
				break;
			case OBJ_SHADER:
				vkDestroyShaderModule(v->dev, U2H(VkShaderModule, h), nullptr);
				break;
			case OBJ_SAMPLER: vkDestroySampler(v->dev, U2H(VkSampler, h), nullptr); break;
			case OBJ_EVENT: vkDestroyEvent(v->dev, U2H(VkEvent, h), nullptr); break;
			case OBJ_SEMAPHORE:
				vkDestroySemaphore(v->dev, U2H(VkSemaphore, h), nullptr);
				break;
			case OBJ_BUFFER_VIEW:
				vkDestroyBufferView(v->dev, U2H(VkBufferView, h), nullptr);
				break;
			case OBJ_QUERY_POOL:
				vkDestroyQueryPool(v->dev, U2H(VkQueryPool, h), nullptr);
				break;
			}
		}
		if (v->fence)
			vkDestroyFence(v->dev, v->fence, nullptr);
		if (v->pool)
			vkDestroyCommandPool(v->dev, v->pool, nullptr);
		vkDestroyDevice(v->dev, nullptr);
	}
	if (v->inst)
		vkDestroyInstance(v->inst, nullptr);
	v->dev = nullptr;
	v->inst = nullptr;
	v->nobjs = 0;
}

/* ------------------------------------------------------------ memory */
static int mem_type(const struct vk *v, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (v->mem.memoryTypes[i].propertyFlags & want) == want)
			return (int)i;
	return -1;
}

static enum result alloc_bind(struct vk *v, VkDeviceMemory *m, VkMemoryRequirements req,
			      VkMemoryPropertyFlags want)
{
	int t = mem_type(v, req.memoryTypeBits, want);

	if (t < 0)
		t = mem_type(v, req.memoryTypeBits, 0);
	if (t < 0) {
		fprintf(stderr, "vkfwtest: no memory type for bits 0x%" PRIx32 "\n",
			req.memoryTypeBits);
		return R_SETUP;
	}
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = (uint32_t)t,
	};
	VK_CHECK(v, vkAllocateMemory(v->dev, &mai, nullptr, m));
	vk_track(v, OBJ_MEMORY, H2U(*m));
	return R_PASS;
}

enum result buf_create(struct vk *v, struct buf *b, VkDeviceSize size, VkBufferUsageFlags usage,
		       bool host)
{
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryRequirements req;

	memset(b, 0, sizeof(*b));
	b->size = size;
	VK_CHECK(v, vkCreateBuffer(v->dev, &bci, nullptr, &b->b));
	vk_track(v, OBJ_BUFFER, H2U(b->b));
	vkGetBufferMemoryRequirements(v->dev, b->b, &req);
	CHECK(alloc_bind(v, &b->m, req, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
						      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
					       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
	VK_CHECK(v, vkBindBufferMemory(v->dev, b->b, b->m, 0));
	if (host) {
		void *p;

		VK_CHECK(v, vkMapMemory(v->dev, b->m, 0, VK_WHOLE_SIZE, 0, &p));
		b->map = p;
	}
	return R_PASS;
}

static bool is_depth(VkFormat f)
{
	return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM ||
	       f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static enum result img_make(struct vk *v, struct img *im, uint32_t w, uint32_t h,
			    uint32_t layers, VkFormat fmt, VkImageUsageFlags usage,
			    VkSampleCountFlagBits samples)
{
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = fmt,
		.extent = { w, h, 1 },
		.mipLevels = 1,
		.arrayLayers = layers,
		.samples = samples,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkMemoryRequirements req;

	memset(im, 0, sizeof(*im));
	im->fmt = fmt;
	im->w = w;
	im->h = h;
	im->layers = layers;
	im->samples = samples;
	im->aspect = is_depth(fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	VK_CHECK(v, vkCreateImage(v->dev, &ici, nullptr, &im->i));
	vk_track(v, OBJ_IMAGE, H2U(im->i));
	vkGetImageMemoryRequirements(v->dev, im->i, &req);
	CHECK(alloc_bind(v, &im->m, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
	VK_CHECK(v, vkBindImageMemory(v->dev, im->i, im->m, 0));
	if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
		VkImageViewCreateInfo vci = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = im->i,
			.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
			.format = fmt,
			.subresourceRange = { im->aspect, 0, 1, 0, layers },
		};
		VK_CHECK(v, vkCreateImageView(v->dev, &vci, nullptr, &im->view));
		vk_track(v, OBJ_VIEW, H2U(im->view));
	}
	return R_PASS;
}

enum result img_create(struct vk *v, struct img *im, uint32_t w, uint32_t h, VkFormat fmt,
		       VkImageUsageFlags usage, VkSampleCountFlagBits samples)
{
	return img_make(v, im, w, h, 1, fmt, usage, samples);
}

enum result img_create_layers(struct vk *v, struct img *im, uint32_t w, uint32_t h,
			      uint32_t layers, VkFormat fmt, VkImageUsageFlags usage)
{
	return img_make(v, im, w, h, layers, fmt, usage, VK_SAMPLE_COUNT_1_BIT);
}

/*
 * A 3D image of `depth` slices that can be viewed as a 2D array (Vulkan 1.1,
 * VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT); im->view is that 2D array view.
 */
enum result img_create_3d_array(struct vk *v, struct img *im, uint32_t w, uint32_t h,
				uint32_t depth, VkFormat fmt, VkImageUsageFlags usage)
{
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT,
		.imageType = VK_IMAGE_TYPE_3D,
		.format = fmt,
		.extent = { w, h, depth },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkMemoryRequirements req;

	if (v->api < VK_API_VERSION_1_1)
		return R_SKIP;
	memset(im, 0, sizeof(*im));
	im->fmt = fmt;
	im->w = w;
	im->h = h;
	im->layers = depth;
	im->samples = VK_SAMPLE_COUNT_1_BIT;
	im->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	VK_CHECK(v, vkCreateImage(v->dev, &ici, nullptr, &im->i));
	vk_track(v, OBJ_IMAGE, H2U(im->i));
	vkGetImageMemoryRequirements(v->dev, im->i, &req);
	CHECK(alloc_bind(v, &im->m, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
	VK_CHECK(v, vkBindImageMemory(v->dev, im->i, im->m, 0));
	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = im->i,
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, depth },
	};
	VK_CHECK(v, vkCreateImageView(v->dev, &vci, nullptr, &im->view));
	vk_track(v, OBJ_VIEW, H2U(im->view));
	return R_PASS;
}

enum result shader_create(struct vk *v, VkShaderModule *m, const uint32_t *code, size_t size)
{
	VkShaderModuleCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = code,
	};

	VK_CHECK(v, vkCreateShaderModule(v->dev, &sci, nullptr, m));
	vk_track(v, OBJ_SHADER, H2U(*m));
	return R_PASS;
}

/* ------------------------------------------------------------ commands */
enum result cmd_begin(struct vk *v, VkCommandBuffer *cb)
{
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = v->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	VK_CHECK(v, vkAllocateCommandBuffers(v->dev, &cai, cb));
	VK_CHECK(v, vkBeginCommandBuffer(*cb, &bi));
	return R_PASS;
}

enum result cmd_submit_wait(struct vk *v, VkCommandBuffer cb)
{
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
	};
	enum result r;

	VK_CHECK(v, vkEndCommandBuffer(cb));
	VK_CHECK(v, vkResetFences(v->dev, 1, &v->fence));
	VK_CHECK(v, vkQueueSubmit(v->queue, 1, &si, v->fence));
	r = VK_TRY(v, vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, v->timeout_ns));
	if (r == R_PASS)
		vkFreeCommandBuffers(v->dev, v->pool, 1, &cb);
	return r;
}

enum result cmd_submit_async(struct vk *v, VkCommandBuffer cb, VkFence *fence)
{
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb,
	};
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

	VK_CHECK(v, vkEndCommandBuffer(cb));
	VK_CHECK(v, vkCreateFence(v->dev, &fci, nullptr, fence));
	VK_CHECK(v, vkQueueSubmit(v->queue, 1, &si, *fence));
	return R_PASS;
}

enum result fence_wait(struct vk *v, VkFence fence, uint64_t timeout_ns)
{
	enum result r = VK_TRY(v, vkWaitForFences(v->dev, 1, &fence, VK_TRUE, timeout_ns));

	if (r != R_TIMEOUT)
		vkDestroyFence(v->dev, fence, nullptr);
	return r;
}

void img_barrier(VkCommandBuffer cb, const struct img *im, VkImageLayout from, VkImageLayout to,
		 VkAccessFlags src_access, VkAccessFlags dst_access,
		 VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src_access,
		.dstAccessMask = dst_access,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = im->i,
		.subresourceRange = { im->aspect, 0, 1, 0, im->layers },
	};

	vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void cmd_copy_to_buffer(VkCommandBuffer cb, const struct img *im, const struct buf *b)
{
	VkBufferImageCopy c = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { im->w, im->h, 1 },
	};
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = b->b,
		.size = VK_WHOLE_SIZE,
	};

	vkCmdCopyImageToBuffer(cb, im->i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->b, 1, &c);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
			     nullptr, 1, &hb, 0, nullptr);
}

void cmd_copy_layer_to_buffer(VkCommandBuffer cb, const struct img *im, uint32_t layer,
			      const struct buf *b, VkDeviceSize off)
{
	VkBufferImageCopy c = {
		.bufferOffset = off,
		.imageSubresource = { im->aspect, 0, layer, 1 },
		.imageExtent = { im->w, im->h, 1 },
	};
	VkBufferMemoryBarrier hb = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = b->b,
		.size = VK_WHOLE_SIZE,
	};

	vkCmdCopyImageToBuffer(cb, im->i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->b, 1, &c);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
			     nullptr, 1, &hb, 0, nullptr);
}

/* ------------------------------------------------------------ pipelines */
enum result rp_build(struct vk *v, VkRenderPass *rp, const struct rp_desc *d)
{
	uint32_t nc = OPT(d->colors, 1), n = 0;
	VkSampleCountFlagBits samples = d->samples ? d->samples : VK_SAMPLE_COUNT_1_BIT;
	bool ms = samples != VK_SAMPLE_COUNT_1_BIT;
	bool has_depth = d->depth != VK_FORMAT_UNDEFINED;
	VkAttachmentDescription a[17];
	VkAttachmentReference cref[8], rref[8], dref;

	if (nc > 8)
		return R_SETUP;
	for (uint32_t i = 0; i < nc; i++) {
		a[n] = (VkAttachmentDescription){
			.format = d->color,
			.samples = samples,
			.loadOp = d->color_load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = d->color_load ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL :
							 VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
					    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		};
		cref[i] = (VkAttachmentReference){ n++, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	}
	for (uint32_t i = 0; d->resolve && i < nc; i++) {
		a[n] = (VkAttachmentDescription){
			.format = d->color,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		};
		rref[i] = (VkAttachmentReference){ n++, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	}
	if (has_depth) {
		VkAttachmentLoadOp lo = d->depth_load ? VK_ATTACHMENT_LOAD_OP_LOAD :
							VK_ATTACHMENT_LOAD_OP_CLEAR;

		a[n] = (VkAttachmentDescription){
			.format = d->depth,
			.samples = samples,
			.loadOp = lo,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = d->stencil ? lo : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = d->stencil ? VK_ATTACHMENT_STORE_OP_STORE :
						       VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = d->depth_load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
							 VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		};
		dref = (VkAttachmentReference){ n++, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	}
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = nc,
		.pColorAttachments = cref,
		.pResolveAttachments = d->resolve ? rref : nullptr,
		.pDepthStencilAttachment = has_depth ? &dref : nullptr,
	};
	VkSubpassDependency dep[2] = {
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
					VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
					VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
					 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
					 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
					 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
					 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
					 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		},
	};
	VkRenderPassCreateInfo rci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = n,
		.pAttachments = a,
		.subpassCount = 1,
		.pSubpasses = &sp,
		.dependencyCount = 2,
		.pDependencies = dep,
	};

	VK_CHECK(v, vkCreateRenderPass(v->dev, &rci, nullptr, rp));
	vk_track(v, OBJ_RENDER_PASS, H2U(*rp));
	return R_PASS;
}

enum result fb_create_views(struct vk *v, VkFramebuffer *fb, VkRenderPass rp,
			    const VkImageView *views, uint32_t n, uint32_t w, uint32_t h,
			    uint32_t layers)
{
	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp,
		.attachmentCount = n,
		.pAttachments = views,
		.width = w,
		.height = h,
		.layers = layers,
	};

	VK_CHECK(v, vkCreateFramebuffer(v->dev, &fci, nullptr, fb));
	vk_track(v, OBJ_FRAMEBUFFER, H2U(*fb));
	return R_PASS;
}

enum result desc_create(struct vk *v, const VkDescriptorSetLayoutBinding *b, uint32_t n,
			VkDescriptorSetLayout *dsl, VkDescriptorSet *set)
{
	VkDescriptorPoolSize ps[16];
	uint32_t np = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t k = 0;

		while (k < np && ps[k].type != b[i].descriptorType)
			k++;
		if (k == np) {
			if (np == 16)
				return R_SETUP;
			ps[np++] = (VkDescriptorPoolSize){ b[i].descriptorType, 0 };
		}
		ps[k].descriptorCount += b[i].descriptorCount;
	}
	VkDescriptorSetLayoutCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = n,
		.pBindings = b,
	};
	VK_CHECK(v, vkCreateDescriptorSetLayout(v->dev, &dci, nullptr, dsl));
	vk_track(v, OBJ_SET_LAYOUT, H2U(*dsl));
	VkDescriptorPool pool;
	VkDescriptorPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = np,
		.pPoolSizes = ps,
	};
	VK_CHECK(v, vkCreateDescriptorPool(v->dev, &pci, nullptr, &pool));
	vk_track(v, OBJ_DESC_POOL, H2U(pool));
	VkDescriptorSetAllocateInfo dai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool,
		.descriptorSetCount = 1,
		.pSetLayouts = dsl,
	};
	VK_CHECK(v, vkAllocateDescriptorSets(v->dev, &dai, set));
	return R_PASS;
}

enum result sampler_create(struct vk *v, const VkSamplerCreateInfo *ci, VkSampler *s)
{
	VK_CHECK(v, vkCreateSampler(v->dev, ci, nullptr, s));
	vk_track(v, OBJ_SAMPLER, H2U(*s));
	return R_PASS;
}

enum result run_fresh(const struct opts *o, test_fn fn, struct report *r)
{
	struct vk v2;
	struct report r2 = { 0 };
	enum result res = vk_init(&v2, o);

	if (res == R_PASS)
		res = fn(&v2, o, &r2);
	vk_fini(&v2);
	r->frames += r2.frames;
	r->checked += r2.checked;
	r->mismatches += r2.mismatches;
	if (res != R_PASS && res != R_SKIP)
		snprintf(r->detail, sizeof(r->detail), "%.180s", r2.detail);
	return res;
}

enum result rp_create(struct vk *v, VkRenderPass *rp, VkFormat color, VkFormat depth)
{
	VkAttachmentDescription a[2] = {
		{
			.format = color,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		},
		{
			.format = depth,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			/* stored, so that partial renders keep the depth (spec section 4) */
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		},
	};
	VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	bool has_depth = depth != VK_FORMAT_UNDEFINED;
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &cref,
		.pDepthStencilAttachment = has_depth ? &dref : nullptr,
	};
	VkSubpassDependency dep = {
		.srcSubpass = 0,
		.dstSubpass = VK_SUBPASS_EXTERNAL,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	VkRenderPassCreateInfo rci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = has_depth ? 2 : 1,
		.pAttachments = a,
		.subpassCount = 1,
		.pSubpasses = &sp,
		.dependencyCount = 1,
		.pDependencies = &dep,
	};

	VK_CHECK(v, vkCreateRenderPass(v->dev, &rci, nullptr, rp));
	vk_track(v, OBJ_RENDER_PASS, H2U(*rp));
	return R_PASS;
}

enum result fb_create(struct vk *v, VkFramebuffer *fb, VkRenderPass rp, const struct img *color,
		      const struct img *depth)
{
	VkImageView views[2] = { color->view, depth ? depth->view : VK_NULL_HANDLE };
	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp,
		.attachmentCount = depth ? 2 : 1,
		.pAttachments = views,
		.width = color->w,
		.height = color->h,
		.layers = 1,
	};

	VK_CHECK(v, vkCreateFramebuffer(v->dev, &fci, nullptr, fb));
	vk_track(v, OBJ_FRAMEBUFFER, H2U(*fb));
	return R_PASS;
}

enum result layout_create(struct vk *v, VkPipelineLayout *l, VkShaderStageFlags pc_stages,
			  uint32_t pc_size, const VkDescriptorSetLayout *set)
{
	VkPushConstantRange pc = { pc_stages, 0, pc_size };
	VkPipelineLayoutCreateInfo lci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = set ? 1 : 0,
		.pSetLayouts = set,
		.pushConstantRangeCount = pc_size ? 1 : 0,
		.pPushConstantRanges = &pc,
	};

	VK_CHECK(v, vkCreatePipelineLayout(v->dev, &lci, nullptr, l));
	vk_track(v, OBJ_PIPELINE_LAYOUT, H2U(*l));
	return R_PASS;
}

double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

enum result compute_pipeline(struct vk *v, const uint32_t *code, size_t size, uint32_t pc_size,
			     const struct buf *ssbo, VkPipelineLayout *l, VkPipeline *p,
			     VkDescriptorSet *set)
{
	VkShaderModule cs;
	VkDescriptorSetLayout dsl;
	VkDescriptorPool pool;

	CHECK(shader_create(v, &cs, code, size));
	VkDescriptorSetLayoutBinding bind = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	VkDescriptorSetLayoutCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &bind,
	};
	VK_CHECK(v, vkCreateDescriptorSetLayout(v->dev, &dci, nullptr, &dsl));
	vk_track(v, OBJ_SET_LAYOUT, H2U(dsl));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &ps,
	};
	VK_CHECK(v, vkCreateDescriptorPool(v->dev, &pci, nullptr, &pool));
	vk_track(v, OBJ_DESC_POOL, H2U(pool));
	VkDescriptorSetAllocateInfo dai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &dsl,
	};
	VK_CHECK(v, vkAllocateDescriptorSets(v->dev, &dai, set));
	VkDescriptorBufferInfo dbi = { ssbo->b, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wr = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = *set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &dbi,
	};
	vkUpdateDescriptorSets(v->dev, 1, &wr, 0, nullptr);
	CHECK(layout_create(v, l, VK_SHADER_STAGE_COMPUTE_BIT, pc_size, &dsl));
	VkComputePipelineCreateInfo cci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = cs,
			.pName = "main",
		},
		.layout = *l,
	};
	VK_CHECK(v, vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &cci, nullptr, p));
	vk_track(v, OBJ_PIPELINE, H2U(*p));
	return R_PASS;
}

enum result gfx_pipeline(struct vk *v, VkPipeline *p, const struct gfx_desc *d)
{
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = d->vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = d->fs, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = d->topology ? *d->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = d->samples ? d->samples : VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineDepthStencilStateCreateInfo ds = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = d->depth,
		.depthWriteEnable = d->depth && !d->depth_no_write,
		.depthCompareOp = d->depth_op ? *d->depth_op : VK_COMPARE_OP_LESS,
		.stencilTestEnable = d->stencil != nullptr,
	};
	if (d->stencil) {
		ds.front = *d->stencil;
		ds.back = *d->stencil;
	}
	VkPipelineColorBlendAttachmentState cba[8];

	for (uint32_t i = 0; i < 8; i++)
		cba[i] = (VkPipelineColorBlendAttachmentState){
			.blendEnable = d->blend_add,
			.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
			.dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
			.colorBlendOp = VK_BLEND_OP_ADD,
			.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
			.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
			.alphaBlendOp = VK_BLEND_OP_ADD,
			.colorWriteMask = d->write_masks ? d->write_masks[i < OPT(d->colors, 1) ? i : 0] : 0xF,
		};
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = OPT(d->colors, 1),
		.pAttachments = cba,
	};
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dyn,
	};
	VkGraphicsPipelineCreateInfo gci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = st,
		.pVertexInputState = d->vertex_input ? d->vertex_input : &vi,
		.pInputAssemblyState = &ia,
		.pViewportState = &vp,
		.pRasterizationState = &rs,
		.pMultisampleState = &ms,
		.pDepthStencilState = &ds,
		.pColorBlendState = &cb,
		.pDynamicState = &dy,
		.pNext = d->pnext,
		.layout = d->layout,
		.renderPass = d->rp,
		.subpass = d->subpass,
	};

	VK_CHECK(v, vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gci, nullptr, p));
	vk_track(v, OBJ_PIPELINE, H2U(*p));
	return R_PASS;
}

/* ------------------------------------------------------------ checking */
static bool near(uint8_t a, uint8_t b, uint32_t tol)
{
	return (uint32_t)(a > b ? a - b : b - a) <= tol;
}

/*
 * --dump: write what was read back, what was expected and a mask of the
 * differing pixels as binary PPM (RGB; alpha dropped) files named
 * <dir>/<test>-<what>-{got,expected,diff}.ppm.
 */
static void dump_images(struct vk *v, const char *what, const uint8_t *px, uint32_t w, uint32_t h,
			uint32_t tol, expect_fn fn, const void *ctx)
{
	char base[512], name[600], clean[128];
	size_t k = 0;
	FILE *f[3];
	static const char *const kind[3] = { "got", "expected", "diff" };

	for (const char *c = what; *c && k + 1 < sizeof(clean); c++)
		clean[k++] = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
				     (*c >= '0' && *c <= '9') ? *c : '_';
	clean[k] = '\0';
	snprintf(base, sizeof(base), "%s/%s-%s", v->dump, v->test ? v->test : "test", clean);
	for (int i = 0; i < 3; i++) {
		snprintf(name, sizeof(name), "%s-%s.ppm", base, kind[i]);
		f[i] = fopen(name, "wb");
		if (!f[i]) {
			fprintf(stderr, "vkfwtest: cannot write %s\n", name);
			for (int j = 0; j < i; j++)
				fclose(f[j]);
			return;
		}
		fprintf(f[i], "P6\n%u %u\n255\n", w, h);
	}
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *p = px + ((size_t)y * w + x) * 4;
			struct rgba e = fn(x, y, ctx);
			bool ok = near(p[0], e.r, tol) && near(p[1], e.g, tol) && near(p[2], e.b, tol) &&
				  near(p[3], e.a, tol);
			uint8_t d[3] = { ok ? 0 : 255, 0, 0 };
			uint8_t ex[3] = { e.r, e.g, e.b };

			fwrite(p, 1, 3, f[0]);
			fwrite(ex, 1, 3, f[1]);
			fwrite(d, 1, 3, f[2]);
		}
	for (int i = 0; i < 3; i++)
		fclose(f[i]);
	fprintf(stderr, "vkfwtest: wrote %s-{got,expected,diff}.ppm\n", base);
}

enum result check_rgba_step(struct vk *v, struct report *r, const char *what, const uint8_t *px,
			    uint32_t w, uint32_t h, uint32_t step, expect_fn fn, const void *ctx)
{
	uint64_t bad = 0, n = 0;

	if (!step)
		step = 1;
	for (uint64_t i = 0; i < (uint64_t)w * h; i += step) {
		uint32_t x = (uint32_t)(i % w), y = (uint32_t)(i / w);
		const uint8_t *p = px + i * 4;
		struct rgba e = fn(x, y, ctx);

		n++;
		if (p[0] == e.r && p[1] == e.g && p[2] == e.b && p[3] == e.a)
			continue;
		if (bad < 8 && v->verbose >= 0)
			fprintf(stderr,
				"vkfwtest: %s: (%" PRIu32 ",%" PRIu32 ") got %02x%02x%02x%02x"
				" expected %02x%02x%02x%02x\n",
				what, x, y, p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a);
		bad++;
	}
	r->checked += n;
	r->mismatches += bad;
	if (bad) {
		if (v->dump)
			dump_images(v, what, px, w, h, 0, fn, ctx);
		snprintf(r->detail, sizeof(r->detail), "%s: %" PRIu64 " of %" PRIu64 " pixels wrong",
			 what, bad, n);
		return R_MISMATCH;
	}
	return R_PASS;
}

enum result check_rgba_tol(struct vk *v, struct report *r, const char *what, const uint8_t *px,
			   uint32_t w, uint32_t h, uint32_t tol, expect_fn fn, const void *ctx)
{
	uint64_t bad = 0;

	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *p = px + ((size_t)y * w + x) * 4;
			struct rgba e = fn(x, y, ctx);

			if (near(p[0], e.r, tol) && near(p[1], e.g, tol) && near(p[2], e.b, tol) &&
			    near(p[3], e.a, tol))
				continue;
			if (bad < 8 && v->verbose >= 0)
				fprintf(stderr,
					"vkfwtest: %s: (%" PRIu32 ",%" PRIu32 ") got %02x%02x%02x%02x"
					" expected %02x%02x%02x%02x (+-%" PRIu32 ")\n",
					what, x, y, p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a, tol);
			bad++;
		}
	r->checked += (uint64_t)w * h;
	r->mismatches += bad;
	if (bad) {
		if (v->dump)
			dump_images(v, what, px, w, h, tol, fn, ctx);
		snprintf(r->detail, sizeof(r->detail), "%s: %" PRIu64 " of %" PRIu64 " pixels wrong",
			 what, bad, (uint64_t)w * h);
		return R_MISMATCH;
	}
	return R_PASS;
}

enum result check_rgba(struct vk *v, struct report *r, const char *what, const uint8_t *px,
		       uint32_t w, uint32_t h, expect_fn fn, const void *ctx)
{
	return check_rgba_step(v, r, what, px, w, h, 1, fn, ctx);
}
