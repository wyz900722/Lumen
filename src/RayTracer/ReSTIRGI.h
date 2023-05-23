#pragma once
#include "Integrator.h"
class ReSTIRGI : public Integrator {
   public:
	ReSTIRGI(LumenInstance* scene, GltfScene* lumen_scene) : Integrator(scene, lumen_scene) {}
	virtual void init() override;
	virtual void render() override;
	virtual bool update() override;
	virtual void destroy() override;

   private:
	Buffer restir_samples_buffer;
	Buffer restir_samples_old_buffer;
	Buffer temporal_reservoir_buffer;
	Buffer spatial_reservoir_buffer;
	Buffer tmp_col_buffer;
	PushConstantRay pc_ray{};
	bool do_spatiotemporal = false;
};
