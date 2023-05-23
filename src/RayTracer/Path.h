#pragma once
#include "Integrator.h"
class Path : public Integrator {
   public:
	Path(LumenInstance* scene, GltfScene* lumen_scene) : Integrator(scene, lumen_scene) {}
	virtual void init() override;
	virtual void render() override;
	virtual bool update() override;
	virtual void destroy() override;

   private:
	PushConstantRay pc_ray{};
};
