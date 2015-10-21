// Copyright [2015] Albert Huang

#include "sceneview/internal_gl.hpp"

#include "sceneview/draw_scene.hpp"

#include <cmath>
#include <vector>

#include <QOpenGLTexture>

#include "sceneview/camera_node.hpp"
#include "sceneview/group_node.hpp"
#include "sceneview/light_node.hpp"
#include "sceneview/mesh_node.hpp"
#include "sceneview/resource_manager.hpp"
#include "sceneview/scene_node.hpp"
#include "sceneview/stock_resources.hpp"

#if 0
#define dbg(fmt, ...) printf(fmt, __VA_ARGS__)
#else
#define dbg(...)
#endif

namespace sv {

DrawScene::DrawScene(const ResourceManager::Ptr& resources,
    const Scene::Ptr& scene) :
  resources_(resources),
  scene_(scene),
  cur_camera_(nullptr),
  bounding_box_mesh_(nullptr),
  draw_bounding_boxes_(false) {}

void DrawScene::Draw(CameraNode* camera) {
  cur_camera_ = camera;

  // TODO(albert) sort the meshes

  // render each mesh
  for (MeshNode* mesh : scene_->Meshes()) {
    if (mesh->Visible()) {
      DrawMesh(mesh);
    }

    if (draw_bounding_boxes_) {
      QMatrix4x4 mesh_to_world = mesh->GetTransform();
      for (SceneNode* node = mesh->ParentNode(); node;
          node = node->ParentNode()) {
        mesh_to_world = node->GetTransform() * mesh_to_world;
      }
      const AxisAlignedBox box_orig = mesh->GeometryBoundingBox();
      const AxisAlignedBox box = box_orig.Transformed(mesh_to_world);
      DrawBoundingBox(box);
    }
  }

  cur_camera_ = nullptr;
}

void DrawScene::DrawMesh(MeshNode* mesh) {
  // Compute the model matrix and check visibility
  QMatrix4x4 mesh_to_world = mesh->GetTransform();
  for (SceneNode* node = mesh->ParentNode(); node; node = node->ParentNode()) {
    mesh_to_world = node->GetTransform() * mesh_to_world;
    if (!node->Visible()) {
      return;
    }
  }

  for (const GeometryMaterialPair& component : mesh->Components()) {
    const GeometryResource::Ptr& geometry = component.first;
    const MaterialResource::Ptr& material = component.second;

    DrawMeshCmoponent(geometry, material, mesh_to_world);
  }
}

static void SetupAttributeArray(QOpenGLShaderProgram* program,
    int location, int num_attributes,
    GLenum attr_type, int offset, int attribute_size) {
  if (location < 0) {
    return;
  }

  if (num_attributes > 0) {
        program->enableAttributeArray(location);
        program->setAttributeBuffer(location,
            attr_type, offset, attribute_size, 0);
  } else {
    program->disableAttributeArray(location);
  }
}

void DrawScene::DrawMeshCmoponent(const GeometryResource::Ptr& geometry,
    const MaterialResource::Ptr& material,
    const QMatrix4x4& mesh_to_world) {
  // Load the mesh geometry and bind its vertex buffer
  QOpenGLBuffer* vbo = geometry->VBO();
  vbo->bind();

  // Activate the shader program
  const ShaderResource::Ptr& shader = material->Shader();
  QOpenGLShaderProgram* program = nullptr;
  if (shader) {
    program = shader->Program();
  }
  if (program) {
    program->bind();

    const QMatrix4x4& model_mat = mesh_to_world;

    // Set shader standard variables
    const ShaderStandardVariables& locs = shader->StandardVariables();
    const QMatrix4x4 proj_mat = cur_camera_->GetProjectionMatrix();
    const QMatrix4x4 view_mat = cur_camera_->GetViewMatrix();

    // Set uniform variables
    if (locs.sv_proj_mat >= 0) {
      program->setUniformValue(locs.sv_proj_mat, proj_mat);
    }
    if (locs.sv_view_mat >= 0) {
      program->setUniformValue(locs.sv_view_mat, view_mat);
    }
    if (locs.sv_view_mat_inv >= 0) {
      program->setUniformValue(locs.sv_view_mat_inv, view_mat.inverted());
    }
    if (locs.sv_model_mat >= 0) {
      program->setUniformValue(locs.sv_model_mat, model_mat);
    }
    if (locs.sv_mvp_mat >= 0) {
      program->setUniformValue(locs.sv_mvp_mat,
          proj_mat * view_mat * model_mat);
    }
    if (locs.sv_mv_mat >= 0) {
      program->setUniformValue(locs.sv_mv_mat, view_mat * model_mat);
    }
    if (locs.sv_model_normal_mat >= 0) {
      program->setUniformValue(locs.sv_model_normal_mat,
          model_mat.normalMatrix());
    }

    const std::vector<LightNode*>& lights = scene_->Lights();
    int num_lights = lights.size();
    if (num_lights > kShaderMaxLights) {
      printf("Too many lights. Max: %d\n", kShaderMaxLights);
      num_lights = kShaderMaxLights;
    }

    for (int light_ind = 0; light_ind < num_lights; ++light_ind) {
      const LightNode* light_node = lights[light_ind];
      const ShaderLightLocation light_loc = locs.sv_lights[light_ind];
      LightType light_type = light_node->GetLightType();

      if (light_loc.is_directional >= 0) {
        const bool is_directional = light_type == LightType::kDirectional;
        program->setUniformValue(light_loc.is_directional, is_directional);
      }

      if (light_loc.direction >= 0) {
        const QVector3D light_dir = light_node->Direction();
        program->setUniformValue(light_loc.direction, light_dir);
      }

      if (light_loc.position >= 0) {
        const QVector3D light_pos = light_node->Translation();
        program->setUniformValue(light_loc.position, light_pos);
      }

      if (light_loc.ambient >= 0) {
        const float ambient = light_node->Ambient();
        program->setUniformValue(light_loc.ambient, ambient);
      }

      if (light_loc.color >= 0) {
        const QVector3D color = light_node->Color();
        program->setUniformValue(light_loc.color, color);
      }

      if (light_loc.attenuation >= 0) {
        const float attenuation = light_node->Attenuation();
        program->setUniformValue(light_loc.attenuation, attenuation);
      }

      if (light_loc.cone_angle >= 0) {
        const float cone_angle = light_node->ConeAngle() * M_PI / 180;
        program->setUniformValue(light_loc.cone_angle, cone_angle);
      }
    }

    // Load shader uniform variables from the material
    for (auto& item : material->ShaderParameters()) {
      ShaderUniform& uniform = item.second;
      uniform.LoadToProgram(program);
    }

    // Load textures
    unsigned int texunit = 0;
    for (auto& item : *(material->GetTextures())) {
      const QString& texname = item.first;
      QOpenGLTexture* texture = item.second;
      texture->bind(texunit);
      program->setUniformValue(texname.toStdString().c_str(), texunit);
    }

    // Per-vertex attribute arrays
    SetupAttributeArray(program, locs.sv_vert_pos,
        geometry->NumVertices(), GL_FLOAT, geometry->VertexOffset(), 3);
    SetupAttributeArray(program, locs.sv_normal,
        geometry->NumNormals(), GL_FLOAT, geometry->NormalOffset(), 3);
    SetupAttributeArray(program, locs.sv_diffuse,
        geometry->NumDiffuse(), GL_FLOAT, geometry->DiffuseOffset(), 4);
    SetupAttributeArray(program, locs.sv_specular,
        geometry->NumSpecular(), GL_FLOAT, geometry->SpecularOffset(), 4);
    SetupAttributeArray(program, locs.sv_shininess,
        geometry->NumShininess(), GL_FLOAT, geometry->ShininessOffset(), 1);
    SetupAttributeArray(program, locs.sv_tex_coords_0,
        geometry->NumTexCoords0(), GL_FLOAT, geometry->TexCoords0Offset(), 2);
  } else {
    glUseProgram(0);
  }

  glFrontFace(GL_CCW);

  // set OpenGL attributes based on material properties.
  if (material->TwoSided()) {
    glDisable(GL_CULL_FACE);
  } else {
    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
  }

  if (material->DepthTest()) {
    glEnable(GL_DEPTH_TEST);
  } else {
    glDisable(GL_DEPTH_TEST);
  }

  if (material->DepthWrite()) {
    glDepthMask(GL_TRUE);
  } else {
    glDepthMask(GL_FALSE);
  }

  if (material->ColorWrite()) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  } else {
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  }

  const float point_size = material->PointSize();
  if (point_size > 0) {
    glPointSize(point_size);
  }

  const float line_width = material->LineWidth();
  if (line_width > 0) {
    glLineWidth(line_width);
  }

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Draw the mesh geometry
  QOpenGLBuffer* index_buffer = geometry->IndexBuffer();
  if (index_buffer) {
    index_buffer->bind();
    glDrawElements(geometry->GLMode(), geometry->NumIndices(),
        geometry->IndexType(), 0);
    index_buffer->release();
  } else {
    glDrawArrays(geometry->GLMode(), 0, geometry->NumVertices());
  }

  GLenum gl_err = glGetError();
  if (gl_err != GL_NO_ERROR) {
    printf("OpenGL: %s\n", sv::glErrorString(gl_err));
  }

  // TODO(albert) check if we should call glDrawElements() instead of
  // glDrawArrays()

  // Done. Release resources
  if (program) {
    program->release();
  }
  vbo->release();

  // If we called glPointSize() earlier, then reset the value.
  if (point_size > 0) {
    glPointSize(1);
  }

  if (line_width > 0) {
    glLineWidth(1);
  }

  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void DrawScene::DrawBoundingBox(const AxisAlignedBox& box) {
  if (!bounding_box_mesh_) {
    StockResources stock(resources_);
    ShaderResource::Ptr shader =
      stock.Shader(StockResources::kUniformColorNoLighting);

    MaterialResource::Ptr material = resources_->MakeMaterial(shader);
    material->SetParam("color", 0.0f, 1.0f, 0.0f, 1.0f);

    GeometryResource::Ptr geometry = resources_->MakeGeometry();
    GeometryData gdata;
    gdata.gl_mode = GL_LINES;
    gdata.vertices = {
      { QVector3D(0, 0, 0) },
      { QVector3D(0, 1, 0) },
      { QVector3D(1, 1, 0) },
      { QVector3D(1, 0, 0) },
      { QVector3D(0, 0, 1) },
      { QVector3D(0, 1, 1) },
      { QVector3D(1, 1, 1) },
      { QVector3D(1, 0, 1) },
    };
    gdata.indices = {
      0, 1, 1, 2, 2, 3, 3, 0,
      4, 5, 5, 6, 6, 7, 7, 4,
      0, 4, 1, 5, 2, 6, 3, 7 };
    geometry->Load(gdata);

    bounding_box_mesh_ = scene_->MakeMesh(nullptr);
    bounding_box_mesh_->Add(geometry, material);

    // hack to prevent the bounding box to appear during normal rendering
    bounding_box_mesh_->SetVisible(false);
  }

  bounding_box_mesh_->SetScale(box.Max() - box.Min());
  bounding_box_mesh_->SetTranslation(box.Min());

  DrawMesh(bounding_box_mesh_);
}


}  // namespace sv
