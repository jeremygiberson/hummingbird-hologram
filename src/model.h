/*
 * model.h — glTF model loading and animation
 *
 * Loads a .glb file via cgltf, uploads geometry and textures to GL,
 * and provides per-frame animation update + draw calls.
 */
#pragma once

#include "platform.h"
#include <stdbool.h>

/* Opaque model handle */
typedef struct Model Model;

/* Load a .glb model from disk. Returns NULL on failure. */
Model *model_load(const char *glb_path);

/* Update animation state. dt is seconds since last frame.
 * The animation loops infinitely. */
void model_update(Model *m, float dt);

/* Draw the model. Caller must have already bound the shader program.
 * model_draw binds VAO/VBOs and issues draw calls.
 *
 * Uniform locations are passed in so the model doesn't need
 * to know about the specific shader. */
#define MAX_JOINTS 32

typedef struct {
    GLint u_model_matrix;
    GLint u_view_matrix;
    GLint u_proj_matrix;
    GLint u_texture0;
    GLint u_texture1;
    GLint u_energy;       /* For subtle scale pulse */
    GLint u_has_skin;     /* Whether GPU skinning is active */
    GLint u_joints[MAX_JOINTS]; /* Joint matrices for skinning */
} ModelUniforms;

void model_draw(const Model *m, const ModelUniforms *uniforms, float energy);

/* Set wing animation speed multiplier (default 2.3).
 * Higher = faster flapping. Set from BPM or other source. */
void model_set_wing_speed(Model *m, float speed);

/* Apply an extra transform on top of the base scene-graph transform.
 * rotation_y is in radians. scale is uniform. translation is (x,y,z).
 * This is applied in model_draw as: base_transform * translate * rotateY * scale */
void model_set_extra_transform(Model *m, float rotation_y,
                                float scale, const float translation[3]);

/* Free GPU and CPU resources. */
void model_destroy(Model *m);

/* Create a procedural debug model (icosahedron) for testing the
 * bloom pipeline when no .glb is available. */
Model *model_create_debug(void);
