#include "LumenPCH.h"
#include <regex>
#include <stb_image.h>
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYOBJLOADER_IMPLEMENTATION
#include "RayTracer.h"
#include <complex>
#include <tinyexr.h>

RayTracer* RayTracer::instance = nullptr;
bool load_reference = false;
bool calc_rmse = false;

static void fb_resize_callback(GLFWwindow* window, int width, int height) {
	auto app = reinterpret_cast<RayTracer*>(glfwGetWindowUserPointer(window));
	app->resized = true;
}

RayTracer::RayTracer(int width, int height, bool debug, int argc, char* argv[]) : LumenInstance(width, height, debug) {
	this->instance = this;
	parse_args(argc, argv);
}

void RayTracer::init(Window* window) {
	// srand((uint32_t)time(NULL));
	srand(42);
	this->window = window;
	vkb.ctx.window_ptr = window->get_window_ptr();
	glfwSetFramebufferSizeCallback(vkb.ctx.window_ptr, fb_resize_callback);
	// Init with ray tracing extensions
	vkb.add_device_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
	vkb.add_device_extension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
	vkb.add_device_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
	vkb.add_device_extension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
	vkb.add_device_extension(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
	vkb.add_device_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	vkb.add_device_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	vkb.add_device_extension(VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME);

	vkb.create_instance();
	if (vkb.enable_validation_layers) {
		vkb.setup_debug_messenger();
	}
	vkb.create_surface();
	vkb.pick_physical_device();
	vkb.create_logical_device();
	vkb.create_swapchain();
	vkb.create_command_pools();
	vkb.create_command_buffers();
	vkb.create_sync_primitives();
	vkb.init_imgui();
	initialized = true;

	//scene.load_scene(scene_name);

    tinygltf::Model tmodel;
	tinygltf::TinyGLTF tcontext;
	std::string warn, error;
	if (!tcontext.LoadASCIIFromFile(&tmodel, &error, &warn, scene_name)) {
		assert(!"Error while loading scene");
	}

	{
		scene.import_materials(tmodel);
		scene.import_drawable_nodes(tmodel, GltfAttributes::Normal | GltfAttributes::Texcoord_0 |
												GltfAttributes::Tangent | GltfAttributes::Color_0);
	}

	// Enable shader reflections for the render graph
	vkb.rg->settings.shader_inference = true;
	// Disable event based synchronization
	// Currently the event API that comes with Vulkan 1.3 is buggy on NVIDIA drivers
	// so this is turned off and pipeline barriers are used instead
	vkb.rg->settings.use_events = true;

	//switch (scene.config.integrator_type) {
	//	case IntegratorType::Path:
			integrator = std::make_unique<Path>(this, &scene);
	//		break;
	//	case IntegratorType::BDPT:
	//		integrator = std::make_unique<BDPT>(this, &scene);
	//		break;
	//	case IntegratorType::SPPM:
	//		integrator = std::make_unique<SPPM>(this, &scene);
	//		break;
	//	case IntegratorType::VCM:
	//		integrator = std::make_unique<VCM>(this, &scene);
	//		break;
	//	case IntegratorType::ReSTIR:
	//		integrator = std::make_unique<ReSTIR>(this, &scene);
	//		break;
	//	case IntegratorType::ReSTIRGI:
	//		integrator = std::make_unique<ReSTIRGI>(this, &scene);
	//		break;
	//	case IntegratorType::PSSMLT:
	//		integrator = std::make_unique<PSSMLT>(this, &scene);
	//		break;
	//	case IntegratorType::SMLT:
	//		integrator = std::make_unique<SMLT>(this, &scene);
	//		break;
	//	case IntegratorType::VCMMLT:
	//		integrator = std::make_unique<VCMMLT>(this, &scene);
	//		break;
	//	case IntegratorType::DDGI:
	//		integrator = std::make_unique<DDGI>(this, &scene);
	//		break;
	//	default:
	//		break;
	//}
	integrator->init();
	post_fx.init(*instance);
	init_resources();
	LUMEN_TRACE("Memory usage {} MB",  get_memory_usage(vk_ctx.physical_device) * 1e-6);
}

void RayTracer::init_resources() {
	RTUtilsDesc desc;
	output_img_buffer.create("Output Image Buffer", &instance->vkb.ctx,
							 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
								 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
							 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE,
							 instance->width * instance->height * 4 * 4);

	output_img_buffer_cpu.create("Output Image CPU", &instance->vkb.ctx,
								 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
								 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
								 VK_SHARING_MODE_EXCLUSIVE, instance->width * instance->height * 4 * 4);
	residual_buffer.create("RMSE Residual", &instance->vkb.ctx,
						   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
							   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE,
						   instance->width * instance->height * 4);

	counter_buffer.create("RMSE Counter", &instance->vkb.ctx,
						  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
							  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, sizeof(int));

	rmse_val_buffer.create("RMSE Value", &instance->vkb.ctx,
						   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
							   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
						   VK_SHARING_MODE_EXCLUSIVE, sizeof(float));

	if (load_reference) {
		// Load the ground truth image
		int width, height;
		float* data = load_exr("out.exr", width, height);
		if (!data) {
			LUMEN_ERROR("Could not load the reference image");
		}
		auto gt_size = width * height * 4 * sizeof(float);
		gt_img_buffer.create("Ground Truth Image", &instance->vkb.ctx,
							 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
							 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, gt_size, data, true);
		desc.gt_img_addr = gt_img_buffer.get_device_address();
		free(data);
	}

	desc.out_img_addr = output_img_buffer.get_device_address();
	desc.residual_addr = residual_buffer.get_device_address();
	desc.counter_addr = counter_buffer.get_device_address();
	desc.rmse_val_addr = rmse_val_buffer.get_device_address();

	REGISTER_BUFFER_WITH_ADDRESS(RTUtilsDesc, desc, out_img_addr, &output_img_buffer, instance->vkb.rg);
	REGISTER_BUFFER_WITH_ADDRESS(RTUtilsDesc, desc, residual_addr, &residual_buffer, instance->vkb.rg);
	REGISTER_BUFFER_WITH_ADDRESS(RTUtilsDesc, desc, counter_addr, &counter_buffer, instance->vkb.rg);
	REGISTER_BUFFER_WITH_ADDRESS(RTUtilsDesc, desc, rmse_val_addr, &rmse_val_buffer, instance->vkb.rg);
}

void RayTracer::update() {
	if (instance->window->is_key_down(KeyInput::KEY_F10)) {
		write_exr = true;
	}
	float frame_time = draw_frame();
	cpu_avg_time = (1.0f - 1.0f / (cnt)) * cpu_avg_time + frame_time / (float)cnt;
	cpu_avg_time = 0.95f * cpu_avg_time + 0.05f * frame_time;
	integrator->update();
}

void RayTracer::render(uint32_t i) {
	integrator->render();
	post_fx.render(integrator->output_tex, vkb.swapchain_images[i]);
	auto cmdbuf = vkb.ctx.command_buffers[i];
	VkCommandBufferBeginInfo begin_info = vk::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	vk::check(vkBeginCommandBuffer(cmdbuf, &begin_info));

	if (write_exr) {
		instance->vkb.rg->current_pass().copy(integrator->output_tex, output_img_buffer_cpu);
	}
	if (calc_rmse && has_gt) {
		auto op_reduce = [&](const std::string& op_name, const std::string& op_shader_name,
							 const std::string& reduce_name, const std::string& reduce_shader_name) {
			uint32_t num_wgs = uint32_t((instance->width * instance->height + 1023) / 1024);
			instance->vkb.rg->add_compute(op_name, {.shader = Shader(op_shader_name), .dims = {num_wgs, 1, 1}})
				.push_constants(&rt_utils_pc)
				.bind(rt_utils_desc_buffer)
				.zero({residual_buffer, counter_buffer});
			while (num_wgs != 1) {
				instance->vkb.rg
					->add_compute(reduce_name, {.shader = Shader(reduce_shader_name), .dims = {num_wgs, 1, 1}})
					.push_constants(&rt_utils_pc)
					.bind(rt_utils_desc_buffer);
				num_wgs = (num_wgs + 1023) / 1024;
			}
		};
		instance->vkb.rg->current_pass().copy(integrator->output_tex, output_img_buffer);
		// Calculate RMSE
		op_reduce("OpReduce: RMSE", "src/shaders/rmse/calc_rmse.comp", "OpReduce: Reduce RMSE",
				  "src/shaders/rmse/reduce_rmse.comp");
		instance->vkb.rg
			->add_compute("Calculate RMSE", {.shader = Shader("src/shaders/rmse/output_rmse.comp"), .dims = {1, 1, 1}})
			.push_constants(&rt_utils_pc)
			.bind(rt_utils_desc_buffer);
	}

	vkb.rg->run(cmdbuf);

	vk::check(vkEndCommandBuffer(cmdbuf), "Failed to record command buffer");
}

float RayTracer::draw_frame() {
	if (cnt == 0) {
		start = clock();
	}
	auto t_begin = glfwGetTime() * 1000;
	bool updated = false;
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
	ImGui::Text("Frame time %f ms ( %f FPS )", cpu_avg_time, 1000 / cpu_avg_time);
	ImGui::Text("Memory Usage: %f MB", get_memory_usage(vk_ctx.physical_device) * 1e-6);

	bool gui_updated = integrator->gui();
	gui_updated |= post_fx.gui();
	ImGui::Checkbox("Show camera statistics", &show_cam_stats);
	if (show_cam_stats) {
		ImGui::PushItemWidth(170);
		ImGui::DragFloat4("", glm::value_ptr(integrator->camera->camera[0]), 0.05f);
		ImGui::DragFloat4("", glm::value_ptr(integrator->camera->camera[1]), 0.05f);
		ImGui::DragFloat4("", glm::value_ptr(integrator->camera->camera[2]), 0.05f);
		ImGui::DragFloat4("", glm::value_ptr(integrator->camera->camera[3]), 0.05f);
	}
	if (ImGui::Button("Reload shaders")) {
		vkb.rg->reload_shaders = true;
		vkb.rg->shader_cache.clear();
		updated |= true;
	}

	if (updated || gui_updated) {
		ImGui::Render();
		auto t_end = glfwGetTime() * 1000;
		auto t_diff = t_end - t_begin;
		integrator->updated = true;
		return (float)t_diff;
	}

	uint32_t image_idx = vkb.prepare_frame();

	if (image_idx == UINT32_MAX) {
		auto t_end = glfwGetTime() * 1000;
		auto t_diff = t_end - t_begin;
		return (float)t_diff;
	}
	render(image_idx);
	vkb.submit_frame(image_idx, resized);
	vkb.rg->reset(vkb.ctx.command_buffers[image_idx]);

	auto now = clock();
	auto diff = ((float)now - start);

	if (write_exr) {
		write_exr = false;
		save_exr((float*)output_img_buffer_cpu.data, instance->width, instance->height, "out.exr");
	}
	bool time_limit = (abs(diff / CLOCKS_PER_SEC - 5)) < 0.1;
	calc_rmse = time_limit;

	if (calc_rmse && has_gt) {
		float rmse = *(float*)rmse_val_buffer.data;
		LUMEN_TRACE("RMSE {}", rmse * 1e6);
		start = now;
	}
	auto t_end = glfwGetTime() * 1000;
	auto t_diff = t_end - t_begin;
	cnt++;
	return (float)t_diff;
}

void RayTracer::parse_args(int argc, char* argv[]) {
	//scene_name = "scenes/caustics.json";
	//std::regex fn("(.*).(.json|.xml)");
	//for (int i = 0; i < argc; i++) {
	//	if (std::regex_match(argv[i], fn)) {
	//		scene_name = argv[i];
	//	}
	//}
	// scene_name = "D:\\wyz\\raytrace\\downloaded_resources\\robot_toon\\robot-toon.gltf";
	//scene_name = "D:\\wyz\\models\\bieshu\\main.gltf";
	scene_name = "D:\\wyz\\models\\demo\\887053776094010679.skp.gltf";
	//scene_name = "D:\\wyz\\models\\sheji\\887053776094007546.skp.gltf";
}
void RayTracer::cleanup() {
	const auto device = vkb.ctx.device;
	vkDeviceWaitIdle(device);
	if (initialized) {
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		std::vector<Buffer*> buffer_list = {&output_img_buffer, &output_img_buffer_cpu, &residual_buffer,
											&counter_buffer,	&rmse_val_buffer,		&rt_utils_desc_buffer};
		if (load_reference) {
			buffer_list.push_back(&gt_img_buffer);
		}
		for (auto b : buffer_list) {
			b->destroy();
		}
		integrator->destroy();
		post_fx.destroy();
		vkb.cleanup();
	}
}
