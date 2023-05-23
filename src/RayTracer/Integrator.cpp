#include "LumenPCH.h"
#include "Integrator.h"
#include <stb_image.h>

void Integrator::init() {
	VkPhysicalDeviceProperties2 prop2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	prop2.pNext = &rt_props;
	vkGetPhysicalDeviceProperties2(instance->vkb.ctx.physical_device, &prop2);
	constexpr int VERTEX_BINDING_ID = 0;

	LumenInstance* instance = this->instance;
	Window* window = instance->window;

    glm::vec3 camDir(-1.0f, 0.0f, 0.0f);
	//if (lumen_scene->cameras.at(0).eye != vec3(0)) {
		camera = std::unique_ptr<PerspectiveCamera>(new PerspectiveCamera(
			60.0f, 0.01f, 1000.0f, (float)instance->width / instance->height,
			camDir, glm::vec3(-4.73f, -21.5f, 24.8f)));
	//} else {
	//	// Assume the camera matrix is given
	//	camera = std::unique_ptr<PerspectiveCamera>(
	//		new PerspectiveCamera(lumen_scene->config.cam_settings.fov, lumen_scene->config.cam_settings.cam_matrix,
	//							  0.01f, 1000.0f, (float)instance->width / instance->height));
	//}

    camera->rotation = vec3(51.6f, -1.4f, 0.0f);

	Camera* cam_ptr = camera.get();
	instance->window->add_mouse_click_callback([cam_ptr, this, window](MouseAction button, KeyAction action) {
		if (updated && window->is_mouse_up(MouseAction::LEFT)) {
			updated = true;
		}
		if (updated && window->is_mouse_down(MouseAction::LEFT)) {
			updated = true;
		}
	});
	instance->window->add_mouse_move_callback([window, cam_ptr, this](double delta_x, double delta_y) {
		if (window->is_mouse_held(MouseAction::LEFT)) {
			cam_ptr->rotate(0.05f * (float)delta_y, -0.05f * (float)delta_x, 0.0f);
			// pc_ray.frame_num = -1;
			updated = true;
		}
	});
	auto vertex_buf_size = lumen_scene->positions.size() * sizeof(glm::vec3);
	auto idx_buf_size = lumen_scene->indices.size() * sizeof(uint32_t);
	std::vector<PrimMeshInfo> prim_lookup;
	uint32_t idx = 0;
	for (auto& pm : lumen_scene->prim_meshes) {
		PrimMeshInfo m_info;
		m_info.index_offset = pm.first_idx;
		m_info.vertex_offset = pm.vtx_offset;
		m_info.material_index = pm.material_idx;
		m_info.min_pos = glm::vec4(pm.pos_min, 0);
		m_info.max_pos = glm::vec4(pm.pos_max, 0);
		m_info.material_index = pm.material_idx;
		prim_lookup.emplace_back(m_info);
		auto& mef = lumen_scene->materials[pm.material_idx].emissive_factor;
		if (mef.x > 0 || mef.y > 0 || mef.z > 0) {
			Light light;
			//light.world_matrix = pm.world_matrix;
			light.num_triangles = pm.idx_count / 3;
			light.prim_mesh_idx = idx;
			light.light_flags = LIGHT_AREA;
			// Is finite
			light.light_flags |= 1 << 4;
			auto a = ((light.light_flags >> 4) & 0x1) != 0;
			;
			lights.emplace_back(light);
			total_light_triangle_cnt += light.num_triangles;
		}
		idx++;
	}

	//for (auto& l : lumen_scene->lights) {
	Light light;
	light.L = vec3(1.0f);
	light.light_flags = 35;
	light.pos = vec3(1,1,1);
	light.to = vec3(0,0,0);
	total_light_triangle_cnt++;
	light.world_radius = lumen_scene->m_dimensions.radius;
	light.world_center = 0.5f * (lumen_scene->m_dimensions.max + lumen_scene->m_dimensions.min);
	lights.emplace_back(light);
	light.light_flags = 49;
	light.pos = vec3(1, 1, 1);
	light.to = vec3(0, 0, 0);
	total_light_triangle_cnt++;
	lights.emplace_back(light);
	//}

    std::vector<Material> materials;
	materials.resize(lumen_scene->materials.size());
	int matIdx = 0;
    for (auto mat : lumen_scene->materials) 
    {
		auto& material = materials.at(matIdx);
		material.albedo = vec3(mat.base_color_factor);
		material.emissive_factor = mat.emissive_factor;
		material.bsdf_type = BSDF_DIFFUSE;
		material.bsdf_props = BSDF_OPAQUE | BSDF_LAMBERTIAN;
		material.texture_id = mat.base_color_texture;
		//material.metalness = mat.metallic_factor;
		material.roughness = mat.roughness_factor;
		matIdx++;
    }

	scene_ubo_buffer.create("Scene UBO", &instance->vkb.ctx, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
							VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
							VK_SHARING_MODE_EXCLUSIVE, sizeof(SceneUBO));
	update_uniform_buffers();

	vertex_buffer.create("Vertex Buffer", &instance->vkb.ctx,
						 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
							 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
							 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
						 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, vertex_buf_size,
						 lumen_scene->positions.data(), true);
	index_buffer.create("Index Buffer", &instance->vkb.ctx,
						VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
							VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
							VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
						VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE, idx_buf_size,
						lumen_scene->indices.data(), true);

	normal_buffer.create("Normal Buffer", &instance->vkb.ctx,
						 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
							 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
						 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE,
						 lumen_scene->normals.size() * sizeof(lumen_scene->normals[0]), lumen_scene->normals.data(),
						 true);
	uv_buffer.create("UV Buffer", &instance->vkb.ctx,
					 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
						 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
					 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE,
					 lumen_scene->texcoords0.size() * sizeof(glm::vec2), lumen_scene->texcoords0.data(), true);
	materials_buffer.create("Materials Buffer", &instance->vkb.ctx,
							VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
							VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE,
							materials.size() * sizeof(Material), materials.data(), true);
	prim_lookup_buffer.create("Prim Lookup Buffer", &instance->vkb.ctx,
							  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
							  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE,
							  prim_lookup.size() * sizeof(PrimMeshInfo), prim_lookup.data(), true);

	// Create a sampler for textures
	VkSamplerCreateInfo sampler_ci = vk::sampler_create_info();
	sampler_ci.minFilter = VK_FILTER_LINEAR;
	sampler_ci.magFilter = VK_FILTER_LINEAR;
	sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler_ci.maxLod = FLT_MAX;
	vk::check(vkCreateSampler(instance->vkb.ctx.device, &sampler_ci, nullptr, &texture_sampler),
			  "Could not create image sampler");

	auto add_default_texture = [this, instance]() {
		std::array<uint8_t, 4> nil = {0, 0, 0, 0};
		scene_textures.resize(1);
		auto ci = make_img2d_ci(VkExtent2D{1, 1});
		scene_textures[0].load_from_data(&instance->vkb.ctx, nil.data(), 4, ci, texture_sampler, VK_IMAGE_USAGE_SAMPLED_BIT, false);
	};

    auto gltfModel = lumen_scene->tModel;
	if (!gltfModel.images.size()) {
		add_default_texture();
	} else {
		//scene_textures.resize(gltfModel.images.size());
		if (gltfModel.images.size() > 28)
			scene_textures.resize(28);
		else
			scene_textures.resize(gltfModel.images.size());

		int i = 0;
		for (auto& img : gltfModel.images) {
			if (i > 27) break;
			void* data = &img.image[0];
			VkDeviceSize size = img.image.size();
			auto img_dims = VkExtent2D{(uint32_t)img.width, (uint32_t)img.height};
			auto ci = make_img2d_ci(img_dims, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT, false);
			scene_textures[i].load_from_data(&instance->vkb.ctx, data, size, ci, texture_sampler, false);
			i++;
		}
	}
	// Create BLAS and TLAS
	create_blas();
	create_tlas();
	// Create offscreen image for output
	TextureSettings settings;
	settings.usage_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
						   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
						   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	settings.base_extent = {(uint32_t)instance->width, (uint32_t)instance->height, 1};
	settings.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	output_tex.create_empty_texture("Color Output", &instance->vkb.ctx, settings, VK_IMAGE_LAYOUT_GENERAL);
}

bool Integrator::gui() {
	//ImGui::Text("Path length: %d", lumen_scene->config.path_length);
	//ImGui::Text("Integrator: %s", lumen_scene->config.integrator_name.c_str());
	if (lights.size() > 0) {
		auto& light = lights.at(0);
		float color[3] = {light.L.x / lightColorIntensity, light.L.y / lightColorIntensity, light.L.z / lightColorIntensity};
		ImGui::ColorEdit3("Light Color", color);
		ImGui::SliderFloat("Light Color Intensity", &lightColorIntensity, 0.0, 50.0);
		light.L = vec3(color[0], color[1], color[2]) * lightColorIntensity;

		float pos[3] = {light.pos.x, light.pos.y, light.pos.z};
		ImGui::DragFloat3("Light Pos", pos);
		light.pos = vec3(pos[0], pos[1], pos[2]);

		float to[3] = {light.to.x, light.to.y, light.to.z};
		ImGui::DragFloat3("Light To", to);
		light.to = vec3(to[0], to[1], to[2]);
    }
	return false;
}

void Integrator::create_blas() {
	std::vector<BlasInput> blas_inputs;
	auto vertex_address = get_device_address(instance->vkb.ctx.device, vertex_buffer.handle);
	auto idx_address = get_device_address(instance->vkb.ctx.device, index_buffer.handle);
	for (auto& prim_mesh : lumen_scene->prim_meshes) {
		BlasInput geo = to_vk_geometry(prim_mesh, vertex_address, idx_address);
		blas_inputs.push_back({geo});
	}
	instance->vkb.build_blas(blas_inputs, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
}

void Integrator::create_tlas() {
	std::vector<VkAccelerationStructureInstanceKHR> tlas;
	float total_light_triangle_area = 0.0f;
	// int light_triangle_cnt = 0;
	const auto& indices = lumen_scene->indices;
	const auto& vertices = lumen_scene->positions;
	for (const auto& pm : lumen_scene->nodes) {
		VkAccelerationStructureInstanceKHR ray_inst{};
		ray_inst.transform = to_vk_matrix(pm.world_matrix);
		ray_inst.instanceCustomIndex = pm.prim_mesh;
		ray_inst.accelerationStructureReference = instance->vkb.get_blas_device_address(pm.prim_mesh);
		ray_inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		ray_inst.mask = 0xFF;
		ray_inst.instanceShaderBindingTableRecordOffset = 0;  // We will use the same hit group for all objects
		tlas.emplace_back(ray_inst);
	}

	for (auto& l : lights) {
		if (l.light_flags == LIGHT_AREA) {
			const auto& pm = lumen_scene->prim_meshes[l.prim_mesh_idx];
			glm::mat4 world_matrix(1.0);
			l.world_matrix = world_matrix;
			auto& idx_base_offset = pm.first_idx;
			auto& vtx_offset = pm.vtx_offset;
			for (uint32_t i = 0; i < l.num_triangles; i++) {
				auto idx_offset = idx_base_offset + 3 * i;
				glm::ivec3 ind = {indices[idx_offset], indices[idx_offset + 1], indices[idx_offset + 2]};
				ind += glm::vec3{vtx_offset, vtx_offset, vtx_offset};
				const vec3 v0 = world_matrix * glm::vec4(vertices[ind.x], 1.0);
				const vec3 v1 = world_matrix * glm::vec4(vertices[ind.y], 1.0);
				const vec3 v2 = world_matrix * glm::vec4(vertices[ind.z], 1.0);
				float area = 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
				total_light_triangle_area += area;
			}
		}
	}
	if (lights.size()) {
		//mesh_lights_buffer.create(&instance->vkb.ctx, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		//						  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_SHARING_MODE_EXCLUSIVE,
		//						  lights.size() * sizeof(Light), lights.data(), true);
		mesh_lights_buffer.create("Scene UBO", &instance->vkb.ctx, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
								VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
								VK_SHARING_MODE_EXCLUSIVE, lights.size() * sizeof(Light), lights.data(), true);
	}

	total_light_area += total_light_triangle_area;

	instance->vkb.build_tlas(tlas, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
}

void Integrator::update_uniform_buffers() {
	camera->update_view_matrix();
	scene_ubo.view = camera->view;
	scene_ubo.projection = camera->projection;
	scene_ubo.view_pos = glm::vec4(camera->position, 1);
	scene_ubo.inv_view = glm::inverse(camera->view);
	scene_ubo.inv_projection = glm::inverse(camera->projection);
	scene_ubo.model = glm::mat4(1.0);
	scene_ubo.light_pos = glm::vec4(3.0f, 2.5f, 1.0f, 1.0f);
	memcpy(scene_ubo_buffer.data, &scene_ubo, sizeof(scene_ubo));

    if (mesh_lights_buffer.handle)
        memcpy(mesh_lights_buffer.data, lights.data(), lights.size() * sizeof(Light));
}

bool Integrator::update() {
	glm::vec3 translation{};
	float trans_speed = 0.01f;
	glm::vec3 front;
	if (instance->window->is_key_held(KeyInput::KEY_LEFT_SHIFT)) {
		trans_speed *= 4;
	}

	front.x = cos(glm::radians(camera->rotation.x)) * sin(glm::radians(camera->rotation.y));
	front.y = sin(glm::radians(camera->rotation.x));
	front.z = cos(glm::radians(camera->rotation.x)) * cos(glm::radians(camera->rotation.y));
	front = glm::normalize(-front);
	if (instance->window->is_key_held(KeyInput::KEY_W)) {
		camera->position += front * trans_speed;
		updated = true;
	}
	if (instance->window->is_key_held(KeyInput::KEY_A)) {
		camera->position -= glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f))) * trans_speed;
		updated = true;
	}
	if (instance->window->is_key_held(KeyInput::KEY_S)) {
		camera->position -= front * trans_speed;
		updated = true;
	}
	if (instance->window->is_key_held(KeyInput::KEY_D)) {
		camera->position += glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f))) * trans_speed;
		updated = true;
	}
	if (instance->window->is_key_held(KeyInput::SPACE) || instance->window->is_key_held(KeyInput::KEY_E)) {
		// Right
		auto right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
		auto up = glm::cross(right, front);
		camera->position += up * trans_speed;
		updated = true;
	}
	if (instance->window->is_key_held(KeyInput::KEY_LEFT_CONTROL) || instance->window->is_key_held(KeyInput::KEY_Q)) {
		auto right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
		auto up = glm::cross(right, front);
		camera->position -= up * trans_speed;
		updated = true;
	}
	bool result = false;
	if (updated) {
		result = true;
		updated = false;
	}
	update_uniform_buffers();
	return result;
}

void Integrator::destroy() {
	std::vector<Buffer*> buffer_list = {&vertex_buffer,	   &normal_buffer,		&uv_buffer,			&index_buffer,
										&materials_buffer, &prim_lookup_buffer, &scene_desc_buffer, &scene_ubo_buffer};
	if (lights.size()) {
		buffer_list.push_back(&mesh_lights_buffer);
	}
	for (auto b : buffer_list) {
		b->destroy();
	}
	output_tex.destroy();
	for (auto& tex : scene_textures) {
		tex.destroy();
	}
	vkDestroySampler(instance->vkb.ctx.device, texture_sampler, nullptr);
}