/*
 * model.c — glTF model loading and animation via cgltf
 */

/* cgltf implementation — define in exactly one .c file */
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "model.h"
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Default wing animation speed multiplier */
#define WING_ANIM_SPEED 2.3f

/* ------------------------------------------------------------------ */
/* Simple 4x4 matrix math (column-major for GL)                        */
/* ------------------------------------------------------------------ */

typedef float Mat4[16];

static void mat4_identity(Mat4 out) {
    memset(out, 0, 16 * sizeof(float));
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

static void mat4_perspective(Mat4 out, float fov_rad, float aspect,
                              float near, float far) {
    memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fov_rad / 2.0f);
    out[0]  = f / aspect;
    out[5]  = f;
    out[10] = (far + near) / (near - far);
    out[11] = -1.0f;
    out[14] = (2.0f * far * near) / (near - far);
}

static void mat4_translate(Mat4 out, float x, float y, float z) {
    mat4_identity(out);
    out[12] = x;
    out[13] = y;
    out[14] = z;
}

static void mat4_scale_uniform(Mat4 m, float s) {
    m[0] *= s; m[1] *= s; m[2]  *= s;
    m[4] *= s; m[5] *= s; m[6]  *= s;
    m[8] *= s; m[9] *= s; m[10] *= s;
}

static void mat4_rotate_y(Mat4 out, float rad) {
    mat4_identity(out);
    float c = cosf(rad), s = sinf(rad);
    out[0]  =  c;
    out[2]  =  s;
    out[8]  = -s;
    out[10] =  c;
}

static void mat4_rotate_x(Mat4 out, float rad) {
    mat4_identity(out);
    float c = cosf(rad), s = sinf(rad);
    out[5]  =  c;
    out[6]  =  s;
    out[9]  = -s;
    out[10] =  c;
}

static void mat4_multiply(Mat4 out, const Mat4 a, const Mat4 b) {
    Mat4 tmp;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[j * 4 + i] =
                a[0 * 4 + i] * b[j * 4 + 0] +
                a[1 * 4 + i] * b[j * 4 + 1] +
                a[2 * 4 + i] * b[j * 4 + 2] +
                a[3 * 4 + i] * b[j * 4 + 3];
        }
    }
    memcpy(out, tmp, sizeof(Mat4));
}

/* Build a model matrix from Translation, Rotation (quaternion), Scale */
static void mat4_from_trs(Mat4 out, const float t[3], const float q[4], const float s[3]) {
    /* Quaternion to rotation matrix (q = [x, y, z, w]) */
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;

    /* Column 0 */
    out[0]  = (1.0f - (yy + zz)) * s[0];
    out[1]  = (xy + wz)          * s[0];
    out[2]  = (xz - wy)          * s[0];
    out[3]  = 0.0f;
    /* Column 1 */
    out[4]  = (xy - wz)          * s[1];
    out[5]  = (1.0f - (xx + zz)) * s[1];
    out[6]  = (yz + wx)          * s[1];
    out[7]  = 0.0f;
    /* Column 2 */
    out[8]  = (xz + wy)          * s[2];
    out[9]  = (yz - wx)          * s[2];
    out[10] = (1.0f - (xx + yy)) * s[2];
    out[11] = 0.0f;
    /* Column 3 */
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1.0f;
}

/* ------------------------------------------------------------------ */
/* Animation keyframe sampling                                         */
/* ------------------------------------------------------------------ */

/* Binary search for the keyframe pair bracketing time t.
 * Returns the index of the keyframe just before t (or 0 / count-2 at edges). */
static cgltf_size find_keyframe(const cgltf_accessor *input, float t) {
    cgltf_size count = input->count;
    if (count < 2) return 0;

    /* Read timestamps via cgltf */
    float first, last;
    cgltf_accessor_read_float(input, 0, &first, 1);
    cgltf_accessor_read_float(input, count - 1, &last, 1);

    if (t <= first) return 0;
    if (t >= last)  return count - 2;

    /* Linear scan (animations are typically < 200 keyframes; simpler than bsearch) */
    for (cgltf_size i = 0; i < count - 1; i++) {
        float t_next;
        cgltf_accessor_read_float(input, i + 1, &t_next, 1);
        if (t < t_next) return i;
    }
    return count - 2;
}

/* Interpolation factor between keyframes[idx] and keyframes[idx+1] */
static float keyframe_factor(const cgltf_accessor *input, cgltf_size idx, float t) {
    float t0, t1;
    cgltf_accessor_read_float(input, idx, &t0, 1);
    cgltf_accessor_read_float(input, idx + 1, &t1, 1);
    float span = t1 - t0;
    if (span <= 0.0f) return 0.0f;
    float f = (t - t0) / span;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

/* Lerp a vec3 (translation or scale) from sampler output */
static void sample_vec3(const cgltf_animation_sampler *sampler, float t, float out[3]) {
    const cgltf_accessor *input  = sampler->input;
    const cgltf_accessor *output = sampler->output;
    cgltf_size idx = find_keyframe(input, t);

    float v0[3], v1[3];
    cgltf_accessor_read_float(output, idx, v0, 3);

    if (sampler->interpolation == cgltf_interpolation_type_step || idx + 1 >= output->count) {
        out[0] = v0[0]; out[1] = v0[1]; out[2] = v0[2];
        return;
    }

    cgltf_accessor_read_float(output, idx + 1, v1, 3);
    float f = keyframe_factor(input, idx, t);

    out[0] = v0[0] + (v1[0] - v0[0]) * f;
    out[1] = v0[1] + (v1[1] - v0[1]) * f;
    out[2] = v0[2] + (v1[2] - v0[2]) * f;
}

/* Slerp a quaternion from sampler output (glTF uses [x,y,z,w]) */
static void sample_quat(const cgltf_animation_sampler *sampler, float t, float out[4]) {
    const cgltf_accessor *input  = sampler->input;
    const cgltf_accessor *output = sampler->output;
    cgltf_size idx = find_keyframe(input, t);

    float q0[4], q1[4];
    cgltf_accessor_read_float(output, idx, q0, 4);

    if (sampler->interpolation == cgltf_interpolation_type_step || idx + 1 >= output->count) {
        out[0] = q0[0]; out[1] = q0[1]; out[2] = q0[2]; out[3] = q0[3];
        return;
    }

    cgltf_accessor_read_float(output, idx + 1, q1, 4);
    float f = keyframe_factor(input, idx, t);

    /* Ensure shortest path (dot product sign check) */
    float dot = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
    if (dot < 0.0f) {
        q1[0] = -q1[0]; q1[1] = -q1[1]; q1[2] = -q1[2]; q1[3] = -q1[3];
        dot = -dot;
    }

    /* Fall back to lerp+normalize for nearly-identical quaternions */
    if (dot > 0.9995f) {
        out[0] = q0[0] + (q1[0] - q0[0]) * f;
        out[1] = q0[1] + (q1[1] - q0[1]) * f;
        out[2] = q0[2] + (q1[2] - q0[2]) * f;
        out[3] = q0[3] + (q1[3] - q0[3]) * f;
    } else {
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        float w0 = sinf((1.0f - f) * theta) / sin_theta;
        float w1 = sinf(f * theta) / sin_theta;
        out[0] = q0[0] * w0 + q1[0] * w1;
        out[1] = q0[1] * w0 + q1[1] * w1;
        out[2] = q0[2] * w0 + q1[2] * w1;
        out[3] = q0[3] * w0 + q1[3] * w1;
    }

    /* Normalize */
    float len = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
    if (len > 0.0f) {
        out[0] /= len; out[1] /= len; out[2] /= len; out[3] /= len;
    }
}

/* ------------------------------------------------------------------ */
/* Mesh data on GPU                                                    */
/* ------------------------------------------------------------------ */

#define MAX_TEXTURES 4
#define MAX_JOINTS   32

struct Model {
    /* Geometry */
    GLuint vbo_position;
    GLuint vbo_normal;
    GLuint vbo_texcoord;
    GLuint vbo_joints;    /* JOINTS_0: vec4 of joint indices */
    GLuint vbo_weights;   /* WEIGHTS_0: vec4 of joint weights */
    GLuint ibo;
    int    index_count;
    GLenum index_type;  /* GL_UNSIGNED_SHORT or GL_UNSIGNED_INT */

    /* Textures */
    GLuint textures[MAX_TEXTURES];
    int    texture_count;

    /* Animation */
    float  anim_time;       /* Body/head/tail time (1x speed) */
    float  anim_time_wings; /* Wing time (fast speed) */
    float  anim_duration;

    /* Skinning */
    bool   has_skin;
    int    joint_count;
    cgltf_node  *joint_nodes[MAX_JOINTS];    /* Pointers to joint nodes in gltf */
    Mat4   inverse_bind_matrices[MAX_JOINTS]; /* From skin */
    Mat4   joint_matrices[MAX_JOINTS];        /* Computed per-frame: final joint transforms */

    /* cgltf data (kept alive for animation access) */
    cgltf_data *gltf;
    const cgltf_mesh *mesh_ref;  /* Pointer to the mesh node for finding skin */

    /* Transform */
    Mat4 base_transform;  /* Root node hierarchy transform (from scene graph) */
    Mat4 model_matrix;
    Mat4 view_matrix;
    Mat4 proj_matrix;

    /* External overrides (set by layer) */
    float wing_speed;         /* Wing animation speed multiplier (default 2.3) */
    float extra_rotation_y;   /* Additional Y rotation (yaw) in radians */
    float extra_rotation_x;   /* Additional X rotation (pitch) in radians */
    float extra_scale;        /* Additional uniform scale (default 1.0) */
    float extra_translation[3]; /* Additional translation */
};

/* ------------------------------------------------------------------ */
/* Texture loading from cgltf buffer                                   */
/* ------------------------------------------------------------------ */

static GLuint upload_texture(const cgltf_image *image, const cgltf_data *data) {
    (void)data;

    if (!image->buffer_view) {
        fprintf(stderr, "[model] Image has no buffer_view, skipping\n");
        return 0;
    }

    /* For embedded textures in .glb, the data is in the buffer */
    const unsigned char *buf =
        (const unsigned char *)image->buffer_view->buffer->data
        + image->buffer_view->offset;
    int len = (int)image->buffer_view->size;

    /* Decode with stb_image */
    int w, h, channels;
    unsigned char *pixels = stbi_load_from_memory(buf, len, &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "[model] Failed to decode texture: %s\n",
                image->name ? image->name : "(unnamed)");
        return 0;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    stbi_image_free(pixels);

    fprintf(stderr, "[model] Uploaded texture: %s (%dx%d) → %u\n",
            image->name ? image->name : "(unnamed)", w, h, tex);
    return tex;
}

/* ------------------------------------------------------------------ */
/* Accessor helpers                                                    */
/* ------------------------------------------------------------------ */

static const void *accessor_data(const cgltf_accessor *acc) {
    const cgltf_buffer_view *bv = acc->buffer_view;
    return (const char *)bv->buffer->data + bv->offset + acc->offset;
}

static size_t accessor_stride(const cgltf_accessor *acc) {
    const cgltf_buffer_view *bv = acc->buffer_view;
    return bv->stride ? bv->stride : cgltf_calc_size(acc->type, acc->component_type);
}

/* ------------------------------------------------------------------ */
/* External texture loading (from PNG file on disk)                     */
/* ------------------------------------------------------------------ */

static GLuint load_texture_from_file(const char *path) {
    int w, h, channels;
    unsigned char *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "[model] Failed to load texture: %s\n", path);
        return 0;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    stbi_image_free(pixels);
    fprintf(stderr, "[model] Loaded external texture: %s (%dx%d) → %u\n", path, w, h, tex);
    return tex;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Model *model_load(const char *glb_path) {
    cgltf_options options = {0};
    cgltf_data *data = NULL;

    cgltf_result result = cgltf_parse_file(&options, glb_path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "[model] Failed to parse: %s (error %d)\n", glb_path, result);
        return NULL;
    }

    result = cgltf_load_buffers(&options, data, glb_path);
    if (result != cgltf_result_success) {
        fprintf(stderr, "[model] Failed to load buffers: %s\n", glb_path);
        cgltf_free(data);
        return NULL;
    }

    if (data->meshes_count == 0) {
        fprintf(stderr, "[model] No meshes found in %s\n", glb_path);
        cgltf_free(data);
        return NULL;
    }

    Model *m = (Model *)calloc(1, sizeof(Model));
    m->gltf = data;
    m->wing_speed = WING_ANIM_SPEED;
    m->extra_scale = 1.0f;

    /* Use the first mesh, first primitive */
    const cgltf_mesh *mesh = &data->meshes[0];
    const cgltf_primitive *prim = &mesh->primitives[0];

    fprintf(stderr, "[model] Mesh: %s, %zu primitives\n",
            mesh->name ? mesh->name : "(unnamed)", mesh->primitives_count);

    /* Upload index buffer */
    if (prim->indices) {
        const cgltf_accessor *idx = prim->indices;
        const void *idx_data = accessor_data(idx);
        size_t idx_size = idx->count * cgltf_calc_size(idx->type, idx->component_type);

        m->index_count = (int)idx->count;
        m->index_type = (idx->component_type == cgltf_component_type_r_32u)
                        ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;

        glGenBuffers(1, &m->ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_size, idx_data, GL_STATIC_DRAW);

        fprintf(stderr, "[model] Indices: %d\n", m->index_count);
    }

    /* Upload vertex attributes */
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        const cgltf_attribute *attr = &prim->attributes[i];
        const cgltf_accessor *acc = attr->data;
        const void *data_ptr = accessor_data(acc);
        size_t data_size = acc->count * accessor_stride(acc);

        GLuint *target_vbo = NULL;
        if (attr->type == cgltf_attribute_type_position) {
            target_vbo = &m->vbo_position;
            fprintf(stderr, "[model] Positions: %zu vertices\n", acc->count);
        } else if (attr->type == cgltf_attribute_type_normal) {
            target_vbo = &m->vbo_normal;
        } else if (attr->type == cgltf_attribute_type_texcoord) {
            target_vbo = &m->vbo_texcoord;
        } else if (attr->type == cgltf_attribute_type_joints) {
            target_vbo = &m->vbo_joints;
        } else if (attr->type == cgltf_attribute_type_weights) {
            target_vbo = &m->vbo_weights;
        } else {
            continue;
        }

        glGenBuffers(1, target_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, *target_vbo);
        glBufferData(GL_ARRAY_BUFFER, data_size, data_ptr, GL_STATIC_DRAW);
    }

    /* Upload ALL textures from the GLB images array.
     * The embedded textures have correct UV-mapped vibrant colors.
     * Image 0 = base color, Image 1 = normal map. */
    m->texture_count = 0;
    for (cgltf_size i = 0; i < data->images_count && m->texture_count < MAX_TEXTURES; i++) {
        GLuint tex = upload_texture(&data->images[i], data);
        if (tex) m->textures[m->texture_count++] = tex;
    }

    /* Load skin (skeletal animation) */
    m->has_skin = false;
    m->joint_count = 0;
    m->mesh_ref = mesh;

    /* Find the skin associated with the mesh node */
    cgltf_node *mesh_node = NULL;
    for (cgltf_size ni = 0; ni < data->nodes_count; ni++) {
        if (data->nodes[ni].mesh == mesh) {
            mesh_node = &data->nodes[ni];
            break;
        }
    }
    if (mesh_node && mesh_node->skin) {
        const cgltf_skin *skin = mesh_node->skin;
        int jcount = (int)skin->joints_count;
        if (jcount > MAX_JOINTS) jcount = MAX_JOINTS;
        m->joint_count = jcount;
        m->has_skin = true;

        /* Store joint node pointers */
        for (int j = 0; j < jcount; j++) {
            m->joint_nodes[j] = skin->joints[j];
        }

        /* Load inverse bind matrices */
        if (skin->inverse_bind_matrices) {
            for (int j = 0; j < jcount; j++) {
                cgltf_accessor_read_float(skin->inverse_bind_matrices, j,
                                          m->inverse_bind_matrices[j], 16);
            }
        } else {
            for (int j = 0; j < jcount; j++) {
                mat4_identity(m->inverse_bind_matrices[j]);
            }
        }

        /* Initialize joint matrices to identity */
        for (int j = 0; j < jcount; j++) {
            mat4_identity(m->joint_matrices[j]);
        }

        fprintf(stderr, "[model] Skin loaded: %d joints\n", jcount);
    }

    /* Check if JOINTS_0 uses unsigned byte (needs conversion to float) */
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == cgltf_attribute_type_joints) {
            const cgltf_accessor *jacc = prim->attributes[i].data;
            fprintf(stderr, "[model] JOINTS_0: component_type=%d (5121=ubyte, 5123=ushort)\n",
                    jacc->component_type);
            /* If JOINTS_0 is stored as unsigned bytes, we need to convert to floats
             * for the vertex shader. Upload as float VBO. */
            if (jacc->component_type == cgltf_component_type_r_8u) {
                /* Re-upload as float data */
                int count = (int)jacc->count;
                float *float_joints = (float *)malloc(count * 4 * sizeof(float));
                for (int v = 0; v < count; v++) {
                    float vals[4];
                    cgltf_accessor_read_float(jacc, v, vals, 4);
                    float_joints[v * 4 + 0] = vals[0];
                    float_joints[v * 4 + 1] = vals[1];
                    float_joints[v * 4 + 2] = vals[2];
                    float_joints[v * 4 + 3] = vals[3];
                }
                glBindBuffer(GL_ARRAY_BUFFER, m->vbo_joints);
                glBufferData(GL_ARRAY_BUFFER, count * 4 * sizeof(float),
                             float_joints, GL_STATIC_DRAW);
                free(float_joints);
                fprintf(stderr, "[model] Re-uploaded JOINTS_0 as float\n");
            } else if (jacc->component_type == cgltf_component_type_r_16u) {
                /* Re-upload as float data */
                int count = (int)jacc->count;
                float *float_joints = (float *)malloc(count * 4 * sizeof(float));
                for (int v = 0; v < count; v++) {
                    float vals[4];
                    cgltf_accessor_read_float(jacc, v, vals, 4);
                    float_joints[v * 4 + 0] = vals[0];
                    float_joints[v * 4 + 1] = vals[1];
                    float_joints[v * 4 + 2] = vals[2];
                    float_joints[v * 4 + 3] = vals[3];
                }
                glBindBuffer(GL_ARRAY_BUFFER, m->vbo_joints);
                glBufferData(GL_ARRAY_BUFFER, count * 4 * sizeof(float),
                             float_joints, GL_STATIC_DRAW);
                free(float_joints);
                fprintf(stderr, "[model] Re-uploaded JOINTS_0 as float\n");
            }
            break;
        }
    }

    /* Animation duration */
    if (data->animations_count > 0) {
        const cgltf_animation *anim = &data->animations[0];
        m->anim_duration = 0.0f;
        for (cgltf_size i = 0; i < anim->samplers_count; i++) {
            const cgltf_accessor *input = anim->samplers[i].input;
            if (input->count > 0) {
                float max_t = input->max[0];
                if (max_t > m->anim_duration) m->anim_duration = max_t;
            }
        }
        /* Find the usable loop point by detecting where wing rotation
         * stops cycling. The baked animation has a "rest" tail at the end
         * where the wings lock in place. We find the last keyframe that
         * still matches the cyclic pattern by comparing to the first
         * keyframe's value — the cycle restarts each time the quaternion
         * returns close to its t=0 value. */
        for (cgltf_size i = 0; i < anim->channels_count; i++) {
            const cgltf_animation_channel *ch = &anim->channels[i];
            if (ch->target_path != cgltf_animation_path_type_rotation)
                continue;
            if (!ch->target_node->name || !strstr(ch->target_node->name, "Wing1.R"))
                continue;

            /* Sample at fine intervals and find where the cycle breaks */
            float q_prev[4], q_curr[4];
            sample_quat(ch->sampler, 0.0f, q_prev);
            float last_cycle_start = 0.0f;
            float step = 0.05f;

            for (float t = step; t <= m->anim_duration; t += step) {
                sample_quat(ch->sampler, t, q_curr);
                /* Detect when the quaternion returns close to t=0 value */
                float dot = q_prev[0]*q_curr[0] + q_prev[1]*q_curr[1] +
                            q_prev[2]*q_curr[2] + q_prev[3]*q_curr[3];
                if (dot < 0.0f) dot = -dot;

                float dot0 = q_curr[0]*q_prev[0] + q_curr[1]*q_prev[1] +
                             q_curr[2]*q_prev[2] + q_curr[3]*q_prev[3]; (void)dot0;

                /* Check similarity to the t=0 pose */
                float q0[4];
                sample_quat(ch->sampler, 0.0f, q0);
                float sim = q_curr[0]*q0[0] + q_curr[1]*q0[1] +
                            q_curr[2]*q0[2] + q_curr[3]*q0[3];
                if (sim < 0.0f) sim = -sim;
                if (sim > 0.999f && t > 1.0f) {
                    last_cycle_start = t;
                }
            }

            if (last_cycle_start > 1.0f) {
                fprintf(stderr, "[model] Wing flap cycle: last clean restart at %.2fs (was %.2fs)\n",
                        last_cycle_start, m->anim_duration);
                m->anim_duration = last_cycle_start;
            }
            break;
        }

        fprintf(stderr, "[model] Animation loop duration: %.2fs\n", m->anim_duration);
    } else {
        m->anim_duration = 1.0f;  /* Static model, just loop identity */
    }

    /* Compute the root transform by walking the scene graph.
     * glTF models from Blender often have a root node with rotation
     * (Z-up to Y-up conversion). We need to apply this. */
    mat4_identity(m->model_matrix);

    /* Find which node owns this mesh and accumulate transforms up to root */
    for (cgltf_size ni = 0; ni < data->nodes_count; ni++) {
        if (data->nodes[ni].mesh == mesh) {
            /* Walk from mesh node up to root, collecting transforms */
            #define MAX_DEPTH 16
            const cgltf_node *chain[MAX_DEPTH];
            int depth = 0;
            const cgltf_node *cur = &data->nodes[ni];
            while (cur && depth < MAX_DEPTH) {
                chain[depth++] = cur;
                cur = cur->parent;
            }
            /* Multiply from root down to mesh node */
            mat4_identity(m->model_matrix);
            for (int d = depth - 1; d >= 0; d--) {
                const cgltf_node *n = chain[d];
                Mat4 local;
                if (n->has_matrix) {
                    memcpy(local, n->matrix, sizeof(Mat4));
                } else {
                    float t[3] = {0,0,0}, r[4] = {0,0,0,1}, s[3] = {1,1,1};
                    if (n->has_translation) { t[0]=n->translation[0]; t[1]=n->translation[1]; t[2]=n->translation[2]; }
                    if (n->has_rotation)    { r[0]=n->rotation[0]; r[1]=n->rotation[1]; r[2]=n->rotation[2]; r[3]=n->rotation[3]; }
                    if (n->has_scale)       { s[0]=n->scale[0]; s[1]=n->scale[1]; s[2]=n->scale[2]; }
                    mat4_from_trs(local, t, r, s);
                }
                mat4_multiply(m->model_matrix, m->model_matrix, local);
            }
            #undef MAX_DEPTH
            break;
        }
    }
    /* Rotate model 90° around Y for side profile view */
    Mat4 y_rot;
    mat4_rotate_y(y_rot, -(float)M_PI / 2.0f);
    mat4_multiply(m->model_matrix, y_rot, m->model_matrix);

    memcpy(m->base_transform, m->model_matrix, sizeof(Mat4));

    /* Compute camera distance from model bounding box */
    float cam_dist = 5.0f;  /* default fallback */
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == cgltf_attribute_type_position) {
            const cgltf_accessor *pos = prim->attributes[i].data;
            if (pos->has_min && pos->has_max) {
                float dx = pos->max[0] - pos->min[0];
                float dy = pos->max[1] - pos->min[1];
                float dz = pos->max[2] - pos->min[2];
                float extent = sqrtf(dx*dx + dy*dy + dz*dz);
                /* Position camera so the model fits in the FOV */
                float fov = 45.0f * (float)M_PI / 180.0f;
                cam_dist = (extent * 0.6f) / tanf(fov / 2.0f);
                float cy = (pos->max[1] + pos->min[1]) * 0.5f;
                mat4_translate(m->view_matrix, 0.0f, -cy, -cam_dist);
                fprintf(stderr, "[model] Extent: %.2f, camera dist: %.2f, center Y: %.2f\n",
                        extent, cam_dist, cy);
            }
            break;
        }
    }
    if (m->view_matrix[14] == 0.0f) {
        /* Fallback if no min/max available */
        mat4_translate(m->view_matrix, 0.0f, 0.0f, -cam_dist);
    }

    mat4_perspective(m->proj_matrix, 45.0f * (float)M_PI / 180.0f,
                     (float)HOLOGRAM_DEFAULT_WIDTH / (float)HOLOGRAM_DEFAULT_HEIGHT,
                     0.1f, 100.0f);

    fprintf(stderr, "[model] Loaded: %s (%d indices, %d textures)\n",
            glb_path, m->index_count, m->texture_count);
    return m;
}

/* Compute a node's local TRS matrix (rest pose) */
static void node_local_matrix(const cgltf_node *n, Mat4 out) {
    if (n->has_matrix) {
        memcpy(out, n->matrix, sizeof(Mat4));
    } else {
        float t[3] = {0,0,0}, r[4] = {0,0,0,1}, s[3] = {1,1,1};
        if (n->has_translation) { t[0]=n->translation[0]; t[1]=n->translation[1]; t[2]=n->translation[2]; }
        if (n->has_rotation)    { r[0]=n->rotation[0]; r[1]=n->rotation[1]; r[2]=n->rotation[2]; r[3]=n->rotation[3]; }
        if (n->has_scale)       { s[0]=n->scale[0]; s[1]=n->scale[1]; s[2]=n->scale[2]; }
        mat4_from_trs(out, t, r, s);
    }
}

/* Compute world transform for a node by walking up to root */
static void node_world_transform(const cgltf_node *n, Mat4 out) {
    #define WT_MAX_DEPTH 16
    const cgltf_node *chain[WT_MAX_DEPTH];
    int depth = 0;
    const cgltf_node *cur = n;
    while (cur && depth < WT_MAX_DEPTH) {
        chain[depth++] = cur;
        cur = cur->parent;
    }
    mat4_identity(out);
    for (int d = depth - 1; d >= 0; d--) {
        Mat4 local;
        node_local_matrix(chain[d], local);
        mat4_multiply(out, out, local);
    }
    #undef WT_MAX_DEPTH
}

/* Wing bones animate at a faster rate to simulate hummingbird flapping.
 * The baked animation has ~0.67 flaps/sec (1.5s per cycle).
 * Body/head/tail stay at 1x for smooth, natural sway. */

static bool is_wing_node(const cgltf_node *node) {
    if (!node || !node->name) return false;
    return strstr(node->name, "Wing") != NULL;
}

void model_update(Model *m, float dt) {
    if (!m) return;

    m->anim_time += dt;
    m->anim_time_wings += dt * m->wing_speed;

    if (m->gltf && m->gltf->animations_count > 0) {
        /* Wrap both timers */
        while (m->anim_time >= m->anim_duration)
            m->anim_time -= m->anim_duration;
        while (m->anim_time_wings >= m->anim_duration)
            m->anim_time_wings -= m->anim_duration;

        /* Apply animation channels: wing bones use fast time,
         * everything else uses normal time. */
        const cgltf_animation *anim = &m->gltf->animations[0];
        for (cgltf_size i = 0; i < anim->channels_count; i++) {
            const cgltf_animation_channel *ch = &anim->channels[i];
            const cgltf_animation_sampler *sampler = ch->sampler;

            if (!ch->target_node || !sampler->input || !sampler->output)
                continue;
            if (sampler->input->count < 1)
                continue;

            cgltf_node *target = ch->target_node;
            float t = is_wing_node(target) ? m->anim_time_wings : m->anim_time;

            switch (ch->target_path) {
            case cgltf_animation_path_type_translation:
                sample_vec3(sampler, t, target->translation);
                target->has_translation = 1;
                target->has_matrix = 0;
                break;
            case cgltf_animation_path_type_rotation:
                sample_quat(sampler, t, target->rotation);
                target->has_rotation = 1;
                target->has_matrix = 0;
                break;
            case cgltf_animation_path_type_scale:
                sample_vec3(sampler, t, target->scale);
                target->has_scale = 1;
                target->has_matrix = 0;
                break;
            default:
                break;
            }
        }

        /* Compute skinning joint matrices */
        if (m->has_skin) {
            for (int j = 0; j < m->joint_count; j++) {
                Mat4 world;
                node_world_transform(m->joint_nodes[j], world);
                mat4_multiply(m->joint_matrices[j], world, m->inverse_bind_matrices[j]);
            }
        }

        /* Model matrix = base_transform * extra_translate * extra_rotateY * extra_scale */
        memcpy(m->model_matrix, m->base_transform, sizeof(Mat4));

        /* Apply extra translation */
        if (m->extra_translation[0] != 0.0f || m->extra_translation[1] != 0.0f ||
            m->extra_translation[2] != 0.0f) {
            Mat4 t;
            mat4_translate(t, m->extra_translation[0], m->extra_translation[1],
                           m->extra_translation[2]);
            mat4_multiply(m->model_matrix, m->model_matrix, t);
        }

        /* Apply extra Y rotation (yaw) */
        if (m->extra_rotation_y != 0.0f) {
            Mat4 r;
            mat4_rotate_y(r, m->extra_rotation_y);
            mat4_multiply(m->model_matrix, m->model_matrix, r);
        }

        /* Apply extra X rotation (pitch) */
        if (m->extra_rotation_x != 0.0f) {
            Mat4 r;
            mat4_rotate_x(r, m->extra_rotation_x);
            mat4_multiply(m->model_matrix, m->model_matrix, r);
        }

        /* Apply extra scale */
        if (m->extra_scale != 1.0f) {
            mat4_scale_uniform(m->model_matrix, m->extra_scale);
        }
    } else {
        /* No animation data (debug model or static) — spin on Y axis */
        mat4_rotate_y(m->model_matrix, m->anim_time * 0.8f);
    }
}

void model_draw(const Model *m, const ModelUniforms *u, float energy) {
    if (!m || m->index_count == 0) return;

    /* Compute model matrix with subtle energy pulse */
    Mat4 model;
    memcpy(model, m->model_matrix, sizeof(Mat4));
    float scale = 1.0f + energy * 0.05f;  /* Max 5% scale pulse */
    mat4_scale_uniform(model, scale);

    /* Set uniforms */
    glUniformMatrix4fv(u->u_model_matrix, 1, GL_FALSE, model);
    glUniformMatrix4fv(u->u_view_matrix, 1, GL_FALSE, m->view_matrix);
    glUniformMatrix4fv(u->u_proj_matrix, 1, GL_FALSE, m->proj_matrix);
    glUniform1f(u->u_energy, energy);

    /* Upload skinning uniforms */
    if (m->has_skin && u->u_has_skin >= 0) {
        glUniform1i(u->u_has_skin, 1);
        for (int j = 0; j < m->joint_count; j++) {
            if (u->u_joints[j] >= 0) {
                glUniformMatrix4fv(u->u_joints[j], 1, GL_FALSE, m->joint_matrices[j]);
            }
        }
    } else if (u->u_has_skin >= 0) {
        glUniform1i(u->u_has_skin, 0);
    }

    /* Bind textures */
    for (int i = 0; i < m->texture_count && i < 2; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m->textures[i]);
    }
    if (u->u_texture0 >= 0) glUniform1i(u->u_texture0, 0);
    if (u->u_texture1 >= 0) glUniform1i(u->u_texture1, 1);

    /* Bind vertex attributes: 0=pos, 1=normal, 2=texcoord, 3=joints, 4=weights */
    if (m->vbo_position) {
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo_position);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);
    }
    if (m->vbo_normal) {
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo_normal);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, NULL);
    }
    if (m->vbo_texcoord) {
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo_texcoord);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    }
    if (m->vbo_joints) {
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo_joints);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 0, NULL);
    }
    if (m->vbo_weights) {
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo_weights);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 0, NULL);
    }

    /* Draw */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glDrawElements(GL_TRIANGLES, m->index_count, m->index_type, NULL);

    /* Cleanup */
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
}

void model_set_wing_speed(Model *m, float speed) {
    if (m) m->wing_speed = speed;
}

void model_set_extra_transform(Model *m, float rotation_y, float rotation_x,
                                float scale, const float translation[3]) {
    if (!m) return;
    m->extra_rotation_y = rotation_y;
    m->extra_rotation_x = rotation_x;
    m->extra_scale = scale;
    m->extra_translation[0] = translation[0];
    m->extra_translation[1] = translation[1];
    m->extra_translation[2] = translation[2];
}

void model_destroy(Model *m) {
    if (!m) return;

    if (m->vbo_position) glDeleteBuffers(1, &m->vbo_position);
    if (m->vbo_normal)   glDeleteBuffers(1, &m->vbo_normal);
    if (m->vbo_texcoord) glDeleteBuffers(1, &m->vbo_texcoord);
    if (m->vbo_joints)   glDeleteBuffers(1, &m->vbo_joints);
    if (m->vbo_weights)  glDeleteBuffers(1, &m->vbo_weights);
    if (m->ibo)          glDeleteBuffers(1, &m->ibo);

    for (int i = 0; i < m->texture_count; i++) {
        glDeleteTextures(1, &m->textures[i]);
    }

    if (m->gltf) cgltf_free(m->gltf);
    free(m);
}

/* ------------------------------------------------------------------ */
/* Debug procedural geometry: icosahedron                               */
/* ------------------------------------------------------------------ */

Model *model_create_debug(void) {
    /* Icosahedron vertices (golden ratio construction) */
    const float t = (1.0f + sqrtf(5.0f)) / 2.0f;
    const float n = 1.0f / sqrtf(1.0f + t * t);  /* normalize factor */

    /* 12 vertices of an icosahedron */
    float verts[][3] = {
        {-n, t*n, 0}, { n, t*n, 0}, {-n,-t*n, 0}, { n,-t*n, 0},
        {0, -n, t*n}, {0,  n, t*n}, {0, -n,-t*n}, {0,  n,-t*n},
        { t*n, 0,-n}, { t*n, 0, n}, {-t*n, 0,-n}, {-t*n, 0, n},
    };

    /* 20 triangular faces */
    unsigned short indices[] = {
        0,11,5,  0,5,1,   0,1,7,   0,7,10,  0,10,11,
        1,5,9,   5,11,4,  11,10,2, 10,7,6,  7,1,8,
        3,9,4,   3,4,2,   3,2,6,   3,6,8,   3,8,9,
        4,9,5,   2,4,11,  6,2,10,  8,6,7,   9,8,1,
    };

    int vert_count = 12;
    int idx_count = 60;

    /* Compute face normals (same as vertex normals for icosahedron) */
    float normals[12][3];
    for (int i = 0; i < vert_count; i++) {
        float len = sqrtf(verts[i][0]*verts[i][0] +
                          verts[i][1]*verts[i][1] +
                          verts[i][2]*verts[i][2]);
        normals[i][0] = verts[i][0] / len;
        normals[i][1] = verts[i][1] / len;
        normals[i][2] = verts[i][2] / len;
    }

    /* Simple UV: spherical projection */
    float texcoords[12][2];
    for (int i = 0; i < vert_count; i++) {
        texcoords[i][0] = 0.5f + atan2f(normals[i][2], normals[i][0]) / (2.0f * (float)M_PI);
        texcoords[i][1] = 0.5f - asinf(normals[i][1]) / (float)M_PI;
    }

    Model *m = (Model *)calloc(1, sizeof(Model));
    m->gltf = NULL;
    m->anim_time = 0.0f;
    m->anim_duration = 0.0f;
    m->texture_count = 0;
    m->wing_speed = WING_ANIM_SPEED;
    m->extra_scale = 1.0f;
    mat4_identity(m->base_transform);

    /* Upload geometry */
    glGenBuffers(1, &m->vbo_position);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo_position);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glGenBuffers(1, &m->vbo_normal);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo_normal);
    glBufferData(GL_ARRAY_BUFFER, sizeof(normals), normals, GL_STATIC_DRAW);

    glGenBuffers(1, &m->vbo_texcoord);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo_texcoord);
    glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_STATIC_DRAW);

    glGenBuffers(1, &m->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    m->index_count = idx_count;
    m->index_type = GL_UNSIGNED_SHORT;

    /* Create a white placeholder texture */
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    unsigned char white[] = {255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m->textures[0] = tex;
    m->texture_count = 1;

    /* Initial transforms */
    mat4_identity(m->model_matrix);
    mat4_translate(m->view_matrix, 0.0f, 0.0f, -3.0f);
    mat4_perspective(m->proj_matrix, 45.0f * (float)M_PI / 180.0f,
                     (float)HOLOGRAM_DEFAULT_WIDTH / (float)HOLOGRAM_DEFAULT_HEIGHT,
                     0.1f, 100.0f);

    fprintf(stderr, "[model] Created debug icosahedron (%d tris)\n", idx_count / 3);
    return m;
}
