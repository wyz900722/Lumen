#ifndef PSSMLT_UTILS
#define PSSMLT_UTILS
#include "../../commons.glsl"
layout(push_constant) uniform _PushConstantRay { PushConstantRay pc_ray; };
layout(constant_id = 0) const int SEEDING = 0;
layout(buffer_reference, scalar) buffer BootstrapData { BootstrapSample d[]; };
layout(buffer_reference, scalar) buffer SeedsData { VCMMLTSeedData d[]; };
layout(buffer_reference, scalar) buffer PrimarySamples { PrimarySample d[]; };
layout(buffer_reference, scalar) buffer MLTSamplers { VCMMLTSampler d[]; };
layout(buffer_reference, scalar) buffer MLTColor { vec3 d[]; };
layout(buffer_reference, scalar) buffer ChainStats { ChainData d[]; };
layout(buffer_reference, scalar) buffer Splats { Splat d[]; };
layout(buffer_reference, scalar) buffer LightVertices { VCMVertex d[]; };
layout(buffer_reference, scalar) buffer CameraVertices { VCMVertex d[]; };
layout(buffer_reference, scalar) buffer PathCnt { uint d[]; };
layout(buffer_reference, scalar) buffer ColorStorages { vec3 d[]; };

uint chain = 0;
uint depth_factor = pc_ray.max_depth * (pc_ray.max_depth + 1);

LightVertices light_verts = LightVertices(scene_desc.vcm_vertices_addr);
MLTSamplers mlt_samplers = MLTSamplers(scene_desc.mlt_samplers_addr);
MLTColor mlt_col = MLTColor(scene_desc.mlt_col_addr);
ChainStats chain_stats = ChainStats(scene_desc.chain_stats_addr);
Splats splat_data = Splats(scene_desc.splat_addr);
Splats past_splat_data = Splats(scene_desc.past_splat_addr);
BootstrapData bootstrap_data = BootstrapData(scene_desc.bootstrap_addr);
SeedsData seeds_data = SeedsData(scene_desc.seeds_addr);
PrimarySamples light_primary_samples =
    PrimarySamples(scene_desc.light_primary_samples_addr);
PrimarySamples cam_primary_samples =
    PrimarySamples(scene_desc.cam_primary_samples_addr);
PrimarySamples prim_samples[2] =
    PrimarySamples[](light_primary_samples, cam_primary_samples);
ColorStorages tmp_col = ColorStorages(scene_desc.color_storage_addr);
const uint flags = gl_RayFlagsOpaqueEXT;
const float tmin = 0.001;
const float tmax = 10000.0;
#define RR_MIN_DEPTH 3
uvec4 seed = init_rng(gl_LaunchIDEXT.xy, gl_LaunchSizeEXT.xy,
                      pc_ray.frame_num ^ pc_ray.random_num);
uint screen_size = gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y;
uint pixel_idx = (gl_LaunchIDEXT.x * gl_LaunchSizeEXT.y + gl_LaunchIDEXT.y);
uint splat_idx = (gl_LaunchIDEXT.x * gl_LaunchSizeEXT.y + gl_LaunchIDEXT.y) *
                 2 * ((pc_ray.max_depth * (pc_ray.max_depth + 1)));
uint vcm_light_path_idx =
    (gl_LaunchIDEXT.x * gl_LaunchSizeEXT.y + gl_LaunchIDEXT.y) *
    (pc_ray.max_depth);
uint mlt_sampler_idx = pixel_idx * 2;
uint light_primary_sample_idx =
    (gl_LaunchIDEXT.x * gl_LaunchSizeEXT.y + gl_LaunchIDEXT.y) *
    pc_ray.light_rand_count * 2;
uint cam_primary_sample_idx =
    (gl_LaunchIDEXT.x * gl_LaunchSizeEXT.y + gl_LaunchIDEXT.y) *
    pc_ray.cam_rand_count;
uint prim_sample_idxs[2] =
    uint[](light_primary_sample_idx, cam_primary_sample_idx);

PathCnt path_cnts = PathCnt(scene_desc.path_cnt_addr);

#define mlt_sampler mlt_samplers.d[mlt_sampler_idx + chain]
#define primary_sample(i)                                                      \
    light_primary_samples                                                      \
        .d[light_primary_sample_idx + chain * pc_ray.light_rand_count + i]

uint mlt_get_next() { return mlt_sampler.num_light_samples++; }

uint mlt_get_sample_count() { return mlt_sampler.num_light_samples; }

void mlt_start_iteration() { mlt_sampler.iter++; }

void mlt_start_chain() { mlt_sampler.num_light_samples = 0; }

float mlt_rand(inout uvec4 seed, bool large_step) {
    if (SEEDING == 1) {
        return rand(seed);
    }
    const uint cnt = mlt_get_next();
    const float sigma = 0.01;
    if (primary_sample(cnt).last_modified < mlt_sampler.last_large_step) {
        primary_sample(cnt).val = rand(seed);
        primary_sample(cnt).last_modified = mlt_sampler.last_large_step;
    }
    // Backup current sample
    primary_sample(cnt).backup = primary_sample(cnt).val;
    primary_sample(cnt).last_modified_backup =
        primary_sample(cnt).last_modified;
    if (large_step) {
        primary_sample(cnt).val = rand(seed);
    } else {
        uint diff = mlt_sampler.iter - primary_sample(cnt).last_modified;
        float nrm_sample = sqrt2 * erf_inv(2 * rand(seed) - 1);
        float eff_sigma = sigma * sqrt(float(diff));
        primary_sample(cnt).val += nrm_sample * eff_sigma;
        primary_sample(cnt).val -= floor(primary_sample(cnt).val);
    }
    primary_sample(cnt).last_modified = mlt_sampler.iter;
    return primary_sample(cnt).val;
}

void mlt_accept(bool large_step) {

    if (large_step) {
        mlt_sampler.last_large_step = mlt_sampler.iter;
    }
}
void mlt_reject() {
    const uint cnt = mlt_get_sample_count();
    for (int i = 0; i < cnt; i++) {
        // Restore
        if (primary_sample(i).last_modified == mlt_sampler.iter) {
            primary_sample(i).val = primary_sample(i).backup;
            primary_sample(i).last_modified =
                primary_sample(i).last_modified_backup;
        }
    }
    mlt_sampler.iter--;
}

float eval_target(float lum, uint c) { return c == 0 ? float(lum > 0) : lum; }

float mlt_mis(float lum, float target, uint c) {
    const float num = target / chain_stats.d[c].normalization;
    const float denum = 1. / chain_stats.d[0].normalization +
                        lum / chain_stats.d[1].normalization;
    return num / denum;
}


vec3 vcm_connect_cam(const vec3 cam_pos, const vec3 cam_nrm, const vec3 nrm,
                     const float cam_A, const vec3 pos, const in VCMState state,
                     const vec3 wo, const MaterialProps mat, out ivec2 coords) {
    vec3 L = vec3(0);
    vec3 dir = cam_pos - pos;
    float len = length(dir);
    dir /= len;
    float cos_y = dot(dir, nrm);
    float cos_theta = dot(cam_nrm, -dir);
    if (cos_theta <= 0.) {
        return L;
    }

    float cos_3_theta = cos_theta * cos_theta * cos_theta;
    const float cam_pdf_ratio = abs(cos_y) / (cam_A * cos_3_theta * len * len);
    vec3 ray_origin = offset_ray(pos, nrm);
    float pdf_rev, pdf_fwd;
    const vec3 f = eval_bsdf(nrm, wo, mat, 0, dot(payload.shading_nrm, wo) > 0,
                             dir, pdf_fwd, pdf_rev, cos_y);
    if (f == vec3(0)) {
        return L;
    }
    if (cam_pdf_ratio > 0.0) {
        any_hit_payload.hit = 1;
        traceRayEXT(tlas,
                    gl_RayFlagsTerminateOnFirstHitEXT |
                        gl_RayFlagsSkipClosestHitShaderEXT,
                    0xFF, 1, 0, 1, ray_origin, 0, dir, len - EPS, 1);
        if (any_hit_payload.hit == 0) {
            const float w_light = (cam_pdf_ratio / screen_size) *
                                  (state.d_vcm + pdf_rev * state.d_vc);
            const float mis_weight = 1. / (1. + w_light);
            L = mis_weight * state.throughput * cam_pdf_ratio * f / screen_size;
        }
    }
    dir = -dir;
    vec4 target = ubo.view * vec4(dir.x, dir.y, dir.z, 0);
    target /= target.z;
    target = -ubo.projection * target;
    vec2 screen_dims = vec2(pc_ray.size_x, pc_ray.size_y);
    coords = ivec2(0.5 * (1 + target.xy) * screen_dims - 0.5);
    if (coords.x < 0 || coords.x >= pc_ray.size_x || coords.y < 0 ||
        coords.y >= pc_ray.size_y || dot(dir, cam_nrm) < 0) {
        return vec3(0);
    }
    return L;
}

bool vcm_generate_light_sample(out VCMState light_state, inout uvec4 seed,
                               bool large_step) {
    // Sample light
    uint light_idx;
    uint light_triangle_idx;
    MeshLight light;
    uint light_material_idx;
    vec2 uv_unused;
    const vec4 rands_pos =
        vec4(mlt_rand(seed, large_step), mlt_rand(seed, large_step),
             mlt_rand(seed, large_step), mlt_rand(seed, large_step));
    const TriangleRecord record =
        sample_area_light(rands_pos, pc_ray.num_mesh_lights, light_idx,
                          light_triangle_idx, light_material_idx, light);
    const MaterialProps light_mat =
        load_material(light_material_idx, uv_unused);
    vec3 wi = sample_cos_hemisphere(
        vec2(mlt_rand(seed, large_step), mlt_rand(seed, large_step)),
        record.triangle_normal);
    float pdf_pos = record.triangle_pdf * (1.0 / pc_ray.light_triangle_count);
    float cos_theta = abs(dot(wi, record.triangle_normal));
    float pdf_dir = cos_theta / PI;
    if (pdf_dir <= EPS) {
        return false;
    }
    light_state.pos = record.pos;
    light_state.shading_nrm = record.triangle_normal;
    light_state.area = 1.0 / record.triangle_pdf;
    light_state.wi = wi;
    light_state.throughput =
        light_mat.emissive_factor * cos_theta / (pdf_dir * pdf_pos);
    light_state.d_vcm = PI / cos_theta;
    light_state.d_vc = cos_theta / (pdf_dir * pdf_pos);
    return true;
}



vec3 vcm_get_light_radiance(in const MaterialProps mat,
                            in const VCMState camera_state, int d) {
    if (d == 2) {
        return mat.emissive_factor;
    }
    const float pdf_light_pos =
        1.0 / (payload.area * pc_ray.light_triangle_count);
    const float pdf_light_dir =
        abs(dot(payload.shading_nrm, -camera_state.wi)) / PI;
    const float w_camera = pdf_light_pos * camera_state.d_vcm +
                           (pdf_light_pos * pdf_light_dir) * camera_state.d_vc;

    const float mis_weight = 1. / (1. + w_camera);
    return mis_weight * mat.emissive_factor;
}

float mlt_fill_eye_path(const vec4 origin, const float cam_area) {
#define cam_vtx(i) light_verts.d[vcm_light_path_idx + i]
    const float fov = ubo.projection[1][1];
    const vec3 cam_pos = origin.xyz;
    vec2 dir = vec2(rand(seed), rand(seed)) * 2.0 - 1.0;
    const vec3 direction = sample_camera(dir).xyz;
    VCMState camera_state;
    float lum_sum = 0;
    // Generate camera sample
    camera_state.wi = direction;
    camera_state.pos = origin.xyz;
    camera_state.throughput = vec3(1.0);
    camera_state.shading_nrm = vec3(-ubo.inv_view * vec4(0, 0, 1, 0));
    float cos_theta = abs(dot(camera_state.shading_nrm, direction));
    // Defer r^2 / cos term
    camera_state.d_vcm =
        cam_area * screen_size * cos_theta * cos_theta * cos_theta;
    camera_state.d_vc = 0;
    camera_state.d_vm = 0;
    int d;
    int path_idx = 0;
    ivec2 coords = ivec2(0.5 * (1 + dir) * vec2(pc_ray.size_x, pc_ray.size_y));
    uint coords_idx = coords.x * pc_ray.size_y + coords.y;
    for (d = 2;; d++) {
        traceRayEXT(tlas, flags, 0xFF, 0, 0, 0, camera_state.pos, tmin,
                    camera_state.wi, tmax, 0);
        if (d >= pc_ray.max_depth + 1) {
            break;
        }
        if (payload.material_idx == -1) {
            // TODO:
            tmp_col.d[coords_idx] += camera_state.throughput * pc_ray.sky_col;
            break;
        }

        vec3 wo = camera_state.pos - payload.pos;
        float dist = length(payload.pos - camera_state.pos);
        float dist_sqr = dist * dist;
        wo /= dist;
        vec3 shading_nrm = payload.shading_nrm;
        float cos_wo = dot(wo, shading_nrm);
        vec3 geometry_nrm = payload.geometry_nrm;
        bool side = true;
        if (dot(payload.geometry_nrm, wo) < 0.)
            geometry_nrm = -geometry_nrm;
        if (cos_wo < 0.) {
            cos_wo = -cos_wo;
            shading_nrm = -shading_nrm;
            side = false;
        }

        const MaterialProps mat =
            load_material(payload.material_idx, payload.uv);
        const bool mat_specular =
            (mat.bsdf_props & BSDF_SPECULAR) == BSDF_SPECULAR;
        // Complete the missing geometry terms
        camera_state.d_vcm *= dist_sqr;
        camera_state.d_vcm /= cos_wo;
        camera_state.d_vc /= cos_wo;
        camera_state.d_vm /= cos_wo;

        // Get the radiance
        if (luminance(mat.emissive_factor) > 0) {
            vec3 L = camera_state.throughput *
                     vcm_get_light_radiance(mat, camera_state, d);
            tmp_col.d[coords_idx] += L;
            lum_sum += luminance(L);
            // if (pc_ray.use_vc == 1 || pc_ray.use_vm == 1) {
            //     // break;
            // }
        }

        // Copy to camera vertex buffer
        if (d > 2) {
            cam_vtx(path_idx).wi = camera_state.wi;
            cam_vtx(path_idx).shading_nrm = camera_state.shading_nrm;
            cam_vtx(path_idx).pos = camera_state.pos;
            cam_vtx(path_idx).uv = camera_state.uv;
            cam_vtx(path_idx).throughput = camera_state.throughput;
            cam_vtx(path_idx).material_idx = camera_state.material_idx;
            cam_vtx(path_idx).area = camera_state.area;
            cam_vtx(path_idx).d_vcm = camera_state.d_vcm;
            cam_vtx(path_idx).d_vc = camera_state.d_vc;
            cam_vtx(path_idx).d_vm = camera_state.d_vm;
            cam_vtx(path_idx).path_len = d;
            cam_vtx(path_idx).side = uint(side);
            cam_vtx(path_idx).coords = coords_idx;
            path_idx++;
        }

        // Connect to light
        float pdf_rev;
        vec3 f;
        if (!mat_specular) {
            uint light_idx;
            uint light_triangle_idx;
            uint light_material_idx;
            vec2 uv_unused;
            MeshLight light;
            const TriangleRecord record = sample_area_light(
                light_idx, light_triangle_idx, light_material_idx, light, seed,
                pc_ray.num_mesh_lights);
            const MaterialProps light_mat =
                load_material(light_material_idx, uv_unused);
            vec3 wi = record.pos - payload.pos;
            float ray_len = length(wi);
            float ray_len_sqr = ray_len * ray_len;
            wi /= ray_len;
            const float cos_x = dot(wi, shading_nrm);
            const vec3 ray_origin = offset_ray(payload.pos, shading_nrm);
            any_hit_payload.hit = 1;
            float pdf_fwd;
            f = eval_bsdf(shading_nrm, wo, mat, 1, side, wi, pdf_fwd, pdf_rev,
                          cos_x);
            if (f != vec3(0)) {
                traceRayEXT(tlas,
                            gl_RayFlagsTerminateOnFirstHitEXT |
                                gl_RayFlagsSkipClosestHitShaderEXT,
                            0xFF, 1, 0, 1, ray_origin, 0, wi, ray_len - EPS, 1);
                const bool visible = any_hit_payload.hit == 0;
                if (visible) {
                    float g =
                        abs(dot(record.triangle_normal, -wi)) / (ray_len_sqr);
                    const float cos_y = dot(-wi, record.triangle_normal);
                    const float pdf_pos_dir = record.triangle_pdf * cos_y / PI;

                    const float pdf_light_w = record.triangle_pdf / g;
                    const float w_light = pdf_fwd / (pdf_light_w);
                    const float w_cam =
                        pdf_pos_dir * abs(cos_x) / (pdf_light_w * cos_y) *
                        (camera_state.d_vcm + camera_state.d_vc * pdf_rev);
                    const float mis_weight = 1. / (1. + w_light + w_cam);
                    if (mis_weight > 0) {
                        vec3 L = mis_weight * abs(cos_x) * f *
                                 camera_state.throughput *
                                 light_mat.emissive_factor /
                                 (pdf_light_w / pc_ray.light_triangle_count);
                        tmp_col.d[coords_idx] += L;
                        lum_sum += luminance(L);
                    }
                }
            }
        }
        // Scattering
        float pdf_dir;
        float cos_theta;
        f = sample_bsdf(shading_nrm, wo, mat, 0, side, camera_state.wi, pdf_dir,
                        cos_theta, seed);

        const bool mat_transmissive =
            (mat.bsdf_props & BSDF_TRANSMISSIVE) == BSDF_TRANSMISSIVE;
        const bool same_hemisphere =
            same_hemisphere(camera_state.wi, wo, shading_nrm);
        if (f == vec3(0) || pdf_dir == 0 ||
            (!same_hemisphere && !mat_transmissive)) {
            break;
        }
        pdf_rev = pdf_dir;
        if (!mat_specular) {
            pdf_rev = bsdf_pdf(mat, shading_nrm, camera_state.wi, wo);
        }
        const float abs_cos_theta = abs(cos_theta);

        camera_state.pos = offset_ray(payload.pos, shading_nrm);
        // Note, same cancellations also occur here from now on
        // see _vcm_generate_light_sample_
        if (!mat_specular) {
            camera_state.d_vc =
                (abs(abs_cos_theta) / pdf_dir) *
                (camera_state.d_vcm + pdf_rev * camera_state.d_vc);
            camera_state.d_vcm = 1.0 / pdf_dir;
        } else {
            camera_state.d_vcm = 0;
            camera_state.d_vc *= abs_cos_theta;
        }

        camera_state.throughput *= f * abs_cos_theta / pdf_dir;
        camera_state.shading_nrm = shading_nrm;
        camera_state.area = payload.area;
        camera_state.material_idx = payload.material_idx;
    }
    path_cnts.d[pixel_idx] = path_idx;
#undef cam_vtx
    return lum_sum;
}

float mlt_trace_light(const vec3 cam_pos, const vec3 cam_nrm,
                      const float cam_area, bool large_step, inout uvec4 seed,
                      const bool save_radiance) {
#define splat(i) splat_data.d[splat_idx + chain * depth_factor + i]
    // Select camera path
    float luminance_sum = 0;
    mlt_sampler.splat_cnt = 0;
    uint path_idx =
        uint(mlt_rand(seed, large_step) * (pc_ray.size_x * pc_ray.size_y));
    uint path_len = path_cnts.d[path_idx];
    path_idx *= pc_ray.max_depth;
    // Trace from light
    VCMState light_state;
    if (!vcm_generate_light_sample(light_state, seed, large_step)) {
        return 0;
    }
    int d;
    for (d = 1;; d++) {
        traceRayEXT(tlas, flags, 0xFF, 0, 0, 0, light_state.pos, tmin,
                    light_state.wi, tmax, 0);
        if (payload.material_idx == -1) {
            break;
        }
        const vec3 hit_pos = payload.pos;
        vec3 wo = light_state.pos - hit_pos;

        vec3 shading_nrm = payload.shading_nrm;
        float cos_wo = dot(wo, shading_nrm);
        vec3 geometry_nrm = payload.geometry_nrm;
        bool side = true;
        if (dot(payload.geometry_nrm, wo) <= 0.)
            geometry_nrm = -geometry_nrm;
        if (cos_wo <= 0.) {
            cos_wo = -cos_wo;
            shading_nrm = -shading_nrm;
            side = false;
        }

        if (dot(geometry_nrm, wo) * dot(shading_nrm, wo) <= 0) {
            // We dont handle BTDF at the moment
            break;
        }
        float dist = length(payload.pos - light_state.pos);
        float dist_sqr = dist * dist;
        wo /= dist;
        const MaterialProps mat =
            load_material(payload.material_idx, payload.uv);
        const bool mat_specular =
            (mat.bsdf_props & BSDF_SPECULAR) == BSDF_SPECULAR;
        // Complete the missing geometry terms
        float cos_theta_wo = abs(dot(wo, shading_nrm));
        // Can't connect from specular to camera path, can't merge either
        light_state.d_vcm *= dist_sqr;
        light_state.d_vcm /= cos_theta_wo;
        light_state.d_vc /= cos_theta_wo;
        light_state.d_vm /= cos_theta_wo;
        if (d >= pc_ray.max_depth + 1) {
            break;
        }
        if (d < pc_ray.max_depth) {
            // Connect to camera
            ivec2 coords;
            vec3 splat_col =
                vcm_connect_cam(cam_pos, cam_nrm, shading_nrm, cam_area,
                                payload.pos, light_state, wo, mat, coords);
            const float lum_val = luminance(splat_col);
            if (lum_val > 0) {
                luminance_sum += lum_val;
                if (save_radiance) {
                    const uint idx = coords.x * pc_ray.size_y + coords.y;
                    const uint splat_cnt = mlt_sampler.splat_cnt;
                    mlt_sampler.splat_cnt++;
                    splat(splat_cnt).idx = idx;
                    splat(splat_cnt).L = splat_col;
                }
            }
        }
        vec3 unused;

        if (!mat_specular) {
#define cam_vtx(i) light_verts.d[i]
            // Connect to cam vertices
            for (int i = 0; i < path_len; i++) {
                uint t = cam_vtx(path_idx + i).path_len;
                uint depth = t + d - 1;
                if (depth > pc_ray.max_depth) {
                    break;
                }
                vec3 dir = hit_pos - cam_vtx(path_idx + i).pos;
                const float len = length(dir);
                const float len_sqr = len * len;
                dir /= len;
                const float cos_light = dot(shading_nrm, -dir);
                const float cos_cam =
                    dot(cam_vtx(path_idx + i).shading_nrm, dir);
                const float G = cos_cam * cos_light / len_sqr;
                if (G > 0) {
                    float pdf_rev = bsdf_pdf(mat, shading_nrm, -dir, wo);
                    vec3 unused;
                    float cam_pdf_fwd, cam_pdf_rev, light_pdf_fwd;
                    const MaterialProps cam_mat =
                        load_material(cam_vtx(path_idx + i).material_idx,
                                      cam_vtx(path_idx + i).uv);
                    const vec3 f_cam =
                        eval_bsdf(cam_vtx(path_idx + i).shading_nrm, unused,
                                  cam_mat, 1, cam_vtx(path_idx + i).side == 1,
                                  dir, cam_pdf_fwd, cam_pdf_rev, cos_cam);
                    const vec3 f_light =
                        eval_bsdf(shading_nrm, wo, mat, 0, side, -dir,
                                  light_pdf_fwd, cos_light);
                    if (f_light != vec3(0) && f_cam != vec3(0)) {
                        cam_pdf_fwd = abs(cos_light) / len_sqr;
                        light_pdf_fwd = abs(cos_cam) / len_sqr;
                        const float w_light =
                            cam_pdf_fwd *
                            (light_state.d_vcm + pdf_rev * light_state.d_vc);
                        const float w_cam =
                            light_pdf_fwd *
                            (cam_vtx(path_idx + i).d_vcm +
                             cam_pdf_rev * cam_vtx(path_idx + i).d_vc);
                        const float mis_weight = 1. / (1 + w_light + w_cam);
                        const vec3 ray_origin =
                            offset_ray(hit_pos, shading_nrm);
                        any_hit_payload.hit = 1;
                        traceRayEXT(tlas,
                                    gl_RayFlagsTerminateOnFirstHitEXT |
                                        gl_RayFlagsSkipClosestHitShaderEXT,
                                    0xFF, 1, 0, 1, ray_origin, 0, -dir,
                                    len - EPS, 1);
                        const bool visible = any_hit_payload.hit == 0;
                        if (visible) {
                            const vec3 L = mis_weight * G *
                                           light_state.throughput *
                                           cam_vtx(path_idx + i).throughput *
                                           f_cam * f_light;
                            luminance_sum += luminance(L);
                            if (save_radiance) {
                                const uint idx = cam_vtx(path_idx + i).coords;
                                const uint splat_cnt = mlt_sampler.splat_cnt;
                                mlt_sampler.splat_cnt++;
                                splat(splat_cnt).idx = idx;
                                splat(splat_cnt).L = L;
                            }
                        }
                    }
                }
            }
        }
        // Continue the walk
        float pdf_dir;
        float cos_theta;
        vec2 rands_dir =
            vec2(mlt_rand(seed, large_step), mlt_rand(seed, large_step));
        const vec3 f =
            sample_bsdf(shading_nrm, wo, mat, 0, side, light_state.wi, pdf_dir,
                        cos_theta, rands_dir);
        const bool same_hemisphere =
            same_hemisphere(light_state.wi, wo, shading_nrm);

        const bool mat_transmissive =
            (mat.bsdf_props & BSDF_TRANSMISSIVE) == BSDF_TRANSMISSIVE;
        if (f == vec3(0) || pdf_dir == 0 ||
            (!same_hemisphere && !mat_transmissive)) {
            break;
        }

        float pdf_rev = pdf_dir;
        if (!mat_specular) {
            pdf_rev = bsdf_pdf(mat, shading_nrm, light_state.wi, wo);
        }
        const float abs_cos_theta = abs(cos_theta);

        light_state.pos = offset_ray(payload.pos, shading_nrm);
        // Note, same cancellations also occur here from now on
        // see _vcm_generate_light_sample_
        if (!mat_specular) {
            light_state.d_vc = (abs_cos_theta / pdf_dir) *
                               (light_state.d_vcm + pdf_rev * light_state.d_vc);
            light_state.d_vcm = 1.0 / pdf_dir;
        } else {
            // Specular pdf has value = inf, so d_vcm = 0;
            light_state.d_vcm = 0;
            // pdf_fwd = pdf_rev = delta -> cancels
            light_state.d_vc *= abs_cos_theta;
        }
        light_state.throughput *= f * abs_cos_theta / pdf_dir;
        light_state.shading_nrm = shading_nrm;
        light_state.area = payload.area;
        light_state.material_idx = payload.material_idx;
    }
    return luminance_sum;
#undef splat
#undef cam_vtx
}

#endif