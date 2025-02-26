/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 * Copyright (C) 2009  VMware, Inc.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include <stdio.h>
#include <inttypes.h>  /* for PRId64 macro */

#include "glheader.h"
#include "imports.h"
#include "bufferobj.h"
#include "context.h"
#include "enable.h"
#include "enums.h"
#include "glformats.h"
#include "hash.h"
#include "image.h"
#include "macros.h"
#include "mtypes.h"
#include "varray.h"
#include "arrayobj.h"
#include "get.h"
#include "main/dispatch.h"


/** Used to do error checking for GL_EXT_vertex_array_bgra */
#define BGRA_OR_4  5


/** Used to indicate which GL datatypes are accepted by each of the
 * glVertex/Color/Attrib/EtcPointer() functions.
 */
#define BOOL_BIT                          (1 << 0)
#define BYTE_BIT                          (1 << 1)
#define UNSIGNED_BYTE_BIT                 (1 << 2)
#define SHORT_BIT                         (1 << 3)
#define UNSIGNED_SHORT_BIT                (1 << 4)
#define INT_BIT                           (1 << 5)
#define UNSIGNED_INT_BIT                  (1 << 6)
#define HALF_BIT                          (1 << 7)
#define FLOAT_BIT                         (1 << 8)
#define DOUBLE_BIT                        (1 << 9)
#define FIXED_ES_BIT                      (1 << 10)
#define FIXED_GL_BIT                      (1 << 11)
#define UNSIGNED_INT_2_10_10_10_REV_BIT   (1 << 12)
#define INT_2_10_10_10_REV_BIT            (1 << 13)
#define UNSIGNED_INT_10F_11F_11F_REV_BIT  (1 << 14)
#define ALL_TYPE_BITS                    ((1 << 15) - 1)

#define ATTRIB_FORMAT_TYPES_MASK (BYTE_BIT | UNSIGNED_BYTE_BIT | \
                                  SHORT_BIT | UNSIGNED_SHORT_BIT | \
                                  INT_BIT | UNSIGNED_INT_BIT | \
                                  HALF_BIT | FLOAT_BIT | DOUBLE_BIT | \
                                  FIXED_GL_BIT | \
                                  UNSIGNED_INT_2_10_10_10_REV_BIT | \
                                  INT_2_10_10_10_REV_BIT | \
                                  UNSIGNED_INT_10F_11F_11F_REV_BIT)

#define ATTRIB_IFORMAT_TYPES_MASK (BYTE_BIT | UNSIGNED_BYTE_BIT | \
                                   SHORT_BIT | UNSIGNED_SHORT_BIT | \
                                   INT_BIT | UNSIGNED_INT_BIT)

#define ATTRIB_LFORMAT_TYPES_MASK DOUBLE_BIT


/** Convert GL datatype enum into a <type>_BIT value seen above */
static GLbitfield
type_to_bit(const struct gl_context *ctx, GLenum type)
{
   switch (type) {
   case GL_BOOL:
      return BOOL_BIT;
   case GL_BYTE:
      return BYTE_BIT;
   case GL_UNSIGNED_BYTE:
      return UNSIGNED_BYTE_BIT;
   case GL_SHORT:
      return SHORT_BIT;
   case GL_UNSIGNED_SHORT:
      return UNSIGNED_SHORT_BIT;
   case GL_INT:
      return INT_BIT;
   case GL_UNSIGNED_INT:
      return UNSIGNED_INT_BIT;
   case GL_HALF_FLOAT:
   case GL_HALF_FLOAT_OES:
      if (ctx->Extensions.ARB_half_float_vertex)
         return HALF_BIT;
      else
         return 0x0;
   case GL_FLOAT:
      return FLOAT_BIT;
   case GL_DOUBLE:
      return DOUBLE_BIT;
   case GL_FIXED:
      return _mesa_is_desktop_gl(ctx) ? FIXED_GL_BIT : FIXED_ES_BIT;
   case GL_UNSIGNED_INT_2_10_10_10_REV:
      return UNSIGNED_INT_2_10_10_10_REV_BIT;
   case GL_INT_2_10_10_10_REV:
      return INT_2_10_10_10_REV_BIT;
   case GL_UNSIGNED_INT_10F_11F_11F_REV:
      return UNSIGNED_INT_10F_11F_11F_REV_BIT;
   default:
      return 0;
   }
}


/**
 * Depending on the position and generic0 attributes enable flags select
 * the one that is used for both attributes.
 * The generic0 attribute takes precedence.
 */
static inline void
update_attribute_map_mode(const struct gl_context *ctx,
                          struct gl_vertex_array_object *vao)
{
   /*
    * There is no need to change the mapping away from the
    * identity mapping if we are not in compat mode.
    */
   if (ctx->API != API_OPENGL_COMPAT)
      return;
   /* The generic0 attribute superseeds the position attribute */
   const GLbitfield enabled = vao->Enabled;
   if (enabled & VERT_BIT_GENERIC0)
      vao->_AttributeMapMode = ATTRIBUTE_MAP_MODE_GENERIC0;
   else if (enabled & VERT_BIT_POS)
      vao->_AttributeMapMode = ATTRIBUTE_MAP_MODE_POSITION;
   else
      vao->_AttributeMapMode = ATTRIBUTE_MAP_MODE_IDENTITY;
}


/**
 * Sets the BufferBindingIndex field for the vertex attribute given by
 * attribIndex.
 */
void
_mesa_vertex_attrib_binding(struct gl_context *ctx,
                            struct gl_vertex_array_object *vao,
                            gl_vert_attrib attribIndex,
                            GLuint bindingIndex)
{
   struct gl_array_attributes *array = &vao->VertexAttrib[attribIndex];
   assert(!vao->SharedAndImmutable);

   if (array->BufferBindingIndex != bindingIndex) {
      const GLbitfield array_bit = VERT_BIT(attribIndex);

      if (_mesa_is_bufferobj(vao->BufferBinding[bindingIndex].BufferObj))
         vao->VertexAttribBufferMask |= array_bit;
      else
         vao->VertexAttribBufferMask &= ~array_bit;

      vao->BufferBinding[array->BufferBindingIndex]._BoundArrays &= ~array_bit;
      vao->BufferBinding[bindingIndex]._BoundArrays |= array_bit;

      array->BufferBindingIndex = bindingIndex;

      vao->NewArrays |= vao->Enabled & array_bit;
   }
}


/**
 * Binds a buffer object to the vertex buffer binding point given by index,
 * and sets the Offset and Stride fields.
 */
void
_mesa_bind_vertex_buffer(struct gl_context *ctx,
                         struct gl_vertex_array_object *vao,
                         GLuint index,
                         struct gl_buffer_object *vbo,
                         GLintptr offset, GLsizei stride)
{
   assert(index < ARRAY_SIZE(vao->BufferBinding));
   assert(!vao->SharedAndImmutable);
   struct gl_vertex_buffer_binding *binding = &vao->BufferBinding[index];

   if (binding->BufferObj != vbo ||
       binding->Offset != offset ||
       binding->Stride != stride) {

      _mesa_reference_buffer_object(ctx, &binding->BufferObj, vbo);

      binding->Offset = offset;
      binding->Stride = stride;

      if (!_mesa_is_bufferobj(vbo)) {
         vao->VertexAttribBufferMask &= ~binding->_BoundArrays;
      } else {
         vao->VertexAttribBufferMask |= binding->_BoundArrays;
         vbo->UsageHistory |= USAGE_ARRAY_BUFFER;
      }

      vao->NewArrays |= vao->Enabled & binding->_BoundArrays;
   }
}


/**
 * Sets the InstanceDivisor field in the vertex buffer binding point
 * given by bindingIndex.
 */
static void
vertex_binding_divisor(struct gl_context *ctx,
                       struct gl_vertex_array_object *vao,
                       GLuint bindingIndex,
                       GLuint divisor)
{
   struct gl_vertex_buffer_binding *binding =
      &vao->BufferBinding[bindingIndex];
   assert(!vao->SharedAndImmutable);

   if (binding->InstanceDivisor != divisor) {
      binding->InstanceDivisor = divisor;
      vao->NewArrays |= vao->Enabled & binding->_BoundArrays;
   }
}


void
_mesa_set_vertex_format(struct gl_vertex_format *vertex_format,
                        GLubyte size, GLenum16 type, GLenum16 format,
                        GLboolean normalized, GLboolean integer,
                        GLboolean doubles)
{
   assert(size <= 4);
   vertex_format->Type = type;
   vertex_format->Format = format;
   vertex_format->Size = size;
   vertex_format->Normalized = normalized;
   vertex_format->Integer = integer;
   vertex_format->Doubles = doubles;
   vertex_format->_ElementSize = _mesa_bytes_per_vertex_attrib(size, type);
   assert(vertex_format->_ElementSize <= 4*sizeof(double));
}


/**
 * Examine the API profile and extensions to determine which types are legal
 * for vertex arrays.  This is called once from update_array_format().
 */
static GLbitfield
get_legal_types_mask(const struct gl_context *ctx)
{
   GLbitfield legalTypesMask = ALL_TYPE_BITS;

   if (_mesa_is_gles(ctx)) {
      legalTypesMask &= ~(FIXED_GL_BIT |
                          DOUBLE_BIT |
                          UNSIGNED_INT_10F_11F_11F_REV_BIT);

      /* GL_INT and GL_UNSIGNED_INT data is not allowed in OpenGL ES until
       * 3.0.  The 2_10_10_10 types are added in OpenGL ES 3.0 or
       * GL_OES_vertex_type_10_10_10_2.  GL_HALF_FLOAT data is not allowed
       * until 3.0 or with the GL_OES_vertex_half float extension, which isn't
       * quite as trivial as we'd like because it uses a different enum value
       * for GL_HALF_FLOAT_OES.
       */
      if (ctx->Version < 30) {
         legalTypesMask &= ~(UNSIGNED_INT_BIT |
                             INT_BIT |
                             UNSIGNED_INT_2_10_10_10_REV_BIT |
                             INT_2_10_10_10_REV_BIT);

         if (!_mesa_has_OES_vertex_half_float(ctx))
            legalTypesMask &= ~HALF_BIT;
      }
   }
   else {
      legalTypesMask &= ~FIXED_ES_BIT;

      if (!ctx->Extensions.ARB_ES2_compatibility)
         legalTypesMask &= ~FIXED_GL_BIT;

      if (!ctx->Extensions.ARB_vertex_type_2_10_10_10_rev)
         legalTypesMask &= ~(UNSIGNED_INT_2_10_10_10_REV_BIT |
                             INT_2_10_10_10_REV_BIT);

      if (!ctx->Extensions.ARB_vertex_type_10f_11f_11f_rev)
         legalTypesMask &= ~UNSIGNED_INT_10F_11F_11F_REV_BIT;
   }

   return legalTypesMask;
}

static GLenum
get_array_format(const struct gl_context *ctx, GLint sizeMax, GLint *size)
{
   GLenum format = GL_RGBA;

   /* Do size parameter checking.
    * If sizeMax = BGRA_OR_4 it means that size = GL_BGRA is legal and
    * must be handled specially.
    */
   if (ctx->Extensions.EXT_vertex_array_bgra && sizeMax == BGRA_OR_4 &&
       *size == GL_BGRA) {
      format = GL_BGRA;
      *size = 4;
   }

   return format;
}


/**
 * \param attrib         The index of the attribute array
 * \param size           Components per element (1, 2, 3 or 4)
 * \param type           Datatype of each component (GL_FLOAT, GL_INT, etc)
 * \param format         Either GL_RGBA or GL_BGRA.
 * \param normalized     Whether integer types are converted to floats in [-1, 1]
 * \param integer        Integer-valued values (will not be normalized to [-1, 1])
 * \param doubles        Double values not reduced to floats
 * \param relativeOffset Offset of the first element relative to the binding
 *                       offset.
 */
void
_mesa_update_array_format(struct gl_context *ctx,
                          struct gl_vertex_array_object *vao,
                          gl_vert_attrib attrib, GLint size, GLenum type,
                          GLenum format, GLboolean normalized,
                          GLboolean integer, GLboolean doubles,
                          GLuint relativeOffset)
{
   struct gl_array_attributes *const array = &vao->VertexAttrib[attrib];

   assert(!vao->SharedAndImmutable);
   assert(size <= 4);

   array->RelativeOffset = relativeOffset;
   _mesa_set_vertex_format(&array->Format, size, type, format,
                           normalized, integer, doubles);

   vao->NewArrays |= vao->Enabled & VERT_BIT(attrib);
}

/**
 * Does error checking of the format in an attrib array.
 *
 * Called by *Pointer() and VertexAttrib*Format().
 *
 * \param func         Name of calling function used for error reporting
 * \param attrib       The index of the attribute array
 * \param legalTypes   Bitmask of *_BIT above indicating legal datatypes
 * \param sizeMin      Min allowable size value
 * \param sizeMax      Max allowable size value (may also be BGRA_OR_4)
 * \param size         Components per element (1, 2, 3 or 4)
 * \param type         Datatype of each component (GL_FLOAT, GL_INT, etc)
 * \param normalized   Whether integer types are converted to floats in [-1, 1]
 * \param integer      Integer-valued values (will not be normalized to [-1, 1])
 * \param doubles      Double values not reduced to floats
 * \param relativeOffset Offset of the first element relative to the binding offset.
 * \return bool True if validation is successful, False otherwise.
 */
static bool
validate_array_format(struct gl_context *ctx, const char *func,
                      struct gl_vertex_array_object *vao,
                      GLuint attrib, GLbitfield legalTypesMask,
                      GLint sizeMin, GLint sizeMax,
                      GLint size, GLenum type, GLboolean normalized,
                      GLboolean integer, GLboolean doubles,
                      GLuint relativeOffset, GLenum format)
{
   GLbitfield typeBit;

   /* at most, one of these bools can be true */
   assert((int) normalized + (int) integer + (int) doubles <= 1);

   if (ctx->Array.LegalTypesMask == 0 || ctx->Array.LegalTypesMaskAPI != ctx->API) {
      /* Compute the LegalTypesMask only once, unless the context API has
       * changed, in which case we want to compute it again.  We can't do this
       * in _mesa_init_varrays() below because extensions are not yet enabled
       * at that point.
       */
      ctx->Array.LegalTypesMask = get_legal_types_mask(ctx);
      ctx->Array.LegalTypesMaskAPI = ctx->API;
   }

   legalTypesMask &= ctx->Array.LegalTypesMask;

   if (_mesa_is_gles(ctx) && sizeMax == BGRA_OR_4) {
      /* BGRA ordering is not supported in ES contexts.
       */
      sizeMax = 4;
   }

   typeBit = type_to_bit(ctx, type);
   if (typeBit == 0x0 || (typeBit & legalTypesMask) == 0x0) {
      _mesa_error(ctx, GL_INVALID_ENUM, "%s(type = %s)",
                  func, _mesa_enum_to_string(type));
      return false;
   }

   if (format == GL_BGRA) {
      /* Page 298 of the PDF of the OpenGL 4.3 (Core Profile) spec says:
       *
       * "An INVALID_OPERATION error is generated under any of the following
       *  conditions:
       *    ...
       *    • size is BGRA and type is not UNSIGNED_BYTE, INT_2_10_10_10_REV
       *      or UNSIGNED_INT_2_10_10_10_REV;
       *    ...
       *    • size is BGRA and normalized is FALSE;"
       */
      bool bgra_error = false;

      if (ctx->Extensions.ARB_vertex_type_2_10_10_10_rev) {
         if (type != GL_UNSIGNED_INT_2_10_10_10_REV &&
             type != GL_INT_2_10_10_10_REV &&
             type != GL_UNSIGNED_BYTE)
            bgra_error = true;
      } else if (type != GL_UNSIGNED_BYTE)
         bgra_error = true;

      if (bgra_error) {
         _mesa_error(ctx, GL_INVALID_OPERATION, "%s(size=GL_BGRA and type=%s)",
                     func, _mesa_enum_to_string(type));
         return false;
      }

      if (!normalized) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "%s(size=GL_BGRA and normalized=GL_FALSE)", func);
         return false;
      }
   }
   else if (size < sizeMin || size > sizeMax || size > 4) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(size=%d)", func, size);
      return false;
   }

   if (ctx->Extensions.ARB_vertex_type_2_10_10_10_rev &&
       (type == GL_UNSIGNED_INT_2_10_10_10_REV ||
        type == GL_INT_2_10_10_10_REV) && size != 4) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(size=%d)", func, size);
      return false;
   }

   /* The ARB_vertex_attrib_binding_spec says:
    *
    *   An INVALID_VALUE error is generated if <relativeoffset> is larger than
    *   the value of MAX_VERTEX_ATTRIB_RELATIVE_OFFSET.
    */
   if (relativeOffset > ctx->Const.MaxVertexAttribRelativeOffset) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(relativeOffset=%d > "
                  "GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET)",
                  func, relativeOffset);
      return false;
   }

   if (ctx->Extensions.ARB_vertex_type_10f_11f_11f_rev &&
         type == GL_UNSIGNED_INT_10F_11F_11F_REV && size != 3) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(size=%d)", func, size);
      return false;
   }

   return true;
}

/**
 * Do error checking for glVertex/Color/TexCoord/...Pointer functions.
 *
 * \param func  name of calling function used for error reporting
 * \param vao the vao to update
 * \param obj the bound buffer object
 * \param attrib  the attribute array index to update
 * \param legalTypes  bitmask of *_BIT above indicating legal datatypes
 * \param sizeMin  min allowable size value
 * \param sizeMax  max allowable size value (may also be BGRA_OR_4)
 * \param size  components per element (1, 2, 3 or 4)
 * \param type  datatype of each component (GL_FLOAT, GL_INT, etc)
 * \param stride  stride between elements, in elements
 * \param normalized  are integer types converted to floats in [-1, 1]?
 * \param integer  integer-valued values (will not be normalized to [-1,1])
 * \param doubles  Double values not reduced to floats
 * \param ptr  the address (or offset inside VBO) of the array data
 */
static void
validate_array(struct gl_context *ctx, const char *func,
               struct gl_vertex_array_object *vao,
               struct gl_buffer_object *obj,
               GLuint attrib, GLbitfield legalTypesMask,
               GLint sizeMin, GLint sizeMax,
               GLint size, GLenum type, GLsizei stride,
               GLboolean normalized, GLboolean integer, GLboolean doubles,
               const GLvoid *ptr)
{
   /* Page 407 (page 423 of the PDF) of the OpenGL 3.0 spec says:
    *
    *     "Client vertex arrays - all vertex array attribute pointers must
    *     refer to buffer objects (section 2.9.2). The default vertex array
    *     object (the name zero) is also deprecated. Calling
    *     VertexAttribPointer when no buffer object or no vertex array object
    *     is bound will generate an INVALID_OPERATION error..."
    *
    * The check for VBOs is handled below.
    */
   if (ctx->API == API_OPENGL_CORE && (vao == ctx->Array.DefaultVAO)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(no array object bound)",
                  func);
      return;
   }

   if (stride < 0) {
      _mesa_error( ctx, GL_INVALID_VALUE, "%s(stride=%d)", func, stride );
      return;
   }

   if (_mesa_is_desktop_gl(ctx) && ctx->Version >= 44 &&
       stride > ctx->Const.MaxVertexAttribStride) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(stride=%d > "
                  "GL_MAX_VERTEX_ATTRIB_STRIDE)", func, stride);
      return;
   }

   /* Page 29 (page 44 of the PDF) of the OpenGL 3.3 spec says:
    *
    *     "An INVALID_OPERATION error is generated under any of the following
    *     conditions:
    *
    *     ...
    *
    *     * any of the *Pointer commands specifying the location and
    *       organization of vertex array data are called while zero is bound
    *       to the ARRAY_BUFFER buffer object binding point (see section
    *       2.9.6), and the pointer argument is not NULL."
    */
   if (ptr != NULL && vao != ctx->Array.DefaultVAO &&
       !_mesa_is_bufferobj(obj)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(non-VBO array)", func);
      return;
   }
}


static bool
validate_array_and_format(struct gl_context *ctx, const char *func,
                          struct gl_vertex_array_object *vao,
                          struct gl_buffer_object *obj,
                          GLuint attrib, GLbitfield legalTypes,
                          GLint sizeMin, GLint sizeMax,
                          GLint size, GLenum type, GLsizei stride,
                          GLboolean normalized, GLboolean integer,
                          GLboolean doubles, GLenum format, const GLvoid *ptr)
{
   validate_array(ctx, func, vao, obj, attrib, legalTypes, sizeMin, sizeMax,
                  size, type, stride, normalized, integer, doubles, ptr);

   return validate_array_format(ctx, func, vao, attrib, legalTypes, sizeMin,
                                sizeMax, size, type, normalized, integer,
                                doubles, 0, format);
}


/**
 * Update state for glVertex/Color/TexCoord/...Pointer functions.
 *
 * \param vao the vao to update
 * \param obj the bound buffer object
 * \param attrib  the attribute array index to update
 * \param format  Either GL_RGBA or GL_BGRA.
 * \param sizeMax  max allowable size value (may also be BGRA_OR_4)
 * \param size  components per element (1, 2, 3 or 4)
 * \param type  datatype of each component (GL_FLOAT, GL_INT, etc)
 * \param stride  stride between elements, in elements
 * \param normalized  are integer types converted to floats in [-1, 1]?
 * \param integer  integer-valued values (will not be normalized to [-1,1])
 * \param doubles  Double values not reduced to floats
 * \param ptr  the address (or offset inside VBO) of the array data
 */
static void
update_array(struct gl_context *ctx,
             struct gl_vertex_array_object *vao,
             struct gl_buffer_object *obj,
             GLuint attrib, GLenum format,
             GLint sizeMax,
             GLint size, GLenum type, GLsizei stride,
             GLboolean normalized, GLboolean integer, GLboolean doubles,
             const GLvoid *ptr)
{
   _mesa_update_array_format(ctx, vao, attrib, size, type, format,
                             normalized, integer, doubles, 0);

   /* Reset the vertex attrib binding */
   _mesa_vertex_attrib_binding(ctx, vao, attrib, attrib);

   /* The Stride and Ptr fields are not set by update_array_format() */
   struct gl_array_attributes *array = &vao->VertexAttrib[attrib];
   array->Stride = stride;
   /* For updating the pointer we would need to add the vao->NewArrays flag
    * to the VAO. But but that is done already unconditionally in
    * _mesa_update_array_format called above.
    */
   assert((vao->NewArrays | ~vao->Enabled) & VERT_BIT(attrib));
   array->Ptr = ptr;

   /* Update the vertex buffer binding */
   GLsizei effectiveStride = stride != 0 ?
      stride : array->Format._ElementSize;
   _mesa_bind_vertex_buffer(ctx, vao, attrib,
                            obj, (GLintptr) ptr,
                            effectiveStride);
}


/* Helper function for all EXT_direct_state_access glVertexArray* functions */
static bool
_lookup_vao_and_vbo_dsa(struct gl_context *ctx,
                        GLuint vaobj, GLuint buffer,
                        GLintptr offset,
                        struct gl_vertex_array_object** vao,
                        struct gl_buffer_object** vbo,
                        const char* caller)
{
   *vao = _mesa_lookup_vao_err(ctx, vaobj, true, caller);
   if (!(*vao))
      return false;

   if (buffer != 0) {
      *vbo = _mesa_lookup_bufferobj(ctx, buffer);
      if (!_mesa_handle_bind_buffer_gen(ctx, buffer, vbo, caller))
         return false;

      if (offset < 0) {
         _mesa_error(ctx, GL_INVALID_VALUE,
                     "%s(negative offset with non-0 buffer)", caller);
         return false;
      }
   } else {
      *vbo = ctx->Shared->NullBufferObj;
   }

   return true;
}


void GLAPIENTRY
_mesa_VertexPointer_no_error(GLint size, GLenum type, GLsizei stride,
                             const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_POS, GL_RGBA, 4, size, type, stride,
                GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (BYTE_BIT | SHORT_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (SHORT_BIT | INT_BIT | FLOAT_BIT |
         DOUBLE_BIT | HALF_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   if (!validate_array_and_format(ctx, "glVertexPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_POS, legalTypes, 2, 4, size,
                                  type, stride, GL_FALSE, GL_FALSE, GL_FALSE,
                                  format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_POS, format, 4, size, type, stride,
                GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayVertexOffsetEXT(GLuint vaobj, GLuint buffer, GLint size,
                                 GLenum type, GLsizei stride, GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (BYTE_BIT | SHORT_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (SHORT_BIT | INT_BIT | FLOAT_BIT |
         DOUBLE_BIT | HALF_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayVertexOffsetEXT"))
      return;

   if (!validate_array_and_format(ctx, "glVertexArrayVertexOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_POS, legalTypes, 2, 4, size,
                                  type, stride, GL_FALSE, GL_FALSE, GL_FALSE,
                                  format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_POS, format, 4, size, type, stride,
                GL_FALSE, GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_NormalPointer_no_error(GLenum type, GLsizei stride, const GLvoid *ptr )
{
   GET_CURRENT_CONTEXT(ctx);

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_NORMAL, GL_RGBA, 3, 3, type, stride, GL_TRUE,
                GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_NormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr )
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (BYTE_BIT | SHORT_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (BYTE_BIT | SHORT_BIT | INT_BIT |
         HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   if (!validate_array_and_format(ctx, "glNormalPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_NORMAL, legalTypes, 3, 3, 3,
                                  type, stride, GL_TRUE, GL_FALSE,
                                  GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_NORMAL, format, 3, 3, type, stride, GL_TRUE,
                GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayNormalOffsetEXT(GLuint vaobj, GLuint buffer, GLenum type,
                                 GLsizei stride, GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (BYTE_BIT | SHORT_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (BYTE_BIT | SHORT_BIT | INT_BIT |
         HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glNormalPointer"))
      return;

   if (!validate_array_and_format(ctx, "glNormalPointer",
                                  vao, vbo,
                                  VERT_ATTRIB_NORMAL, legalTypes, 3, 3, 3,
                                  type, stride, GL_TRUE, GL_FALSE,
                                  GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_NORMAL, format, 3, 3, type, stride, GL_TRUE,
                GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_ColorPointer_no_error(GLint size, GLenum type, GLsizei stride,
                            const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_COLOR0, format, BGRA_OR_4, size,
                type, stride, GL_TRUE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_ColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);
   const GLint sizeMin = (ctx->API == API_OPENGLES) ? 4 : 3;

   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   const GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (UNSIGNED_BYTE_BIT | HALF_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (BYTE_BIT | UNSIGNED_BYTE_BIT |
         SHORT_BIT | UNSIGNED_SHORT_BIT |
         INT_BIT | UNSIGNED_INT_BIT |
         HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   if (!validate_array_and_format(ctx, "glColorPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_COLOR0, legalTypes, sizeMin,
                                  BGRA_OR_4, size, type, stride, GL_TRUE,
                                  GL_FALSE, GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_COLOR0, format, BGRA_OR_4, size,
                type, stride, GL_TRUE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayColorOffsetEXT(GLuint vaobj, GLuint buffer, GLint size,
                                GLenum type, GLsizei stride, GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);
   const GLint sizeMin = (ctx->API == API_OPENGLES) ? 4 : 3;

   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   const GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (UNSIGNED_BYTE_BIT | HALF_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (BYTE_BIT | UNSIGNED_BYTE_BIT |
         SHORT_BIT | UNSIGNED_SHORT_BIT |
         INT_BIT | UNSIGNED_INT_BIT |
         HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayColorOffsetEXT"))
      return;

   if (!validate_array_and_format(ctx, "glVertexArrayColorOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_COLOR0, legalTypes, sizeMin,
                                  BGRA_OR_4, size, type, stride, GL_TRUE,
                                  GL_FALSE, GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_COLOR0, format, BGRA_OR_4, size,
                type, stride, GL_TRUE, GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_FogCoordPointer_no_error(GLenum type, GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_FOG, GL_RGBA, 1, 1, type, stride, GL_FALSE,
                GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_FogCoordPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (HALF_BIT | FLOAT_BIT | DOUBLE_BIT);

   if (!validate_array_and_format(ctx, "glFogCoordPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_FOG, legalTypes, 1, 1, 1,
                                  type, stride, GL_FALSE, GL_FALSE,
                                  GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_FOG, format, 1, 1, type, stride, GL_FALSE,
                GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayFogCoordOffsetEXT(GLuint vaobj, GLuint buffer, GLenum type,
                                   GLsizei stride, GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (HALF_BIT | FLOAT_BIT | DOUBLE_BIT);

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayFogCoordOffsetEXT"))
      return;

   if (!validate_array_and_format(ctx, "glVertexArrayFogCoordOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_FOG, legalTypes, 1, 1, 1,
                                  type, stride, GL_FALSE, GL_FALSE,
                                  GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_FOG, format, 1, 1, type, stride, GL_FALSE,
                GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_IndexPointer_no_error(GLenum type, GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_COLOR_INDEX, GL_RGBA, 1, 1, type, stride,
                GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_IndexPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (UNSIGNED_BYTE_BIT | SHORT_BIT | INT_BIT |
                                     FLOAT_BIT | DOUBLE_BIT);

   if (!validate_array_and_format(ctx, "glIndexPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_COLOR_INDEX,
                                  legalTypes, 1, 1, 1, type, stride,
                                  GL_FALSE, GL_FALSE, GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_COLOR_INDEX, format, 1, 1, type, stride,
                GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayIndexOffsetEXT(GLuint vaobj, GLuint buffer, GLenum type,
                                GLsizei stride, GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (UNSIGNED_BYTE_BIT | SHORT_BIT | INT_BIT |
                                     FLOAT_BIT | DOUBLE_BIT);

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayIndexOffsetEXT"))
      return;

   if (!validate_array_and_format(ctx, "glVertexArrayIndexOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_COLOR_INDEX,
                                  legalTypes, 1, 1, 1, type, stride,
                                  GL_FALSE, GL_FALSE, GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_COLOR_INDEX, format, 1, 1, type, stride,
                GL_FALSE, GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_SecondaryColorPointer_no_error(GLint size, GLenum type,
                                     GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_COLOR1, format, BGRA_OR_4, size, type,
                stride, GL_TRUE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_SecondaryColorPointer(GLint size, GLenum type,
			       GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   const GLbitfield legalTypes = (BYTE_BIT | UNSIGNED_BYTE_BIT |
                                  SHORT_BIT | UNSIGNED_SHORT_BIT |
                                  INT_BIT | UNSIGNED_INT_BIT |
                                  HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
                                  UNSIGNED_INT_2_10_10_10_REV_BIT |
                                  INT_2_10_10_10_REV_BIT);

   if (!validate_array_and_format(ctx, "glSecondaryColorPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_COLOR1, legalTypes, 3,
                                  BGRA_OR_4, size, type, stride,
                                  GL_TRUE, GL_FALSE, GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_COLOR1, format, BGRA_OR_4, size, type,
                stride, GL_TRUE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArraySecondaryColorOffsetEXT(GLuint vaobj, GLuint buffer, GLint size,
                                         GLenum type, GLsizei stride, GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   const GLbitfield legalTypes = (BYTE_BIT | UNSIGNED_BYTE_BIT |
                                  SHORT_BIT | UNSIGNED_SHORT_BIT |
                                  INT_BIT | UNSIGNED_INT_BIT |
                                  HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
                                  UNSIGNED_INT_2_10_10_10_REV_BIT |
                                  INT_2_10_10_10_REV_BIT);

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArraySecondaryColorOffsetEXT"))
      return;

   if (!validate_array_and_format(ctx, "glVertexArraySecondaryColorOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_COLOR1, legalTypes, 3,
                                  BGRA_OR_4, size, type, stride,
                                  GL_TRUE, GL_FALSE, GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_COLOR1, format, BGRA_OR_4, size, type,
                stride, GL_TRUE, GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_TexCoordPointer_no_error(GLint size, GLenum type, GLsizei stride,
                               const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);
   const GLuint unit = ctx->Array.ActiveTexture;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_TEX(unit), GL_RGBA, 4, size, type,
                stride, GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_TexCoordPointer(GLint size, GLenum type, GLsizei stride,
                      const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);
   const GLint sizeMin = (ctx->API == API_OPENGLES) ? 2 : 1;
   const GLuint unit = ctx->Array.ActiveTexture;

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (BYTE_BIT | SHORT_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (SHORT_BIT | INT_BIT |
         HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   if (!validate_array_and_format(ctx, "glTexCoordPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_TEX(unit), legalTypes,
                                  sizeMin, 4, size, type, stride,
                                  GL_FALSE, GL_FALSE, GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_TEX(unit), format, 4, size, type,
                stride, GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayTexCoordOffsetEXT(GLuint vaobj, GLuint buffer, GLint size,
                                   GLenum type, GLsizei stride, GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);
   const GLint sizeMin = (ctx->API == API_OPENGLES) ? 2 : 1;
   const GLuint unit = ctx->Array.ActiveTexture;

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (BYTE_BIT | SHORT_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (SHORT_BIT | INT_BIT |
         HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayTexCoordOffsetEXT"))
      return;

   if (!validate_array_and_format(ctx, "glVertexArrayTexCoordOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_TEX(unit), legalTypes,
                                  sizeMin, 4, size, type, stride,
                                  GL_FALSE, GL_FALSE, GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_TEX(unit), format, 4, size, type,
                stride, GL_FALSE, GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_VertexArrayMultiTexCoordOffsetEXT(GLuint vaobj, GLuint buffer, GLenum texunit,
                                        GLint size, GLenum type, GLsizei stride,
                                        GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);
   const GLint sizeMin = (ctx->API == API_OPENGLES) ? 2 : 1;
   const GLuint unit = texunit - GL_TEXTURE0;

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (ctx->API == API_OPENGLES)
      ? (BYTE_BIT | SHORT_BIT | FLOAT_BIT | FIXED_ES_BIT)
      : (SHORT_BIT | INT_BIT |
         HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
         UNSIGNED_INT_2_10_10_10_REV_BIT |
         INT_2_10_10_10_REV_BIT);

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayMultiTexCoordOffsetEXT"))
      return;

   if (unit >= ctx->Const.MaxCombinedTextureImageUnits) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glVertexArrayMultiTexCoordOffsetEXT(texunit=%d)",
         texunit);
      return;
   }

   if (!validate_array_and_format(ctx, "glVertexArrayMultiTexCoordOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_TEX(unit), legalTypes,
                                  sizeMin, 4, size, type, stride,
                                  GL_FALSE, GL_FALSE, GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_TEX(unit), format, 4, size, type,
                stride, GL_FALSE, GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_EdgeFlagPointer_no_error(GLsizei stride, const GLvoid *ptr)
{
   /* this is the same type that glEdgeFlag uses */
   const GLboolean integer = GL_FALSE;
   GET_CURRENT_CONTEXT(ctx);

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_EDGEFLAG, GL_RGBA, 1, 1, GL_UNSIGNED_BYTE,
                stride, GL_FALSE, integer, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_EdgeFlagPointer(GLsizei stride, const GLvoid *ptr)
{
   /* this is the same type that glEdgeFlag uses */
   const GLboolean integer = GL_FALSE;
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = UNSIGNED_BYTE_BIT;

   if (!validate_array_and_format(ctx, "glEdgeFlagPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_EDGEFLAG, legalTypes,
                                  1, 1, 1, GL_UNSIGNED_BYTE, stride,
                                  GL_FALSE, integer, GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_EDGEFLAG, format, 1, 1, GL_UNSIGNED_BYTE,
                stride, GL_FALSE, integer, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayEdgeFlagOffsetEXT(GLuint vaobj, GLuint buffer, GLsizei stride,
                                   GLintptr offset)
{
   /* this is the same type that glEdgeFlag uses */
   const GLboolean integer = GL_FALSE;
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = UNSIGNED_BYTE_BIT;

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayEdgeFlagOffsetEXT"))
      return;

   if (!validate_array_and_format(ctx, "glVertexArrayEdgeFlagOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_EDGEFLAG, legalTypes,
                                  1, 1, 1, GL_UNSIGNED_BYTE, stride,
                                  GL_FALSE, integer, GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_EDGEFLAG, format, 1, 1, GL_UNSIGNED_BYTE,
                stride, GL_FALSE, integer, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_PointSizePointerOES_no_error(GLenum type, GLsizei stride,
                                   const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_POINT_SIZE, GL_RGBA, 1, 1, type, stride,
                GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_PointSizePointerOES(GLenum type, GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   if (ctx->API != API_OPENGLES) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glPointSizePointer(ES 1.x only)");
      return;
   }

   const GLbitfield legalTypes = (FLOAT_BIT | FIXED_ES_BIT);

   if (!validate_array_and_format(ctx, "glPointSizePointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_POINT_SIZE, legalTypes,
                                  1, 1, 1, type, stride, GL_FALSE, GL_FALSE,
                                  GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_POINT_SIZE, format, 1, 1, type, stride,
                GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexAttribPointer_no_error(GLuint index, GLint size, GLenum type,
                                   GLboolean normalized,
                                   GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_GENERIC(index), format, BGRA_OR_4,
                size, type, stride, normalized, GL_FALSE, GL_FALSE, ptr);
}


/**
 * Set a generic vertex attribute array.
 * Note that these arrays DO NOT alias the conventional GL vertex arrays
 * (position, normal, color, fog, texcoord, etc).
 */
void GLAPIENTRY
_mesa_VertexAttribPointer(GLuint index, GLint size, GLenum type,
                             GLboolean normalized,
                             GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glVertexAttribPointerARB(idx)");
      return;
   }

   const GLbitfield legalTypes = (BYTE_BIT | UNSIGNED_BYTE_BIT |
                                  SHORT_BIT | UNSIGNED_SHORT_BIT |
                                  INT_BIT | UNSIGNED_INT_BIT |
                                  HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
                                  FIXED_ES_BIT | FIXED_GL_BIT |
                                  UNSIGNED_INT_2_10_10_10_REV_BIT |
                                  INT_2_10_10_10_REV_BIT |
                                  UNSIGNED_INT_10F_11F_11F_REV_BIT);

   if (!validate_array_and_format(ctx, "glVertexAttribPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_GENERIC(index), legalTypes,
                                  1, BGRA_OR_4, size, type, stride,
                                  normalized, GL_FALSE, GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_GENERIC(index), format, BGRA_OR_4,
                size, type, stride, normalized, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayVertexAttribOffsetEXT(GLuint vaobj, GLuint buffer, GLuint index, GLint size,
                                       GLenum type, GLboolean normalized,
                                       GLsizei stride, GLintptr offset)
{
   GET_CURRENT_CONTEXT(ctx);
   GLenum format = get_array_format(ctx, BGRA_OR_4, &size);
   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayVertexAttribOffsetEXT"))
      return;

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glVertexArrayVertexAttribOffsetEXT(idx)");
      return;
   }

   const GLbitfield legalTypes = (BYTE_BIT | UNSIGNED_BYTE_BIT |
                                  SHORT_BIT | UNSIGNED_SHORT_BIT |
                                  INT_BIT | UNSIGNED_INT_BIT |
                                  HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
                                  FIXED_ES_BIT | FIXED_GL_BIT |
                                  UNSIGNED_INT_2_10_10_10_REV_BIT |
                                  INT_2_10_10_10_REV_BIT |
                                  UNSIGNED_INT_10F_11F_11F_REV_BIT);

   if (!validate_array_and_format(ctx, "glVertexArrayVertexAttribOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_GENERIC(index), legalTypes,
                                  1, BGRA_OR_4, size, type, stride,
                                  normalized, GL_FALSE, GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_GENERIC(index), format, BGRA_OR_4,
                size, type, stride, normalized, GL_FALSE, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_VertexAttribIPointer_no_error(GLuint index, GLint size, GLenum type,
                                    GLsizei stride, const GLvoid *ptr)
{
   const GLboolean normalized = GL_FALSE;
   const GLboolean integer = GL_TRUE;
   GET_CURRENT_CONTEXT(ctx);

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_GENERIC(index), GL_RGBA, 4,  size, type,
                stride, normalized, integer, GL_FALSE, ptr);
}


/**
 * GL_EXT_gpu_shader4 / GL 3.0.
 * Set an integer-valued vertex attribute array.
 * Note that these arrays DO NOT alias the conventional GL vertex arrays
 * (position, normal, color, fog, texcoord, etc).
 */
void GLAPIENTRY
_mesa_VertexAttribIPointer(GLuint index, GLint size, GLenum type,
                           GLsizei stride, const GLvoid *ptr)
{
   const GLboolean normalized = GL_FALSE;
   const GLboolean integer = GL_TRUE;
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glVertexAttribIPointer(index)");
      return;
   }

   const GLbitfield legalTypes = (BYTE_BIT | UNSIGNED_BYTE_BIT |
                                  SHORT_BIT | UNSIGNED_SHORT_BIT |
                                  INT_BIT | UNSIGNED_INT_BIT);

   if (!validate_array_and_format(ctx, "glVertexAttribIPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_GENERIC(index), legalTypes,
                                  1, 4, size, type, stride,
                                  normalized, integer, GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_GENERIC(index), format, 4,  size, type,
                stride, normalized, integer, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_VertexAttribLPointer_no_error(GLuint index, GLint size, GLenum type,
                                    GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_GENERIC(index), GL_RGBA, 4, size, type,
                stride, GL_FALSE, GL_FALSE, GL_TRUE, ptr);
}


void GLAPIENTRY
_mesa_VertexArrayVertexAttribIOffsetEXT(GLuint vaobj, GLuint buffer, GLuint index, GLint size,
                                        GLenum type, GLsizei stride, GLintptr offset)
{
   const GLboolean normalized = GL_FALSE;
   const GLboolean integer = GL_TRUE;
   GET_CURRENT_CONTEXT(ctx);
   GLenum format = GL_RGBA;

   struct gl_vertex_array_object* vao;
   struct gl_buffer_object* vbo;

   if (!_lookup_vao_and_vbo_dsa(ctx, vaobj, buffer, offset,
                                &vao, &vbo,
                                "glVertexArrayVertexAttribIOffsetEXT"))
      return;

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glVertexArrayVertexAttribIOffsetEXT(index)");
      return;
   }

   const GLbitfield legalTypes = (BYTE_BIT | UNSIGNED_BYTE_BIT |
                                  SHORT_BIT | UNSIGNED_SHORT_BIT |
                                  INT_BIT | UNSIGNED_INT_BIT);

   if (!validate_array_and_format(ctx, "glVertexArrayVertexAttribIOffsetEXT",
                                  vao, vbo,
                                  VERT_ATTRIB_GENERIC(index), legalTypes,
                                  1, 4, size, type, stride,
                                  normalized, integer, GL_FALSE, format, (void*) offset))
      return;

   update_array(ctx, vao, vbo,
                VERT_ATTRIB_GENERIC(index), format, 4,  size, type,
                stride, normalized, integer, GL_FALSE, (void*) offset);
}


void GLAPIENTRY
_mesa_VertexAttribLPointer(GLuint index, GLint size, GLenum type,
                           GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);

   GLenum format = GL_RGBA;
   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glVertexAttribLPointer(index)");
      return;
   }

   const GLbitfield legalTypes = DOUBLE_BIT;

   if (!validate_array_and_format(ctx, "glVertexAttribLPointer",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_GENERIC(index), legalTypes,
                                  1, 4, size, type, stride,
                                  GL_FALSE, GL_FALSE, GL_TRUE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_GENERIC(index), format, 4, size, type,
                stride, GL_FALSE, GL_FALSE, GL_TRUE, ptr);
}


void
_mesa_enable_vertex_array_attribs(struct gl_context *ctx,
                                  struct gl_vertex_array_object *vao,
                                  GLbitfield attrib_bits)
{
   assert((attrib_bits & ~VERT_BIT_ALL) == 0);
   assert(!vao->SharedAndImmutable);

   /* Only work on bits that are disabled */
   attrib_bits &= ~vao->Enabled;
   if (attrib_bits) {
      /* was disabled, now being enabled */
      vao->Enabled |= attrib_bits;
      vao->NewArrays |= attrib_bits;

      /* Update the map mode if needed */
      if (attrib_bits & (VERT_BIT_POS|VERT_BIT_GENERIC0))
         update_attribute_map_mode(ctx, vao);
   }
}

static void
enable_vertex_array_attrib(struct gl_context *ctx,
                           struct gl_vertex_array_object *vao,
                           GLuint index,
                           const char *func)
{
   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(index)", func);
      return;
   }

   _mesa_enable_vertex_array_attrib(ctx, vao, VERT_ATTRIB_GENERIC(index));
}


void GLAPIENTRY
_mesa_EnableVertexAttribArray(GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   enable_vertex_array_attrib(ctx, ctx->Array.VAO, index,
                              "glEnableVertexAttribArray");
}


void GLAPIENTRY
_mesa_EnableVertexAttribArray_no_error(GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_enable_vertex_array_attrib(ctx, ctx->Array.VAO,
                                    VERT_ATTRIB_GENERIC(index));
}


void GLAPIENTRY
_mesa_EnableVertexArrayAttrib(GLuint vaobj, GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao;

   /* The ARB_direct_state_access specification says:
    *
    *   "An INVALID_OPERATION error is generated by EnableVertexArrayAttrib
    *    and DisableVertexArrayAttrib if <vaobj> is not
    *    [compatibility profile: zero or] the name of an existing vertex
    *    array object."
    */
   vao = _mesa_lookup_vao_err(ctx, vaobj, false, "glEnableVertexArrayAttrib");
   if (!vao)
      return;

   enable_vertex_array_attrib(ctx, vao, index, "glEnableVertexArrayAttrib");
}

void GLAPIENTRY
_mesa_EnableVertexArrayAttribEXT(GLuint vaobj, GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object* vao = _mesa_lookup_vao_err(ctx, vaobj,
                                                             true,
                                                             "glEnableVertexArrayAttribEXT");
   if (!vao)
      return;

   enable_vertex_array_attrib(ctx, vao, index, "glEnableVertexArrayAttribEXT");
}


void GLAPIENTRY
_mesa_EnableVertexArrayAttrib_no_error(GLuint vaobj, GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao = _mesa_lookup_vao(ctx, vaobj);
   _mesa_enable_vertex_array_attrib(ctx, vao, VERT_ATTRIB_GENERIC(index));
}


void
_mesa_disable_vertex_array_attribs(struct gl_context *ctx,
                                   struct gl_vertex_array_object *vao,
                                   GLbitfield attrib_bits)
{
   assert((attrib_bits & ~VERT_BIT_ALL) == 0);
   assert(!vao->SharedAndImmutable);

   /* Only work on bits that are enabled */
   attrib_bits &= vao->Enabled;
   if (attrib_bits) {
      /* was enabled, now being disabled */
      vao->Enabled &= ~attrib_bits;
      vao->NewArrays |= attrib_bits;

      /* Update the map mode if needed */
      if (attrib_bits & (VERT_BIT_POS|VERT_BIT_GENERIC0))
         update_attribute_map_mode(ctx, vao);
   }
}


void GLAPIENTRY
_mesa_DisableVertexAttribArray(GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glDisableVertexAttribArray(index)");
      return;
   }

   const gl_vert_attrib attrib = VERT_ATTRIB_GENERIC(index);
   _mesa_disable_vertex_array_attrib(ctx, ctx->Array.VAO, attrib);
}


void GLAPIENTRY
_mesa_DisableVertexAttribArray_no_error(GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   const gl_vert_attrib attrib = VERT_ATTRIB_GENERIC(index);
   _mesa_disable_vertex_array_attrib(ctx, ctx->Array.VAO, attrib);
}


void GLAPIENTRY
_mesa_DisableVertexArrayAttrib(GLuint vaobj, GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao;

   /* The ARB_direct_state_access specification says:
    *
    *   "An INVALID_OPERATION error is generated by EnableVertexArrayAttrib
    *    and DisableVertexArrayAttrib if <vaobj> is not
    *    [compatibility profile: zero or] the name of an existing vertex
    *    array object."
    */
   vao = _mesa_lookup_vao_err(ctx, vaobj, false, "glDisableVertexArrayAttrib");
   if (!vao)
      return;

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glDisableVertexArrayAttrib(index)");
      return;
   }

   const gl_vert_attrib attrib = VERT_ATTRIB_GENERIC(index);
   _mesa_disable_vertex_array_attrib(ctx, vao, attrib);
}

void GLAPIENTRY
_mesa_DisableVertexArrayAttribEXT(GLuint vaobj, GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object* vao = _mesa_lookup_vao_err(ctx, vaobj,
                                                             true,
                                                             "glEnableVertexArrayAttribEXT");
   if (!vao)
      return;

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glDisableVertexArrayAttrib(index)");
      return;
   }

   const gl_vert_attrib attrib = VERT_ATTRIB_GENERIC(index);
   _mesa_disable_vertex_array_attrib(ctx, vao, attrib);
}


void GLAPIENTRY
_mesa_DisableVertexArrayAttrib_no_error(GLuint vaobj, GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao = _mesa_lookup_vao(ctx, vaobj);
   const gl_vert_attrib attrib = VERT_ATTRIB_GENERIC(index);
   _mesa_disable_vertex_array_attrib(ctx, vao, attrib);
}


/**
 * Return info for a vertex attribute array (no alias with legacy
 * vertex attributes (pos, normal, color, etc)).  This function does
 * not handle the 4-element GL_CURRENT_VERTEX_ATTRIB_ARB query.
 */
static GLuint
get_vertex_array_attrib(struct gl_context *ctx,
                        const struct gl_vertex_array_object *vao,
                        GLuint index, GLenum pname,
                        const char *caller)
{
   const struct gl_array_attributes *array;

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(index=%u)", caller, index);
      return 0;
   }

   assert(VERT_ATTRIB_GENERIC(index) < ARRAY_SIZE(vao->VertexAttrib));

   array = &vao->VertexAttrib[VERT_ATTRIB_GENERIC(index)];

   switch (pname) {
   case GL_VERTEX_ATTRIB_ARRAY_ENABLED_ARB:
      return !!(vao->Enabled & VERT_BIT_GENERIC(index));
   case GL_VERTEX_ATTRIB_ARRAY_SIZE_ARB:
      return (array->Format.Format == GL_BGRA) ? GL_BGRA : array->Format.Size;
   case GL_VERTEX_ATTRIB_ARRAY_STRIDE_ARB:
      return array->Stride;
   case GL_VERTEX_ATTRIB_ARRAY_TYPE_ARB:
      return array->Format.Type;
   case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED_ARB:
      return array->Format.Normalized;
   case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING_ARB:
      return vao->BufferBinding[array->BufferBindingIndex].BufferObj->Name;
   case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
      if ((_mesa_is_desktop_gl(ctx)
           && (ctx->Version >= 30 || ctx->Extensions.EXT_gpu_shader4))
          || _mesa_is_gles3(ctx)) {
         return array->Format.Integer;
      }
      goto error;
   case GL_VERTEX_ATTRIB_ARRAY_LONG:
      if (_mesa_is_desktop_gl(ctx)) {
         return array->Format.Doubles;
      }
      goto error;
   case GL_VERTEX_ATTRIB_ARRAY_DIVISOR_ARB:
      if ((_mesa_is_desktop_gl(ctx) && ctx->Extensions.ARB_instanced_arrays)
          || _mesa_is_gles3(ctx)) {
         return vao->BufferBinding[array->BufferBindingIndex].InstanceDivisor;
      }
      goto error;
   case GL_VERTEX_ATTRIB_BINDING:
      if (_mesa_is_desktop_gl(ctx) || _mesa_is_gles31(ctx)) {
         return array->BufferBindingIndex - VERT_ATTRIB_GENERIC0;
      }
      goto error;
   case GL_VERTEX_ATTRIB_RELATIVE_OFFSET:
      if (_mesa_is_desktop_gl(ctx) || _mesa_is_gles31(ctx)) {
         return array->RelativeOffset;
      }
      goto error;
   default:
      ; /* fall-through */
   }

error:
   _mesa_error(ctx, GL_INVALID_ENUM, "%s(pname=0x%x)", caller, pname);
   return 0;
}


static const GLfloat *
get_current_attrib(struct gl_context *ctx, GLuint index, const char *function)
{
   if (index == 0) {
      if (_mesa_attr_zero_aliases_vertex(ctx)) {
	 _mesa_error(ctx, GL_INVALID_OPERATION, "%s(index==0)", function);
	 return NULL;
      }
   }
   else if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE,
		  "%s(index>=GL_MAX_VERTEX_ATTRIBS)", function);
      return NULL;
   }

   assert(VERT_ATTRIB_GENERIC(index) <
          ARRAY_SIZE(ctx->Array.VAO->VertexAttrib));

   FLUSH_CURRENT(ctx, 0);
   return ctx->Current.Attrib[VERT_ATTRIB_GENERIC(index)];
}

void GLAPIENTRY
_mesa_GetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (pname == GL_CURRENT_VERTEX_ATTRIB_ARB) {
      const GLfloat *v = get_current_attrib(ctx, index, "glGetVertexAttribfv");
      if (v != NULL) {
         COPY_4V(params, v);
      }
   }
   else {
      params[0] = (GLfloat) get_vertex_array_attrib(ctx, ctx->Array.VAO,
                                                    index, pname,
                                                    "glGetVertexAttribfv");
   }
}


void GLAPIENTRY
_mesa_GetVertexAttribdv(GLuint index, GLenum pname, GLdouble *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (pname == GL_CURRENT_VERTEX_ATTRIB_ARB) {
      const GLfloat *v = get_current_attrib(ctx, index, "glGetVertexAttribdv");
      if (v != NULL) {
         params[0] = (GLdouble) v[0];
         params[1] = (GLdouble) v[1];
         params[2] = (GLdouble) v[2];
         params[3] = (GLdouble) v[3];
      }
   }
   else {
      params[0] = (GLdouble) get_vertex_array_attrib(ctx, ctx->Array.VAO,
                                                     index, pname,
                                                     "glGetVertexAttribdv");
   }
}

void GLAPIENTRY
_mesa_GetVertexAttribLdv(GLuint index, GLenum pname, GLdouble *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (pname == GL_CURRENT_VERTEX_ATTRIB_ARB) {
      const GLdouble *v =
         (const GLdouble *)get_current_attrib(ctx, index,
                                              "glGetVertexAttribLdv");
      if (v != NULL) {
         params[0] = v[0];
         params[1] = v[1];
         params[2] = v[2];
         params[3] = v[3];
      }
   }
   else {
      params[0] = (GLdouble) get_vertex_array_attrib(ctx, ctx->Array.VAO,
                                                     index, pname,
                                                     "glGetVertexAttribLdv");
   }
}

void GLAPIENTRY
_mesa_GetVertexAttribiv(GLuint index, GLenum pname, GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (pname == GL_CURRENT_VERTEX_ATTRIB_ARB) {
      const GLfloat *v = get_current_attrib(ctx, index, "glGetVertexAttribiv");
      if (v != NULL) {
         /* XXX should floats in[0,1] be scaled to full int range? */
         params[0] = (GLint) v[0];
         params[1] = (GLint) v[1];
         params[2] = (GLint) v[2];
         params[3] = (GLint) v[3];
      }
   }
   else {
      params[0] = (GLint) get_vertex_array_attrib(ctx, ctx->Array.VAO,
                                                  index, pname,
                                                  "glGetVertexAttribiv");
   }
}

void GLAPIENTRY
_mesa_GetVertexAttribLui64vARB(GLuint index, GLenum pname, GLuint64EXT *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (pname == GL_CURRENT_VERTEX_ATTRIB_ARB) {
      const GLuint64 *v =
         (const GLuint64 *)get_current_attrib(ctx, index,
                                              "glGetVertexAttribLui64vARB");
      if (v != NULL) {
         params[0] = v[0];
         params[1] = v[1];
         params[2] = v[2];
         params[3] = v[3];
      }
   }
   else {
      params[0] = (GLuint64) get_vertex_array_attrib(ctx, ctx->Array.VAO,
                                                     index, pname,
                                                     "glGetVertexAttribLui64vARB");
   }
}


/** GL 3.0 */
void GLAPIENTRY
_mesa_GetVertexAttribIiv(GLuint index, GLenum pname, GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (pname == GL_CURRENT_VERTEX_ATTRIB_ARB) {
      const GLint *v = (const GLint *)
	 get_current_attrib(ctx, index, "glGetVertexAttribIiv");
      if (v != NULL) {
         COPY_4V(params, v);
      }
   }
   else {
      params[0] = (GLint) get_vertex_array_attrib(ctx, ctx->Array.VAO,
                                                  index, pname,
                                                  "glGetVertexAttribIiv");
   }
}


/** GL 3.0 */
void GLAPIENTRY
_mesa_GetVertexAttribIuiv(GLuint index, GLenum pname, GLuint *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (pname == GL_CURRENT_VERTEX_ATTRIB_ARB) {
      const GLuint *v = (const GLuint *)
	 get_current_attrib(ctx, index, "glGetVertexAttribIuiv");
      if (v != NULL) {
         COPY_4V(params, v);
      }
   }
   else {
      params[0] = get_vertex_array_attrib(ctx, ctx->Array.VAO,
                                          index, pname,
                                          "glGetVertexAttribIuiv");
   }
}


void GLAPIENTRY
_mesa_GetVertexAttribPointerv(GLuint index, GLenum pname, GLvoid **pointer)
{
   GET_CURRENT_CONTEXT(ctx);

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glGetVertexAttribPointerARB(index)");
      return;
   }

   if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER_ARB) {
      _mesa_error(ctx, GL_INVALID_ENUM, "glGetVertexAttribPointerARB(pname)");
      return;
   }

   assert(VERT_ATTRIB_GENERIC(index) <
          ARRAY_SIZE(ctx->Array.VAO->VertexAttrib));

   *pointer = (GLvoid *)
      ctx->Array.VAO->VertexAttrib[VERT_ATTRIB_GENERIC(index)].Ptr;
}


/** ARB_direct_state_access */
void GLAPIENTRY
_mesa_GetVertexArrayIndexediv(GLuint vaobj, GLuint index,
                              GLenum pname, GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao;

   /* The ARB_direct_state_access specification says:
    *
    *    "An INVALID_OPERATION error is generated if <vaobj> is not
    *     [compatibility profile: zero or] the name of an existing
    *     vertex array object."
    */
   vao = _mesa_lookup_vao_err(ctx, vaobj, false, "glGetVertexArrayIndexediv");
   if (!vao)
      return;

   /* The ARB_direct_state_access specification says:
    *
    *    "For GetVertexArrayIndexediv, <pname> must be one of
    *     VERTEX_ATTRIB_ARRAY_ENABLED, VERTEX_ATTRIB_ARRAY_SIZE,
    *     VERTEX_ATTRIB_ARRAY_STRIDE, VERTEX_ATTRIB_ARRAY_TYPE,
    *     VERTEX_ATTRIB_ARRAY_NORMALIZED, VERTEX_ATTRIB_ARRAY_INTEGER,
    *     VERTEX_ATTRIB_ARRAY_LONG, VERTEX_ATTRIB_ARRAY_DIVISOR, or
    *     VERTEX_ATTRIB_RELATIVE_OFFSET."
    *
    * and:
    *
    *    "Add GetVertexArrayIndexediv in 'Get Command' for
    *     VERTEX_ATTRIB_ARRAY_BUFFER_BINDING
    *     VERTEX_ATTRIB_BINDING,
    *     VERTEX_ATTRIB_RELATIVE_OFFSET,
    *     VERTEX_BINDING_OFFSET, and
    *     VERTEX_BINDING_STRIDE states"
    *
    * The only parameter name common to both lists is
    * VERTEX_ATTRIB_RELATIVE_OFFSET.  Also note that VERTEX_BINDING_BUFFER
    * and VERTEX_BINDING_DIVISOR are missing from both lists.  It seems
    * pretty clear however that the intent is that it should be possible
    * to query all vertex attrib and binding states that can be set with
    * a DSA function.
    */
   switch (pname) {
   case GL_VERTEX_BINDING_OFFSET:
      params[0] = vao->BufferBinding[VERT_ATTRIB_GENERIC(index)].Offset;
      break;
   case GL_VERTEX_BINDING_STRIDE:
      params[0] = vao->BufferBinding[VERT_ATTRIB_GENERIC(index)].Stride;
      break;
   case GL_VERTEX_BINDING_DIVISOR:
      params[0] = vao->BufferBinding[VERT_ATTRIB_GENERIC(index)].InstanceDivisor;
      break;
   case GL_VERTEX_BINDING_BUFFER:
      params[0] = vao->BufferBinding[VERT_ATTRIB_GENERIC(index)].BufferObj->Name;
      break;
   default:
      params[0] = get_vertex_array_attrib(ctx, vao, index, pname,
                                          "glGetVertexArrayIndexediv");
      break;
   }
}


void GLAPIENTRY
_mesa_GetVertexArrayIndexed64iv(GLuint vaobj, GLuint index,
                                GLenum pname, GLint64 *params)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao;

   /* The ARB_direct_state_access specification says:
    *
    *    "An INVALID_OPERATION error is generated if <vaobj> is not
    *     [compatibility profile: zero or] the name of an existing
    *     vertex array object."
    */
   vao = _mesa_lookup_vao_err(ctx, vaobj, false, "glGetVertexArrayIndexed64iv");
   if (!vao)
      return;

   /* The ARB_direct_state_access specification says:
    *
    *    "For GetVertexArrayIndexed64iv, <pname> must be
    *     VERTEX_BINDING_OFFSET."
    *
    * and:
    *
    *    "An INVALID_ENUM error is generated if <pname> is not one of
    *     the valid values listed above for the corresponding command."
    */
   if (pname != GL_VERTEX_BINDING_OFFSET) {
      _mesa_error(ctx, GL_INVALID_ENUM, "glGetVertexArrayIndexed64iv("
                  "pname != GL_VERTEX_BINDING_OFFSET)");
      return;
   }

   /* The ARB_direct_state_access specification says:
    *
    *    "An INVALID_VALUE error is generated if <index> is greater than
    *     or equal to the value of MAX_VERTEX_ATTRIBS."
    *
    * Since the index refers to a buffer binding in this case, the intended
    * limit must be MAX_VERTEX_ATTRIB_BINDINGS.  Both limits are currently
    * required to be the same, so in practice this doesn't matter.
    */
   if (index >= ctx->Const.MaxVertexAttribBindings) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glGetVertexArrayIndexed64iv(index"
                  "%d >= the value of GL_MAX_VERTEX_ATTRIB_BINDINGS (%d))",
                  index, ctx->Const.MaxVertexAttribBindings);
      return;
   }

   params[0] = vao->BufferBinding[VERT_ATTRIB_GENERIC(index)].Offset;
}


void GLAPIENTRY
_mesa_VertexPointerEXT(GLint size, GLenum type, GLsizei stride,
                       GLsizei count, const GLvoid *ptr)
{
   (void) count;
   _mesa_VertexPointer(size, type, stride, ptr);
}


void GLAPIENTRY
_mesa_NormalPointerEXT(GLenum type, GLsizei stride, GLsizei count,
                       const GLvoid *ptr)
{
   (void) count;
   _mesa_NormalPointer(type, stride, ptr);
}


void GLAPIENTRY
_mesa_ColorPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count,
                      const GLvoid *ptr)
{
   (void) count;
   _mesa_ColorPointer(size, type, stride, ptr);
}


void GLAPIENTRY
_mesa_IndexPointerEXT(GLenum type, GLsizei stride, GLsizei count,
                      const GLvoid *ptr)
{
   (void) count;
   _mesa_IndexPointer(type, stride, ptr);
}


void GLAPIENTRY
_mesa_TexCoordPointerEXT(GLint size, GLenum type, GLsizei stride,
                         GLsizei count, const GLvoid *ptr)
{
   (void) count;
   _mesa_TexCoordPointer(size, type, stride, ptr);
}


void GLAPIENTRY
_mesa_MultiTexCoordPointerEXT(GLenum texunit, GLint size, GLenum type,
                              GLsizei stride, const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);
   const GLint sizeMin = 1;
   const GLuint unit = texunit - GL_TEXTURE0;

   GLenum format = GL_RGBA;
   const GLbitfield legalTypes = (SHORT_BIT | INT_BIT |
                                  HALF_BIT | FLOAT_BIT | DOUBLE_BIT |
                                  UNSIGNED_INT_2_10_10_10_REV_BIT |
                                  INT_2_10_10_10_REV_BIT);

   if (!validate_array_and_format(ctx, "glMultiTexCoordPointerEXT",
                                  ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                                  VERT_ATTRIB_TEX(unit), legalTypes,
                                  sizeMin, 4, size, type, stride,
                                  GL_FALSE, GL_FALSE, GL_FALSE, format, ptr))
      return;

   update_array(ctx, ctx->Array.VAO, ctx->Array.ArrayBufferObj,
                VERT_ATTRIB_TEX(unit), format, 4, size, type,
                stride, GL_FALSE, GL_FALSE, GL_FALSE, ptr);
}


void GLAPIENTRY
_mesa_EdgeFlagPointerEXT(GLsizei stride, GLsizei count, const GLboolean *ptr)
{
   (void) count;
   _mesa_EdgeFlagPointer(stride, ptr);
}


void GLAPIENTRY
_mesa_InterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer)
{
   GET_CURRENT_CONTEXT(ctx);
   GLboolean tflag, cflag, nflag;  /* enable/disable flags */
   GLint tcomps, ccomps, vcomps;   /* components per texcoord, color, vertex */
   GLenum ctype = 0;               /* color type */
   GLint coffset = 0, noffset = 0, voffset;/* color, normal, vertex offsets */
   const GLint toffset = 0;        /* always zero */
   GLint defstride;                /* default stride */
   GLint c, f;

   f = sizeof(GLfloat);
   c = f * ((4 * sizeof(GLubyte) + (f - 1)) / f);

   if (stride < 0) {
      _mesa_error( ctx, GL_INVALID_VALUE, "glInterleavedArrays(stride)" );
      return;
   }

   switch (format) {
      case GL_V2F:
         tflag = GL_FALSE;  cflag = GL_FALSE;  nflag = GL_FALSE;
         tcomps = 0;  ccomps = 0;  vcomps = 2;
         voffset = 0;
         defstride = 2*f;
         break;
      case GL_V3F:
         tflag = GL_FALSE;  cflag = GL_FALSE;  nflag = GL_FALSE;
         tcomps = 0;  ccomps = 0;  vcomps = 3;
         voffset = 0;
         defstride = 3*f;
         break;
      case GL_C4UB_V2F:
         tflag = GL_FALSE;  cflag = GL_TRUE;  nflag = GL_FALSE;
         tcomps = 0;  ccomps = 4;  vcomps = 2;
         ctype = GL_UNSIGNED_BYTE;
         coffset = 0;
         voffset = c;
         defstride = c + 2*f;
         break;
      case GL_C4UB_V3F:
         tflag = GL_FALSE;  cflag = GL_TRUE;  nflag = GL_FALSE;
         tcomps = 0;  ccomps = 4;  vcomps = 3;
         ctype = GL_UNSIGNED_BYTE;
         coffset = 0;
         voffset = c;
         defstride = c + 3*f;
         break;
      case GL_C3F_V3F:
         tflag = GL_FALSE;  cflag = GL_TRUE;  nflag = GL_FALSE;
         tcomps = 0;  ccomps = 3;  vcomps = 3;
         ctype = GL_FLOAT;
         coffset = 0;
         voffset = 3*f;
         defstride = 6*f;
         break;
      case GL_N3F_V3F:
         tflag = GL_FALSE;  cflag = GL_FALSE;  nflag = GL_TRUE;
         tcomps = 0;  ccomps = 0;  vcomps = 3;
         noffset = 0;
         voffset = 3*f;
         defstride = 6*f;
         break;
      case GL_C4F_N3F_V3F:
         tflag = GL_FALSE;  cflag = GL_TRUE;  nflag = GL_TRUE;
         tcomps = 0;  ccomps = 4;  vcomps = 3;
         ctype = GL_FLOAT;
         coffset = 0;
         noffset = 4*f;
         voffset = 7*f;
         defstride = 10*f;
         break;
      case GL_T2F_V3F:
         tflag = GL_TRUE;  cflag = GL_FALSE;  nflag = GL_FALSE;
         tcomps = 2;  ccomps = 0;  vcomps = 3;
         voffset = 2*f;
         defstride = 5*f;
         break;
      case GL_T4F_V4F:
         tflag = GL_TRUE;  cflag = GL_FALSE;  nflag = GL_FALSE;
         tcomps = 4;  ccomps = 0;  vcomps = 4;
         voffset = 4*f;
         defstride = 8*f;
         break;
      case GL_T2F_C4UB_V3F:
         tflag = GL_TRUE;  cflag = GL_TRUE;  nflag = GL_FALSE;
         tcomps = 2;  ccomps = 4;  vcomps = 3;
         ctype = GL_UNSIGNED_BYTE;
         coffset = 2*f;
         voffset = c+2*f;
         defstride = c+5*f;
         break;
      case GL_T2F_C3F_V3F:
         tflag = GL_TRUE;  cflag = GL_TRUE;  nflag = GL_FALSE;
         tcomps = 2;  ccomps = 3;  vcomps = 3;
         ctype = GL_FLOAT;
         coffset = 2*f;
         voffset = 5*f;
         defstride = 8*f;
         break;
      case GL_T2F_N3F_V3F:
         tflag = GL_TRUE;  cflag = GL_FALSE;  nflag = GL_TRUE;
         tcomps = 2;  ccomps = 0;  vcomps = 3;
         noffset = 2*f;
         voffset = 5*f;
         defstride = 8*f;
         break;
      case GL_T2F_C4F_N3F_V3F:
         tflag = GL_TRUE;  cflag = GL_TRUE;  nflag = GL_TRUE;
         tcomps = 2;  ccomps = 4;  vcomps = 3;
         ctype = GL_FLOAT;
         coffset = 2*f;
         noffset = 6*f;
         voffset = 9*f;
         defstride = 12*f;
         break;
      case GL_T4F_C4F_N3F_V4F:
         tflag = GL_TRUE;  cflag = GL_TRUE;  nflag = GL_TRUE;
         tcomps = 4;  ccomps = 4;  vcomps = 4;
         ctype = GL_FLOAT;
         coffset = 4*f;
         noffset = 8*f;
         voffset = 11*f;
         defstride = 15*f;
         break;
      default:
         _mesa_error( ctx, GL_INVALID_ENUM, "glInterleavedArrays(format)" );
         return;
   }

   if (stride==0) {
      stride = defstride;
   }

   _mesa_DisableClientState( GL_EDGE_FLAG_ARRAY );
   _mesa_DisableClientState( GL_INDEX_ARRAY );
   /* XXX also disable secondary color and generic arrays? */

   /* Texcoords */
   if (tflag) {
      _mesa_EnableClientState( GL_TEXTURE_COORD_ARRAY );
      _mesa_TexCoordPointer( tcomps, GL_FLOAT, stride,
                             (GLubyte *) pointer + toffset );
   }
   else {
      _mesa_DisableClientState( GL_TEXTURE_COORD_ARRAY );
   }

   /* Color */
   if (cflag) {
      _mesa_EnableClientState( GL_COLOR_ARRAY );
      _mesa_ColorPointer( ccomps, ctype, stride,
			  (GLubyte *) pointer + coffset );
   }
   else {
      _mesa_DisableClientState( GL_COLOR_ARRAY );
   }


   /* Normals */
   if (nflag) {
      _mesa_EnableClientState( GL_NORMAL_ARRAY );
      _mesa_NormalPointer( GL_FLOAT, stride, (GLubyte *) pointer + noffset );
   }
   else {
      _mesa_DisableClientState( GL_NORMAL_ARRAY );
   }

   /* Vertices */
   _mesa_EnableClientState( GL_VERTEX_ARRAY );
   _mesa_VertexPointer( vcomps, GL_FLOAT, stride,
			(GLubyte *) pointer + voffset );
}


void GLAPIENTRY
_mesa_LockArraysEXT(GLint first, GLsizei count)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glLockArrays %d %d\n", first, count);

   if (first < 0) {
      _mesa_error( ctx, GL_INVALID_VALUE, "glLockArraysEXT(first)" );
      return;
   }
   if (count <= 0) {
      _mesa_error( ctx, GL_INVALID_VALUE, "glLockArraysEXT(count)" );
      return;
   }
   if (ctx->Array.LockCount != 0) {
      _mesa_error( ctx, GL_INVALID_OPERATION, "glLockArraysEXT(reentry)" );
      return;
   }

   ctx->Array.LockFirst = first;
   ctx->Array.LockCount = count;
}


void GLAPIENTRY
_mesa_UnlockArraysEXT( void )
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API)
      _mesa_debug(ctx, "glUnlockArrays\n");

   if (ctx->Array.LockCount == 0) {
      _mesa_error( ctx, GL_INVALID_OPERATION, "glUnlockArraysEXT(reexit)" );
      return;
   }

   ctx->Array.LockFirst = 0;
   ctx->Array.LockCount = 0;
}


static void
primitive_restart_index(struct gl_context *ctx, GLuint index)
{
   ctx->Array.RestartIndex = index;
}


/**
 * GL_NV_primitive_restart and GL 3.1
 */
void GLAPIENTRY
_mesa_PrimitiveRestartIndex_no_error(GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);
   primitive_restart_index(ctx, index);
}


void GLAPIENTRY
_mesa_PrimitiveRestartIndex(GLuint index)
{
   GET_CURRENT_CONTEXT(ctx);

   if (!ctx->Extensions.NV_primitive_restart && ctx->Version < 31) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glPrimitiveRestartIndexNV()");
      return;
   }

   primitive_restart_index(ctx, index);
}


void GLAPIENTRY
_mesa_VertexAttribDivisor_no_error(GLuint index, GLuint divisor)
{
   GET_CURRENT_CONTEXT(ctx);

   const gl_vert_attrib genericIndex = VERT_ATTRIB_GENERIC(index);
   struct gl_vertex_array_object * const vao = ctx->Array.VAO;

   assert(genericIndex < ARRAY_SIZE(vao->VertexAttrib));

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "The command
    *
    *       void VertexAttribDivisor(uint index, uint divisor);
    *
    *     is equivalent to (assuming no errors are generated):
    *
    *       VertexAttribBinding(index, index);
    *       VertexBindingDivisor(index, divisor);"
    */
   _mesa_vertex_attrib_binding(ctx, vao, genericIndex, genericIndex);
   vertex_binding_divisor(ctx, vao, genericIndex, divisor);
}


/**
 * See GL_ARB_instanced_arrays.
 * Note that the instance divisor only applies to generic arrays, not
 * the legacy vertex arrays.
 */
void GLAPIENTRY
_mesa_VertexAttribDivisor(GLuint index, GLuint divisor)
{
   GET_CURRENT_CONTEXT(ctx);

   const gl_vert_attrib genericIndex = VERT_ATTRIB_GENERIC(index);
   struct gl_vertex_array_object * const vao = ctx->Array.VAO;

   if (!ctx->Extensions.ARB_instanced_arrays) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glVertexAttribDivisor()");
      return;
   }

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glVertexAttribDivisor(index = %u)", index);
      return;
   }

   assert(genericIndex < ARRAY_SIZE(vao->VertexAttrib));

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "The command
    *
    *       void VertexAttribDivisor(uint index, uint divisor);
    *
    *     is equivalent to (assuming no errors are generated):
    *
    *       VertexAttribBinding(index, index);
    *       VertexBindingDivisor(index, divisor);"
    */
   _mesa_vertex_attrib_binding(ctx, vao, genericIndex, genericIndex);
   vertex_binding_divisor(ctx, vao, genericIndex, divisor);
}


static ALWAYS_INLINE void
vertex_array_vertex_buffer(struct gl_context *ctx,
                           struct gl_vertex_array_object *vao,
                           GLuint bindingIndex, GLuint buffer, GLintptr offset,
                           GLsizei stride, bool no_error, const char *func)
{
   struct gl_buffer_object *vbo;
   if (buffer ==
       vao->BufferBinding[VERT_ATTRIB_GENERIC(bindingIndex)].BufferObj->Name) {
      vbo = vao->BufferBinding[VERT_ATTRIB_GENERIC(bindingIndex)].BufferObj;
   } else if (buffer != 0) {
      vbo = _mesa_lookup_bufferobj(ctx, buffer);

      if (!no_error && !vbo && _mesa_is_gles31(ctx)) {
         _mesa_error(ctx, GL_INVALID_OPERATION, "%s(non-gen name)", func);
         return;
      }
      /* From the GL_ARB_vertex_attrib_array spec:
       *
       *   "[Core profile only:]
       *    An INVALID_OPERATION error is generated if buffer is not zero or a
       *    name returned from a previous call to GenBuffers, or if such a name
       *    has since been deleted with DeleteBuffers.
       *
       * Otherwise, we fall back to the same compat profile behavior as other
       * object references (automatically gen it).
       */
      if (!_mesa_handle_bind_buffer_gen(ctx, buffer, &vbo, func))
         return;
   } else {
      /* The ARB_vertex_attrib_binding spec says:
       *
       *    "If <buffer> is zero, any buffer object attached to this
       *     bindpoint is detached."
       */
      vbo = ctx->Shared->NullBufferObj;
   }

   _mesa_bind_vertex_buffer(ctx, vao, VERT_ATTRIB_GENERIC(bindingIndex),
                            vbo, offset, stride);
}


/**
 * GL_ARB_vertex_attrib_binding
 */
static void
vertex_array_vertex_buffer_err(struct gl_context *ctx,
                               struct gl_vertex_array_object *vao,
                               GLuint bindingIndex, GLuint buffer,
                               GLintptr offset, GLsizei stride,
                               const char *func)
{
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "An INVALID_VALUE error is generated if <bindingindex> is greater than
    *     the value of MAX_VERTEX_ATTRIB_BINDINGS."
    */
   if (bindingIndex >= ctx->Const.MaxVertexAttribBindings) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(bindingindex=%u > "
                  "GL_MAX_VERTEX_ATTRIB_BINDINGS)",
                  func, bindingIndex);
      return;
   }

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "The error INVALID_VALUE is generated if <stride> or <offset>
    *     are negative."
    */
   if (offset < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(offset=%" PRId64 " < 0)",
                  func, (int64_t) offset);
      return;
   }

   if (stride < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(stride=%d < 0)", func, stride);
      return;
   }

   if (((_mesa_is_desktop_gl(ctx) && ctx->Version >= 44) || _mesa_is_gles31(ctx)) &&
       stride > ctx->Const.MaxVertexAttribStride) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(stride=%d > "
                  "GL_MAX_VERTEX_ATTRIB_STRIDE)", func, stride);
      return;
   }

   vertex_array_vertex_buffer(ctx, vao, bindingIndex, buffer, offset,
                              stride, false, func);
}


void GLAPIENTRY
_mesa_BindVertexBuffer_no_error(GLuint bindingIndex, GLuint buffer,
                                GLintptr offset, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);
   vertex_array_vertex_buffer(ctx, ctx->Array.VAO, bindingIndex,
                              buffer, offset, stride, true,
                              "glBindVertexBuffer");
}


void GLAPIENTRY
_mesa_BindVertexBuffer(GLuint bindingIndex, GLuint buffer, GLintptr offset,
                       GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "An INVALID_OPERATION error is generated if no vertex array object
    *     is bound."
    */
   if ((ctx->API == API_OPENGL_CORE || _mesa_is_gles31(ctx)) &&
       ctx->Array.VAO == ctx->Array.DefaultVAO) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glBindVertexBuffer(No array object bound)");
      return;
   }

   vertex_array_vertex_buffer_err(ctx, ctx->Array.VAO, bindingIndex,
                                  buffer, offset, stride,
                                  "glBindVertexBuffer");
}


void GLAPIENTRY
_mesa_VertexArrayVertexBuffer_no_error(GLuint vaobj, GLuint bindingIndex,
                                       GLuint buffer, GLintptr offset,
                                       GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);

   struct gl_vertex_array_object *vao = _mesa_lookup_vao(ctx, vaobj);
   vertex_array_vertex_buffer(ctx, vao, bindingIndex, buffer, offset,
                              stride, true, "glVertexArrayVertexBuffer");
}


void GLAPIENTRY
_mesa_VertexArrayVertexBuffer(GLuint vaobj, GLuint bindingIndex, GLuint buffer,
                              GLintptr offset, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao;

   /* The ARB_direct_state_access specification says:
    *
    *   "An INVALID_OPERATION error is generated by VertexArrayVertexBuffer
    *    if <vaobj> is not [compatibility profile: zero or] the name of an
    *    existing vertex array object."
    */
   vao = _mesa_lookup_vao_err(ctx, vaobj, false, "glVertexArrayVertexBuffer");
   if (!vao)
      return;

   vertex_array_vertex_buffer_err(ctx, vao, bindingIndex, buffer, offset,
                                  stride, "glVertexArrayVertexBuffer");
}


static ALWAYS_INLINE void
vertex_array_vertex_buffers(struct gl_context *ctx,
                            struct gl_vertex_array_object *vao,
                            GLuint first, GLsizei count, const GLuint *buffers,
                            const GLintptr *offsets, const GLsizei *strides,
                            bool no_error, const char *func)
{
   GLint i;

   if (!buffers) {
      /**
       * The ARB_multi_bind spec says:
       *
       *    "If <buffers> is NULL, each affected vertex buffer binding point
       *     from <first> through <first>+<count>-1 will be reset to have no
       *     bound buffer object.  In this case, the offsets and strides
       *     associated with the binding points are set to default values,
       *     ignoring <offsets> and <strides>."
       */
      struct gl_buffer_object *vbo = ctx->Shared->NullBufferObj;

      for (i = 0; i < count; i++)
         _mesa_bind_vertex_buffer(ctx, vao, VERT_ATTRIB_GENERIC(first + i),
                                  vbo, 0, 16);

      return;
   }

   /* Note that the error semantics for multi-bind commands differ from
    * those of other GL commands.
    *
    * The Issues section in the ARB_multi_bind spec says:
    *
    *    "(11) Typically, OpenGL specifies that if an error is generated by
    *          a command, that command has no effect.  This is somewhat
    *          unfortunate for multi-bind commands, because it would require
    *          a first pass to scan the entire list of bound objects for
    *          errors and then a second pass to actually perform the
    *          bindings.  Should we have different error semantics?
    *
    *       RESOLVED:  Yes.  In this specification, when the parameters for
    *       one of the <count> binding points are invalid, that binding
    *       point is not updated and an error will be generated.  However,
    *       other binding points in the same command will be updated if
    *       their parameters are valid and no other error occurs."
    */

   _mesa_HashLockMutex(ctx->Shared->BufferObjects);

   for (i = 0; i < count; i++) {
      struct gl_buffer_object *vbo;

      if (!no_error) {
         /* The ARB_multi_bind spec says:
          *
          *    "An INVALID_VALUE error is generated if any value in
          *     <offsets> or <strides> is negative (per binding)."
          */
         if (offsets[i] < 0) {
            _mesa_error(ctx, GL_INVALID_VALUE,
                        "%s(offsets[%u]=%" PRId64 " < 0)",
                        func, i, (int64_t) offsets[i]);
            continue;
         }

         if (strides[i] < 0) {
            _mesa_error(ctx, GL_INVALID_VALUE,
                        "%s(strides[%u]=%d < 0)",
                        func, i, strides[i]);
            continue;
         }

         if (_mesa_is_desktop_gl(ctx) && ctx->Version >= 44 &&
             strides[i] > ctx->Const.MaxVertexAttribStride) {
            _mesa_error(ctx, GL_INVALID_VALUE,
                        "%s(strides[%u]=%d > "
                        "GL_MAX_VERTEX_ATTRIB_STRIDE)", func, i, strides[i]);
            continue;
         }
      }

      if (buffers[i]) {
         struct gl_vertex_buffer_binding *binding =
            &vao->BufferBinding[VERT_ATTRIB_GENERIC(first + i)];

         if (buffers[i] == binding->BufferObj->Name)
            vbo = binding->BufferObj;
         else
            vbo = _mesa_multi_bind_lookup_bufferobj(ctx, buffers, i, func);

         if (!vbo)
            continue;
      } else {
         vbo = ctx->Shared->NullBufferObj;
      }

      _mesa_bind_vertex_buffer(ctx, vao, VERT_ATTRIB_GENERIC(first + i),
                               vbo, offsets[i], strides[i]);
   }

   _mesa_HashUnlockMutex(ctx->Shared->BufferObjects);
}


static void
vertex_array_vertex_buffers_err(struct gl_context *ctx,
                                struct gl_vertex_array_object *vao,
                                GLuint first, GLsizei count,
                                const GLuint *buffers, const GLintptr *offsets,
                                const GLsizei *strides, const char *func)
{
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   /* The ARB_multi_bind spec says:
    *
    *    "An INVALID_OPERATION error is generated if <first> + <count>
    *     is greater than the value of MAX_VERTEX_ATTRIB_BINDINGS."
    */
   if (first + count > ctx->Const.MaxVertexAttribBindings) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "%s(first=%u + count=%d > the value of "
                  "GL_MAX_VERTEX_ATTRIB_BINDINGS=%u)",
                  func, first, count, ctx->Const.MaxVertexAttribBindings);
      return;
   }

   vertex_array_vertex_buffers(ctx, vao, first, count, buffers, offsets,
                               strides, false, func);
}


void GLAPIENTRY
_mesa_BindVertexBuffers_no_error(GLuint first, GLsizei count,
                                 const GLuint *buffers, const GLintptr *offsets,
                                 const GLsizei *strides)
{
   GET_CURRENT_CONTEXT(ctx);

   vertex_array_vertex_buffers(ctx, ctx->Array.VAO, first, count,
                               buffers, offsets, strides, true,
                               "glBindVertexBuffers");
}


void GLAPIENTRY
_mesa_BindVertexBuffers(GLuint first, GLsizei count, const GLuint *buffers,
                        const GLintptr *offsets, const GLsizei *strides)
{
   GET_CURRENT_CONTEXT(ctx);

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "An INVALID_OPERATION error is generated if no
    *     vertex array object is bound."
    */
   if (ctx->API == API_OPENGL_CORE &&
       ctx->Array.VAO == ctx->Array.DefaultVAO) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glBindVertexBuffers(No array object bound)");
      return;
   }

   vertex_array_vertex_buffers_err(ctx, ctx->Array.VAO, first, count,
                                   buffers, offsets, strides,
                                   "glBindVertexBuffers");
}


void GLAPIENTRY
_mesa_VertexArrayVertexBuffers_no_error(GLuint vaobj, GLuint first,
                                        GLsizei count, const GLuint *buffers,
                                        const GLintptr *offsets,
                                        const GLsizei *strides)
{
   GET_CURRENT_CONTEXT(ctx);

   struct gl_vertex_array_object *vao = _mesa_lookup_vao(ctx, vaobj);
   vertex_array_vertex_buffers(ctx, vao, first, count,
                               buffers, offsets, strides, true,
                               "glVertexArrayVertexBuffers");
}


void GLAPIENTRY
_mesa_VertexArrayVertexBuffers(GLuint vaobj, GLuint first, GLsizei count,
                               const GLuint *buffers,
                               const GLintptr *offsets, const GLsizei *strides)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao;

   /* The ARB_direct_state_access specification says:
    *
    *   "An INVALID_OPERATION error is generated by VertexArrayVertexBuffer
    *    if <vaobj> is not [compatibility profile: zero or] the name of an
    *    existing vertex array object."
    */
   vao = _mesa_lookup_vao_err(ctx, vaobj, false, "glVertexArrayVertexBuffers");
   if (!vao)
      return;

   vertex_array_vertex_buffers_err(ctx, vao, first, count,
                                   buffers, offsets, strides,
                                   "glVertexArrayVertexBuffers");
}


static void
vertex_attrib_format(GLuint attribIndex, GLint size, GLenum type,
                     GLboolean normalized, GLboolean integer,
                     GLboolean doubles, GLbitfield legalTypes,
                     GLsizei sizeMax, GLuint relativeOffset,
                     const char *func)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   GLenum format = get_array_format(ctx, sizeMax, &size);

   if (!_mesa_is_no_error_enabled(ctx)) {
      /* The ARB_vertex_attrib_binding spec says:
       *
       *    "An INVALID_OPERATION error is generated under any of the
       *    following conditions:
       *     - if no vertex array object is currently bound (see section
       *       2.10);
       *     - ..."
       *
       * This error condition only applies to VertexAttribFormat and
       * VertexAttribIFormat in the extension spec, but we assume that this
       * is an oversight.  In the OpenGL 4.3 (Core Profile) spec, it applies
       * to all three functions.
       */
      if ((ctx->API == API_OPENGL_CORE || _mesa_is_gles31(ctx)) &&
          ctx->Array.VAO == ctx->Array.DefaultVAO) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "%s(No array object bound)", func);
         return;
      }

      /* The ARB_vertex_attrib_binding spec says:
       *
       *   "The error INVALID_VALUE is generated if index is greater than or
       *   equal to the value of MAX_VERTEX_ATTRIBS."
       */
      if (attribIndex >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
         _mesa_error(ctx, GL_INVALID_VALUE,
                     "%s(attribindex=%u > "
                     "GL_MAX_VERTEX_ATTRIBS)",
                     func, attribIndex);
         return;
      }

      if (!validate_array_format(ctx, func, ctx->Array.VAO,
                                 VERT_ATTRIB_GENERIC(attribIndex),
                                 legalTypes, 1, sizeMax, size, type,
                                 normalized, integer, doubles, relativeOffset,
                                 format)) {
         return;
      }
   }

   _mesa_update_array_format(ctx, ctx->Array.VAO,
                             VERT_ATTRIB_GENERIC(attribIndex), size, type,
                             format, normalized, integer, doubles,
                             relativeOffset);
}


void GLAPIENTRY
_mesa_VertexAttribFormat(GLuint attribIndex, GLint size, GLenum type,
                         GLboolean normalized, GLuint relativeOffset)
{
   vertex_attrib_format(attribIndex, size, type, normalized,
                        GL_FALSE, GL_FALSE, ATTRIB_FORMAT_TYPES_MASK,
                        BGRA_OR_4, relativeOffset,
                        "glVertexAttribFormat");
}


void GLAPIENTRY
_mesa_VertexAttribIFormat(GLuint attribIndex, GLint size, GLenum type,
                          GLuint relativeOffset)
{
   vertex_attrib_format(attribIndex, size, type, GL_FALSE,
                        GL_TRUE, GL_FALSE, ATTRIB_IFORMAT_TYPES_MASK, 4,
                        relativeOffset, "glVertexAttribIFormat");
}


void GLAPIENTRY
_mesa_VertexAttribLFormat(GLuint attribIndex, GLint size, GLenum type,
                          GLuint relativeOffset)
{
   vertex_attrib_format(attribIndex, size, type, GL_FALSE, GL_FALSE,
                        GL_TRUE, ATTRIB_LFORMAT_TYPES_MASK, 4,
                        relativeOffset, "glVertexAttribLFormat");
}


static void
vertex_array_attrib_format(GLuint vaobj, GLuint attribIndex, GLint size,
                           GLenum type, GLboolean normalized,
                           GLboolean integer, GLboolean doubles,
                           GLbitfield legalTypes, GLsizei sizeMax,
                           GLuint relativeOffset, const char *func)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao;

   ASSERT_OUTSIDE_BEGIN_END(ctx);

   GLenum format = get_array_format(ctx, sizeMax, &size);

   if (_mesa_is_no_error_enabled(ctx)) {
      vao = _mesa_lookup_vao(ctx, vaobj);
      if (!vao)
         return;
   } else {
      /* The ARB_direct_state_access spec says:
       *
       *   "An INVALID_OPERATION error is generated by
       *   VertexArrayAttrib*Format if <vaobj> is not [compatibility profile:
       *   zero or] the name of an existing vertex array object."
       */
      vao = _mesa_lookup_vao_err(ctx, vaobj, false, func);
      if (!vao)
         return;

      /* The ARB_vertex_attrib_binding spec says:
       *
       *   "The error INVALID_VALUE is generated if index is greater than or
       *   equal to the value of MAX_VERTEX_ATTRIBS."
       */
      if (attribIndex >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
         _mesa_error(ctx, GL_INVALID_VALUE,
                     "%s(attribindex=%u > GL_MAX_VERTEX_ATTRIBS)",
                     func, attribIndex);
         return;
      }

      if (!validate_array_format(ctx, func, vao,
                                 VERT_ATTRIB_GENERIC(attribIndex),
                                 legalTypes, 1, sizeMax, size, type,
                                 normalized, integer, doubles, relativeOffset,
                                 format)) {
         return;
      }
   }

   _mesa_update_array_format(ctx, vao, VERT_ATTRIB_GENERIC(attribIndex), size,
                             type, format, normalized, integer, doubles,
                             relativeOffset);
}


void GLAPIENTRY
_mesa_VertexArrayAttribFormat(GLuint vaobj, GLuint attribIndex, GLint size,
                              GLenum type, GLboolean normalized,
                              GLuint relativeOffset)
{
   vertex_array_attrib_format(vaobj, attribIndex, size, type, normalized,
                              GL_FALSE, GL_FALSE, ATTRIB_FORMAT_TYPES_MASK,
                              BGRA_OR_4, relativeOffset,
                              "glVertexArrayAttribFormat");
}


void GLAPIENTRY
_mesa_VertexArrayAttribIFormat(GLuint vaobj, GLuint attribIndex,
                               GLint size, GLenum type,
                               GLuint relativeOffset)
{
   vertex_array_attrib_format(vaobj, attribIndex, size, type, GL_FALSE,
                              GL_TRUE, GL_FALSE, ATTRIB_IFORMAT_TYPES_MASK,
                              4, relativeOffset,
                              "glVertexArrayAttribIFormat");
}


void GLAPIENTRY
_mesa_VertexArrayAttribLFormat(GLuint vaobj, GLuint attribIndex,
                               GLint size, GLenum type,
                               GLuint relativeOffset)
{
   vertex_array_attrib_format(vaobj, attribIndex, size, type, GL_FALSE,
                              GL_FALSE, GL_TRUE, ATTRIB_LFORMAT_TYPES_MASK,
                              4, relativeOffset,
                              "glVertexArrayAttribLFormat");
}


static void
vertex_array_attrib_binding(struct gl_context *ctx,
                            struct gl_vertex_array_object *vao,
                            GLuint attribIndex, GLuint bindingIndex,
                            const char *func)
{
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "<attribindex> must be less than the value of MAX_VERTEX_ATTRIBS and
    *     <bindingindex> must be less than the value of
    *     MAX_VERTEX_ATTRIB_BINDINGS, otherwise the error INVALID_VALUE
    *     is generated."
    */
   if (attribIndex >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(attribindex=%u >= "
                  "GL_MAX_VERTEX_ATTRIBS)",
                  func, attribIndex);
      return;
   }

   if (bindingIndex >= ctx->Const.MaxVertexAttribBindings) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(bindingindex=%u >= "
                  "GL_MAX_VERTEX_ATTRIB_BINDINGS)",
                  func, bindingIndex);
      return;
   }

   assert(VERT_ATTRIB_GENERIC(attribIndex) < ARRAY_SIZE(vao->VertexAttrib));

   _mesa_vertex_attrib_binding(ctx, vao,
                               VERT_ATTRIB_GENERIC(attribIndex),
                               VERT_ATTRIB_GENERIC(bindingIndex));
}


void GLAPIENTRY
_mesa_VertexAttribBinding_no_error(GLuint attribIndex, GLuint bindingIndex)
{
   GET_CURRENT_CONTEXT(ctx);
   _mesa_vertex_attrib_binding(ctx, ctx->Array.VAO,
                               VERT_ATTRIB_GENERIC(attribIndex),
                               VERT_ATTRIB_GENERIC(bindingIndex));
}


void GLAPIENTRY
_mesa_VertexAttribBinding(GLuint attribIndex, GLuint bindingIndex)
{
   GET_CURRENT_CONTEXT(ctx);

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "An INVALID_OPERATION error is generated if no vertex array object
    *     is bound."
    */
   if ((ctx->API == API_OPENGL_CORE || _mesa_is_gles31(ctx)) &&
       ctx->Array.VAO == ctx->Array.DefaultVAO) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glVertexAttribBinding(No array object bound)");
      return;
   }

   vertex_array_attrib_binding(ctx, ctx->Array.VAO,
                               attribIndex, bindingIndex,
                               "glVertexAttribBinding");
}


void GLAPIENTRY
_mesa_VertexArrayAttribBinding_no_error(GLuint vaobj, GLuint attribIndex,
                                        GLuint bindingIndex)
{
   GET_CURRENT_CONTEXT(ctx);

   struct gl_vertex_array_object *vao = _mesa_lookup_vao(ctx, vaobj);
   _mesa_vertex_attrib_binding(ctx, vao,
                               VERT_ATTRIB_GENERIC(attribIndex),
                               VERT_ATTRIB_GENERIC(bindingIndex));
}


void GLAPIENTRY
_mesa_VertexArrayAttribBinding(GLuint vaobj, GLuint attribIndex, GLuint bindingIndex)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object *vao;

   /* The ARB_direct_state_access specification says:
    *
    *   "An INVALID_OPERATION error is generated by VertexArrayAttribBinding
    *    if <vaobj> is not [compatibility profile: zero or] the name of an
    *    existing vertex array object."
    */
   vao = _mesa_lookup_vao_err(ctx, vaobj, false, "glVertexArrayAttribBinding");
   if (!vao)
      return;

   vertex_array_attrib_binding(ctx, vao, attribIndex, bindingIndex,
                               "glVertexArrayAttribBinding");
}


static void
vertex_array_binding_divisor(struct gl_context *ctx,
                             struct gl_vertex_array_object *vao,
                             GLuint bindingIndex, GLuint divisor,
                             const char *func)
{
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   if (!ctx->Extensions.ARB_instanced_arrays) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s()", func);
      return;
   }

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "An INVALID_VALUE error is generated if <bindingindex> is greater
    *     than or equal to the value of MAX_VERTEX_ATTRIB_BINDINGS."
    */
   if (bindingIndex >= ctx->Const.MaxVertexAttribBindings) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(bindingindex=%u > "
                  "GL_MAX_VERTEX_ATTRIB_BINDINGS)",
                  func, bindingIndex);
      return;
   }

   vertex_binding_divisor(ctx, vao, VERT_ATTRIB_GENERIC(bindingIndex), divisor);
}


void GLAPIENTRY
_mesa_VertexBindingDivisor_no_error(GLuint bindingIndex, GLuint divisor)
{
   GET_CURRENT_CONTEXT(ctx);
   vertex_binding_divisor(ctx, ctx->Array.VAO,
                          VERT_ATTRIB_GENERIC(bindingIndex), divisor);
}


void GLAPIENTRY
_mesa_VertexBindingDivisor(GLuint bindingIndex, GLuint divisor)
{
   GET_CURRENT_CONTEXT(ctx);

   /* The ARB_vertex_attrib_binding spec says:
    *
    *    "An INVALID_OPERATION error is generated if no vertex array object
    *     is bound."
    */
   if ((ctx->API == API_OPENGL_CORE || _mesa_is_gles31(ctx)) &&
       ctx->Array.VAO == ctx->Array.DefaultVAO) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glVertexBindingDivisor(No array object bound)");
      return;
   }

   vertex_array_binding_divisor(ctx, ctx->Array.VAO,
                                bindingIndex, divisor,
                                "glVertexBindingDivisor");
}


void GLAPIENTRY
_mesa_VertexArrayBindingDivisor_no_error(GLuint vaobj, GLuint bindingIndex,
                                         GLuint divisor)
{
   GET_CURRENT_CONTEXT(ctx);

   struct gl_vertex_array_object *vao = _mesa_lookup_vao(ctx, vaobj);
   vertex_binding_divisor(ctx, vao, VERT_ATTRIB_GENERIC(bindingIndex), divisor);
}


void GLAPIENTRY
_mesa_VertexArrayBindingDivisor(GLuint vaobj, GLuint bindingIndex,
                                GLuint divisor)
{
   struct gl_vertex_array_object *vao;
   GET_CURRENT_CONTEXT(ctx);

   /* The ARB_direct_state_access specification says:
    *
    *   "An INVALID_OPERATION error is generated by VertexArrayBindingDivisor
    *    if <vaobj> is not [compatibility profile: zero or] the name of an
    *    existing vertex array object."
    */
   vao = _mesa_lookup_vao_err(ctx, vaobj, false, "glVertexArrayBindingDivisor");
   if (!vao)
       return;

   vertex_array_binding_divisor(ctx, vao, bindingIndex, divisor,
                                "glVertexArrayBindingDivisor");
}


void
_mesa_copy_vertex_attrib_array(struct gl_context *ctx,
                               struct gl_array_attributes *dst,
                               const struct gl_array_attributes *src)
{
   dst->Ptr            = src->Ptr;
   dst->RelativeOffset = src->RelativeOffset;
   dst->Format         = src->Format;
   dst->Stride         = src->Stride;
   dst->BufferBindingIndex = src->BufferBindingIndex;
   dst->_EffBufferBindingIndex = src->_EffBufferBindingIndex;
   dst->_EffRelativeOffset = src->_EffRelativeOffset;
}

void
_mesa_copy_vertex_buffer_binding(struct gl_context *ctx,
                                 struct gl_vertex_buffer_binding *dst,
                                 const struct gl_vertex_buffer_binding *src)
{
   dst->Offset          = src->Offset;
   dst->Stride          = src->Stride;
   dst->InstanceDivisor = src->InstanceDivisor;
   dst->_BoundArrays    = src->_BoundArrays;
   dst->_EffBoundArrays = src->_EffBoundArrays;
   dst->_EffOffset      = src->_EffOffset;

   _mesa_reference_buffer_object(ctx, &dst->BufferObj, src->BufferObj);
}

/**
 * Print current vertex object/array info.  For debug.
 */
void
_mesa_print_arrays(struct gl_context *ctx)
{
   const struct gl_vertex_array_object *vao = ctx->Array.VAO;

   fprintf(stderr, "Array Object %u\n", vao->Name);

   GLbitfield mask = vao->Enabled;
   while (mask) {
      const gl_vert_attrib i = u_bit_scan(&mask);
      const struct gl_array_attributes *array = &vao->VertexAttrib[i];

      const struct gl_vertex_buffer_binding *binding =
         &vao->BufferBinding[array->BufferBindingIndex];
      const struct gl_buffer_object *bo = binding->BufferObj;

      fprintf(stderr, "  %s: Ptr=%p, Type=%s, Size=%d, ElemSize=%u, "
              "Stride=%d, Buffer=%u(Size %lu)\n",
              gl_vert_attrib_name((gl_vert_attrib)i),
              array->Ptr, _mesa_enum_to_string(array->Format.Type),
              array->Format.Size,
              array->Format._ElementSize, binding->Stride, bo->Name,
              (unsigned long) bo->Size);
   }
}


/**
 * Initialize vertex array state for given context.
 */
void
_mesa_init_varray(struct gl_context *ctx)
{
   ctx->Array.DefaultVAO = _mesa_new_vao(ctx, 0);
   _mesa_reference_vao(ctx, &ctx->Array.VAO, ctx->Array.DefaultVAO);
   ctx->Array._EmptyVAO = _mesa_new_vao(ctx, ~0u);
   _mesa_reference_vao(ctx, &ctx->Array._DrawVAO, ctx->Array._EmptyVAO);
   ctx->Array.ActiveTexture = 0;   /* GL_ARB_multitexture */

   ctx->Array.Objects = _mesa_NewHashTable();
}


/**
 * Callback for deleting an array object.  Called by _mesa_HashDeleteAll().
 */
static void
delete_arrayobj_cb(GLuint id, void *data, void *userData)
{
   struct gl_vertex_array_object *vao = (struct gl_vertex_array_object *) data;
   struct gl_context *ctx = (struct gl_context *) userData;
   _mesa_delete_vao(ctx, vao);
}


/**
 * Free vertex array state for given context.
 */
void
_mesa_free_varray_data(struct gl_context *ctx)
{
   _mesa_HashDeleteAll(ctx->Array.Objects, delete_arrayobj_cb, ctx);
   _mesa_DeleteHashTable(ctx->Array.Objects);
}

void GLAPIENTRY
_mesa_GetVertexArrayIntegervEXT(GLuint vaobj, GLenum pname, GLint *param)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object* vao;
   void* ptr;

   vao = _mesa_lookup_vao_err(ctx, vaobj, true,
                              "glGetVertexArrayIntegervEXT");
   if (!vao)
      return;

   /* The EXT_direct_state_access spec says:
    *
    *    "For GetVertexArrayIntegervEXT, pname must be one of the "Get value" tokens
    *    in tables 6.6, 6.7, 6.8, and 6.9 that use GetIntegerv, IsEnabled, or
    *    GetPointerv for their "Get command" (so excluding the VERTEX_ATTRIB_*
    *    tokens)."
    */
   switch (pname) {
      /* Tokens using GetIntegerv */
      case GL_CLIENT_ACTIVE_TEXTURE:
         *param = GL_TEXTURE0_ARB + ctx->Array.ActiveTexture;
         break;
      case GL_VERTEX_ARRAY_SIZE:
         *param = vao->VertexAttrib[VERT_ATTRIB_POS].Format.Size;
         break;
      case GL_VERTEX_ARRAY_TYPE:
         *param = vao->VertexAttrib[VERT_ATTRIB_POS].Format.Type;
         break;
      case GL_VERTEX_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_POS].Stride;
         break;
      case GL_VERTEX_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_POS].BufferObj->Name;
         break;
      case GL_COLOR_ARRAY_SIZE:
         *param = vao->VertexAttrib[VERT_ATTRIB_COLOR0].Format.Size;
         break;
      case GL_COLOR_ARRAY_TYPE:
         *param = vao->VertexAttrib[VERT_ATTRIB_COLOR0].Format.Type;
         break;
      case GL_COLOR_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_COLOR0].Stride;
         break;
      case GL_COLOR_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_COLOR0].BufferObj->Name;
         break;
      case GL_EDGE_FLAG_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_EDGEFLAG].Stride;
         break;
      case GL_EDGE_FLAG_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_EDGEFLAG].BufferObj->Name;
         break;
      case GL_INDEX_ARRAY_TYPE:
         *param = vao->VertexAttrib[VERT_ATTRIB_COLOR_INDEX].Format.Type;
         break;
      case GL_INDEX_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_COLOR_INDEX].Stride;
         break;
      case GL_INDEX_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_COLOR_INDEX].BufferObj->Name;
         break;
      case GL_NORMAL_ARRAY_TYPE:
         *param = vao->VertexAttrib[VERT_ATTRIB_NORMAL].Format.Type;
         break;
      case GL_NORMAL_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_NORMAL].Stride;
         break;
      case GL_NORMAL_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_NORMAL].BufferObj->Name;
         break;
      case GL_TEXTURE_COORD_ARRAY_SIZE:
         *param = vao->VertexAttrib[VERT_ATTRIB_TEX(ctx->Array.ActiveTexture)].Format.Size;
         break;
      case GL_TEXTURE_COORD_ARRAY_TYPE:
         *param = vao->VertexAttrib[VERT_ATTRIB_TEX(ctx->Array.ActiveTexture)].Format.Type;
         break;
      case GL_TEXTURE_COORD_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_TEX(ctx->Array.ActiveTexture)].Stride;
         break;
      case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_TEX(ctx->Array.ActiveTexture)].BufferObj->Name;
         break;
      case GL_FOG_COORD_ARRAY_TYPE:
         *param = vao->VertexAttrib[VERT_ATTRIB_FOG].Format.Type;
         break;
      case GL_FOG_COORD_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_FOG].Stride;
         break;
      case GL_FOG_COORD_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_FOG].BufferObj->Name;
         break;
      case GL_SECONDARY_COLOR_ARRAY_SIZE:
         *param = vao->VertexAttrib[VERT_ATTRIB_COLOR1].Format.Size;
         break;
      case GL_SECONDARY_COLOR_ARRAY_TYPE:
         *param = vao->VertexAttrib[VERT_ATTRIB_COLOR1].Format.Type;
         break;
      case GL_SECONDARY_COLOR_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_COLOR1].Stride;
         break;
      case GL_SECONDARY_COLOR_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_COLOR1].BufferObj->Name;
         break;

      /* Tokens using IsEnabled */
      case GL_VERTEX_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_POS);
         break;
      case GL_COLOR_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_COLOR0);
         break;
      case GL_EDGE_FLAG_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_EDGEFLAG);
         break;
      case GL_INDEX_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_COLOR_INDEX);
         break;
      case GL_NORMAL_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_NORMAL);
         break;
      case GL_TEXTURE_COORD_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_TEX(ctx->Array.ActiveTexture));
         break;
      case GL_FOG_COORD_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_FOG);
         break;
      case GL_SECONDARY_COLOR_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_COLOR1);
         break;

      /* Tokens using GetPointerv */
      case GL_VERTEX_ARRAY_POINTER:
      case GL_COLOR_ARRAY_POINTER:
      case GL_EDGE_FLAG_ARRAY_POINTER:
      case GL_INDEX_ARRAY_POINTER:
      case GL_NORMAL_ARRAY_POINTER:
      case GL_TEXTURE_COORD_ARRAY_POINTER:
      case GL_FOG_COORD_ARRAY_POINTER:
      case GL_SECONDARY_COLOR_ARRAY_POINTER:
         _get_vao_pointerv(pname, vao, &ptr, "glGetVertexArrayIntegervEXT");
         *param = (int) ((uint64_t) ptr & 0xFFFFFFFF);
         break;

      default:
         _mesa_error(ctx, GL_INVALID_ENUM, "glGetVertexArrayIntegervEXT(pname)");
   }
}

void GLAPIENTRY
_mesa_GetVertexArrayPointervEXT(GLuint vaobj, GLenum pname, GLvoid** param)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object* vao;

   vao = _mesa_lookup_vao_err(ctx, vaobj, true,
                              "glGetVertexArrayPointervEXT");
   if (!vao)
      return;

   /* The EXT_direct_state_access spec says:
    *
    *     "For GetVertexArrayPointervEXT, pname must be a *_ARRAY_POINTER token from
    *     tables 6.6, 6.7, and 6.8 excluding VERTEX_ATTRIB_ARRAY_POINT."
    */
   switch (pname) {
      case GL_VERTEX_ARRAY_POINTER:
      case GL_COLOR_ARRAY_POINTER:
      case GL_EDGE_FLAG_ARRAY_POINTER:
      case GL_INDEX_ARRAY_POINTER:
      case GL_NORMAL_ARRAY_POINTER:
      case GL_TEXTURE_COORD_ARRAY_POINTER:
      case GL_FOG_COORD_ARRAY_POINTER:
      case GL_SECONDARY_COLOR_ARRAY_POINTER:
         break;

      default:
         _mesa_error(ctx, GL_INVALID_ENUM, "glGetVertexArrayPointervEXT(pname)");
         return;
   }

   /* pname has been validated, we can now use the helper function */
   _get_vao_pointerv(pname, vao, param, "glGetVertexArrayPointervEXT");
}

void GLAPIENTRY
_mesa_GetVertexArrayIntegeri_vEXT(GLuint vaobj, GLuint index, GLenum pname, GLint *param)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object* vao;

   vao = _mesa_lookup_vao_err(ctx, vaobj, true,
                              "glGetVertexArrayIntegeri_vEXT");
   if (!vao)
      return;


   /* The EXT_direct_state_access spec says:
    *
    *    "For GetVertexArrayIntegeri_vEXT, pname must be one of the
    *    "Get value" tokens in tables 6.8 and 6.9 that use GetVertexAttribiv
    *    or GetVertexAttribPointerv (so allowing only the VERTEX_ATTRIB_*
    *    tokens) or a token of the form TEXTURE_COORD_ARRAY (the enable) or
    *    TEXTURE_COORD_ARRAY_*; index identifies the vertex attribute
    *    array to query or texture coordinate set index respectively."
    */

   switch (pname) {
      case GL_TEXTURE_COORD_ARRAY:
         *param = !!(vao->Enabled & VERT_BIT_TEX(index));
         break;
      case GL_TEXTURE_COORD_ARRAY_SIZE:
         *param = vao->VertexAttrib[VERT_ATTRIB_TEX(index)].Format.Size;
         break;
      case GL_TEXTURE_COORD_ARRAY_TYPE:
         *param = vao->VertexAttrib[VERT_ATTRIB_TEX(index)].Format.Type;
         break;
      case GL_TEXTURE_COORD_ARRAY_STRIDE:
         *param = vao->VertexAttrib[VERT_ATTRIB_TEX(index)].Stride;
         break;
      case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING:
         *param = vao->BufferBinding[VERT_ATTRIB_TEX(index)].BufferObj->Name;
         break;
      default:
         *param = get_vertex_array_attrib(ctx, vao, index, pname, "glGetVertexArrayIntegeri_vEXT");
   }
}

void GLAPIENTRY
_mesa_GetVertexArrayPointeri_vEXT(GLuint vaobj, GLuint index, GLenum pname, GLvoid** param)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_vertex_array_object* vao;

   vao = _mesa_lookup_vao_err(ctx, vaobj, true,
                              "glGetVertexArrayPointeri_vEXT");
   if (!vao)
      return;

   if (index >= ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs) {
      _mesa_error(ctx, GL_INVALID_VALUE, "glGetVertexArrayPointeri_vEXT(index)");
      return;
   }

   /* The EXT_direct_state_access spec says:
    *
    *     "For GetVertexArrayPointeri_vEXT, pname must be VERTEX_ATTRIB_ARRAY_POINTER
    *     or TEXTURE_COORD_ARRAY_POINTER with the index parameter indicating the vertex
    *     attribute or texture coordindate set index."
    */
   switch(pname) {
      case GL_VERTEX_ATTRIB_ARRAY_POINTER:
         *param = (GLvoid *) vao->VertexAttrib[VERT_ATTRIB_GENERIC(index)].Ptr;
         break;
      case GL_TEXTURE_COORD_ARRAY_POINTER:
         *param = (GLvoid *) vao->VertexAttrib[VERT_ATTRIB_TEX(index)].Ptr;
         break;
      default:
         _mesa_error(ctx, GL_INVALID_ENUM, "glGetVertexArrayPointeri_vEXT(pname)");
   }
}
