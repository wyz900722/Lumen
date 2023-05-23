#pragma once
#include "Integrator.h"
class BDPT : public Integrator {
   public:
	BDPT(LumenInstance* scene, GltfScene* lumen_scene) : Integrator(scene, lumen_scene) {}
	virtual void init() override;
	virtual void render() override;
	virtual bool update() override;
	virtual void destroy() override;

   private:
	PushConstantRay pc_ray{};
	Buffer light_path_buffer;
	Buffer camera_path_buffer;
	Buffer color_storage_buffer;
};
