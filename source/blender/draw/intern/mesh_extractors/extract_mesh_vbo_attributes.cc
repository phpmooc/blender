/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "BLI_array_utils.hh"
#include "BLI_string.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"

#include "attribute_convert.hh"
#include "draw_attributes.hh"
#include "draw_subdivision.hh"
#include "extract_mesh.hh"

#include "GPU_vertex_buffer.hh"

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Attributes
 * \{ */

static void init_vbo_for_attribute(const MeshRenderData &mr,
                                   gpu::VertBuf &vbo,
                                   const DRW_AttributeRequest &request,
                                   bool build_on_device,
                                   uint32_t len)
{
  char attr_name[32], attr_safe_name[GPU_MAX_SAFE_ATTR_NAME];
  GPU_vertformat_safe_attr_name(request.attribute_name, attr_safe_name, GPU_MAX_SAFE_ATTR_NAME);
  /* Attributes use auto-name. */
  SNPRINTF(attr_name, "a%s", attr_safe_name);

  GPUVertFormat format = init_format_for_attribute(request.cd_type, attr_name);
  GPU_vertformat_deinterleave(&format);

  if (mr.active_color_name && STREQ(request.attribute_name, mr.active_color_name)) {
    GPU_vertformat_alias_add(&format, "ac");
  }
  if (mr.default_color_name && STREQ(request.attribute_name, mr.default_color_name)) {
    GPU_vertformat_alias_add(&format, "c");
  }

  if (build_on_device) {
    GPU_vertbuf_init_build_on_device(vbo, format, len);
  }
  else {
    GPU_vertbuf_init_with_format(vbo, format);
    GPU_vertbuf_data_alloc(vbo, len);
  }
}

template<typename T>
static void extract_data_mesh_mapped_corner(const Span<T> attribute,
                                            const Span<int> indices,
                                            gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  MutableSpan data = vbo.data<VBOType>();

  if constexpr (std::is_same_v<T, VBOType>) {
    array_utils::gather(attribute, indices, data);
  }
  else {
    threading::parallel_for(indices.index_range(), 8192, [&](const IndexRange range) {
      for (const int i : range) {
        data[i] = Converter::convert(attribute[indices[i]]);
      }
    });
  }
}

template<typename T>
static void extract_data_mesh_face(const OffsetIndices<int> faces,
                                   const Span<T> attribute,
                                   gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  MutableSpan data = vbo.data<VBOType>();

  threading::parallel_for(faces.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      data.slice(faces[i]).fill(Converter::convert(attribute[i]));
    }
  });
}

template<typename T>
static void extract_data_bmesh_vert(const BMesh &bm, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  const BMFace *face;
  BMIter f_iter;
  BM_ITER_MESH (face, &f_iter, &const_cast<BMesh &>(bm), BM_FACES_OF_MESH) {
    const BMLoop *loop = BM_FACE_FIRST_LOOP(face);
    for ([[maybe_unused]] const int i : IndexRange(face->len)) {
      const T *src = static_cast<const T *>(POINTER_OFFSET(loop->v->head.data, cd_offset));
      *data = Converter::convert(*src);
      loop = loop->next;
      data++;
    }
  }
}

template<typename T>
static void extract_data_bmesh_edge(const BMesh &bm, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  const BMFace *face;
  BMIter f_iter;
  BM_ITER_MESH (face, &f_iter, &const_cast<BMesh &>(bm), BM_FACES_OF_MESH) {
    const BMLoop *loop = BM_FACE_FIRST_LOOP(face);
    for ([[maybe_unused]] const int i : IndexRange(face->len)) {
      const T &src = *static_cast<const T *>(POINTER_OFFSET(loop->e->head.data, cd_offset));
      *data = Converter::convert(src);
      loop = loop->next;
      data++;
    }
  }
}

template<typename T>
static void extract_data_bmesh_face(const BMesh &bm, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  const BMFace *face;
  BMIter f_iter;
  BM_ITER_MESH (face, &f_iter, &const_cast<BMesh &>(bm), BM_FACES_OF_MESH) {
    const T &src = *static_cast<const T *>(POINTER_OFFSET(face->head.data, cd_offset));
    std::fill_n(data, face->len, Converter::convert(src));
    data += face->len;
  }
}

template<typename T>
static void extract_data_bmesh_loop(const BMesh &bm, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  const BMFace *face;
  BMIter f_iter;
  BM_ITER_MESH (face, &f_iter, &const_cast<BMesh &>(bm), BM_FACES_OF_MESH) {
    const BMLoop *loop = BM_FACE_FIRST_LOOP(face);
    for ([[maybe_unused]] const int i : IndexRange(face->len)) {
      const T &src = *static_cast<const T *>(POINTER_OFFSET(loop->head.data, cd_offset));
      *data = Converter::convert(src);
      loop = loop->next;
      data++;
    }
  }
}

static const CustomData *get_custom_data_for_domain(const BMesh &bm, bke::AttrDomain domain)
{
  switch (domain) {
    case bke::AttrDomain::Point:
      return &bm.vdata;
    case bke::AttrDomain::Corner:
      return &bm.ldata;
    case bke::AttrDomain::Face:
      return &bm.pdata;
    case bke::AttrDomain::Edge:
      return &bm.edata;
    default:
      return nullptr;
  }
}

static void extract_attribute_no_init(const MeshRenderData &mr,
                                      const DRW_AttributeRequest &request,
                                      gpu::VertBuf &vbo)
{
  if (mr.extract_type == MeshExtractType::BMesh) {
    const CustomData &custom_data = *get_custom_data_for_domain(*mr.bm, request.domain);
    const char *name = request.attribute_name;
    const int cd_offset = CustomData_get_offset_named(&custom_data, request.cd_type, name);

    bke::attribute_math::convert_to_static_type(request.cd_type, [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
        switch (request.domain) {
          case bke::AttrDomain::Point:
            extract_data_bmesh_vert<T>(*mr.bm, cd_offset, vbo);
            break;
          case bke::AttrDomain::Edge:
            extract_data_bmesh_edge<T>(*mr.bm, cd_offset, vbo);
            break;
          case bke::AttrDomain::Face:
            extract_data_bmesh_face<T>(*mr.bm, cd_offset, vbo);
            break;
          case bke::AttrDomain::Corner:
            extract_data_bmesh_loop<T>(*mr.bm, cd_offset, vbo);
            break;
          default:
            BLI_assert_unreachable();
        }
      }
    });
  }
  else {
    const bke::AttributeAccessor attributes = mr.mesh->attributes();
    const StringRef name = request.attribute_name;
    const eCustomDataType data_type = request.cd_type;
    const GVArraySpan attribute = *attributes.lookup_or_default(name, request.domain, data_type);

    bke::attribute_math::convert_to_static_type(request.cd_type, [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
        switch (request.domain) {
          case bke::AttrDomain::Point:
            extract_data_mesh_mapped_corner(attribute.typed<T>(), mr.corner_verts, vbo);
            break;
          case bke::AttrDomain::Edge:
            extract_data_mesh_mapped_corner(attribute.typed<T>(), mr.corner_edges, vbo);
            break;
          case bke::AttrDomain::Face:
            extract_data_mesh_face(mr.faces, attribute.typed<T>(), vbo);
            break;
          case bke::AttrDomain::Corner:
            vertbuf_data_extract_direct(attribute.typed<T>(), vbo);
            break;
          default:
            BLI_assert_unreachable();
        }
      }
    });
  }
}

gpu::VertBufPtr extract_attribute(const MeshRenderData &mr, const DRW_AttributeRequest &request)
{
  gpu::VertBuf *vbo = GPU_vertbuf_calloc();
  init_vbo_for_attribute(mr, *vbo, request, false, uint32_t(mr.corners_num));
  extract_attribute_no_init(mr, request, *vbo);
  return gpu::VertBufPtr(vbo);
}

gpu::VertBufPtr extract_attribute_subdiv(const MeshRenderData &mr,
                                         const DRWSubdivCache &subdiv_cache,
                                         const DRW_AttributeRequest &request)
{

  const Mesh *coarse_mesh = subdiv_cache.mesh;

  /* Prepare VBO for coarse data. The compute shader only expects floats. */
  gpu::VertBuf *src_data = GPU_vertbuf_calloc();
  GPUVertFormat coarse_format = draw::init_format_for_attribute(request.cd_type, "data");
  GPU_vertbuf_init_with_format_ex(*src_data, coarse_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*src_data, uint32_t(coarse_mesh->corners_num));

  extract_attribute_no_init(mr, request, *src_data);

  gpu::VertBuf *vbo = GPU_vertbuf_calloc();
  init_vbo_for_attribute(mr, *vbo, request, true, subdiv_cache.num_subdiv_loops);

  /* Ensure data is uploaded properly. */
  GPU_vertbuf_tag_dirty(src_data);
  bke::attribute_math::convert_to_static_type(request.cd_type, [&](auto dummy) {
    using T = decltype(dummy);
    using Converter = AttributeConverter<T>;
    if constexpr (!std::is_void_v<typename Converter::VBOType>) {
      draw_subdiv_interp_custom_data(subdiv_cache,
                                     *src_data,
                                     *vbo,
                                     Converter::gpu_component_type,
                                     Converter::gpu_component_len,
                                     0);
    }
  });

  GPU_vertbuf_discard(src_data);
  return gpu::VertBufPtr(vbo);
}

gpu::VertBufPtr extract_attr_viewer(const MeshRenderData &mr)
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "attribute_value", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);

  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
  GPU_vertbuf_data_alloc(*vbo, mr.corners_num);
  MutableSpan vbo_data = vbo->data<ColorGeometry4f>();

  const StringRefNull attr_name = ".viewer";
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const bke::AttributeReader attribute = attributes.lookup_or_default<ColorGeometry4f>(
      attr_name, bke::AttrDomain::Corner, {1.0f, 0.0f, 1.0f, 1.0f});
  attribute.varray.materialize(vbo_data);
  return vbo;
}

/** \} */

}  // namespace blender::draw
