/*
    This file is part of darktable,
    Copyright (C) 2009-2025 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/** this is the view for the darkroom module.  */

#include "common/extra_optimizations.h"

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/focus_peaking.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/iop_order.h"
#include "common/overlay.h"
#include "common/selection.h"
#include "common/styles.h"
#include "common/tags.h"
#include "common/undo.h"
#include "common/utility.h"
#include "common/color_picker.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "dtgtk/button.h"
#include "dtgtk/stylemenu.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "gui/styles.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "libs/colorpicker.h"
#include "views/view.h"
#include "views/view_api.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#ifdef USE_LUA
#include "lua/image.h"
#endif

#include <gdk/gdkkeysyms.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

DT_MODULE(1)

static void _update_softproof_gamut_checking(dt_develop_t *d);

/* signal handler for filmstrip image switching */

static void _dev_change_image(dt_develop_t *dev, const dt_imgid_t imgid);

static void _darkroom_display_second_window(dt_develop_t *dev);
static void _darkroom_ui_second_window_write_config(GtkWidget *widget);
static void _darkroom_ui_second_window_cleanup(dt_develop_t *dev);

const char *name(const dt_view_t *self)
{
  return _("darkroom");
}

#ifdef USE_LUA

static const char *_live_snapshot_field_name(dt_introspection_field_t *field)
{
  if(field == NULL) return "value";
  if(field->header.field_name && field->header.field_name[0] != '\0') return field->header.field_name;
  if(field->header.name && field->header.name[0] != '\0') return field->header.name;
  return "value";
}

static gchar *_live_snapshot_join_path(const gchar *prefix, const gchar *suffix)
{
  if(suffix == NULL || suffix[0] == '\0') return g_strdup(prefix ? prefix : "");
  if(prefix == NULL || prefix[0] == '\0') return g_strdup(suffix);
  return g_strdup_printf("%s.%s", prefix, suffix);
}

static gchar *_live_snapshot_index_path(const gchar *prefix, const size_t index)
{
  if(prefix == NULL || prefix[0] == '\0') return g_strdup_printf("[%zu]", index);
  return g_strdup_printf("%s[%zu]", prefix, index);
}

static void _live_snapshot_add_field_header(JsonBuilder *builder, const gchar *path, const gchar *kind)
{
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "path");
  json_builder_add_string_value(builder, path);
  json_builder_set_member_name(builder, "kind");
  json_builder_add_string_value(builder, kind);
}

static void _live_snapshot_add_integer_value(JsonBuilder *builder, const guint64 value)
{
  if(value <= G_MAXINT64)
    json_builder_add_int_value(builder, (gint64)value);
  else
    json_builder_add_double_value(builder, (gdouble)value);
}

static void _live_snapshot_append_param_fields(JsonBuilder *builder,
                                               dt_introspection_field_t *field,
                                               const void *base,
                                               const gchar *path)
{
  if(field == NULL || base == NULL) return;

  const guint8 *value = ((const guint8 *)base) + field->header.offset;

  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_STRUCT:
    case DT_INTROSPECTION_TYPE_UNION:
      for(size_t index = 0; index < field->Struct.entries; index++)
      {
        dt_introspection_field_t *child = field->Struct.fields[index];
        g_autofree gchar *child_path = _live_snapshot_join_path(path, _live_snapshot_field_name(child));
        _live_snapshot_append_param_fields(builder, child, base, child_path);
      }
      break;
    case DT_INTROSPECTION_TYPE_ARRAY:
      if(field->Array.type == DT_INTROSPECTION_TYPE_CHAR)
      {
        const char *text = (const char *)value;
        const size_t text_len = strnlen(text, field->Array.count);
        if(g_utf8_validate(text, text_len, NULL))
        {
          g_autofree gchar *string_value = g_strndup(text, text_len);
          _live_snapshot_add_field_header(builder, path, "string");
          json_builder_set_member_name(builder, "value");
          json_builder_add_string_value(builder, string_value);
          json_builder_end_object(builder);
          break;
        }
      }

      for(size_t index = 0; index < field->Array.count; index++)
      {
        dt_introspection_field_t *child = NULL;
        const void *child_value = dt_introspection_access_array(field, (void *)value, index, &child);
        if(child_value == NULL || child == NULL) continue;

        g_autofree gchar *child_path = _live_snapshot_index_path(path, index);
        _live_snapshot_append_param_fields(builder, child,
                                           (const guint8 *)child_value - child->header.offset,
                                           child_path);
      }
      break;
    case DT_INTROSPECTION_TYPE_FLOAT:
      if(isfinite(*(const float *)value))
      {
        _live_snapshot_add_field_header(builder, path, "float");
        json_builder_set_member_name(builder, "value");
        json_builder_add_double_value(builder, *(const float *)value);
        json_builder_end_object(builder);
      }
      break;
    case DT_INTROSPECTION_TYPE_DOUBLE:
      if(isfinite(*(const double *)value))
      {
        _live_snapshot_add_field_header(builder, path, "double");
        json_builder_set_member_name(builder, "value");
        json_builder_add_double_value(builder, *(const double *)value);
        json_builder_end_object(builder);
      }
      break;
    case DT_INTROSPECTION_TYPE_CHAR:
      _live_snapshot_add_field_header(builder, path, "char");
      json_builder_set_member_name(builder, "value");
      json_builder_add_int_value(builder, (gint64)*(const char *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_INT8:
      _live_snapshot_add_field_header(builder, path, "int8");
      json_builder_set_member_name(builder, "value");
      json_builder_add_int_value(builder, (gint64)*(const int8_t *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_UINT8:
      _live_snapshot_add_field_header(builder, path, "uint8");
      json_builder_set_member_name(builder, "value");
      json_builder_add_int_value(builder, (gint64)*(const uint8_t *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_SHORT:
      _live_snapshot_add_field_header(builder, path, "short");
      json_builder_set_member_name(builder, "value");
      json_builder_add_int_value(builder, (gint64)*(const short *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_USHORT:
      _live_snapshot_add_field_header(builder, path, "ushort");
      json_builder_set_member_name(builder, "value");
      json_builder_add_int_value(builder, (gint64)*(const unsigned short *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_INT:
      _live_snapshot_add_field_header(builder, path, "int");
      json_builder_set_member_name(builder, "value");
      json_builder_add_int_value(builder, (gint64)*(const int *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_UINT:
      _live_snapshot_add_field_header(builder, path, "uint");
      json_builder_set_member_name(builder, "value");
      json_builder_add_int_value(builder, (gint64)*(const unsigned int *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_LONG:
      _live_snapshot_add_field_header(builder, path, "long");
      json_builder_set_member_name(builder, "value");
      json_builder_add_int_value(builder, (gint64)*(const long *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_ULONG:
      _live_snapshot_add_field_header(builder, path, "ulong");
      json_builder_set_member_name(builder, "value");
      _live_snapshot_add_integer_value(builder, *(const unsigned long *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_BOOL:
      _live_snapshot_add_field_header(builder, path, "bool");
      json_builder_set_member_name(builder, "value");
      json_builder_add_boolean_value(builder, *(const gboolean *)value);
      json_builder_end_object(builder);
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
      {
        _live_snapshot_add_field_header(builder, path, "enum");
        json_builder_set_member_name(builder, "value");
        const int enum_value = *(const int *)value;
        const char *enum_name = dt_introspection_get_enum_name(field, enum_value);
        if(enum_name != NULL)
          json_builder_add_string_value(builder, enum_name);
        else
          json_builder_add_int_value(builder, (gint64)enum_value);
        json_builder_end_object(builder);
      }
      break;
    case DT_INTROSPECTION_TYPE_NONE:
    case DT_INTROSPECTION_TYPE_OPAQUE:
    case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
    default:
      break;
  }
}

static void _live_snapshot_add_params(JsonBuilder *builder,
                                      dt_iop_module_t *module,
                                      const void *params)
{
  json_builder_set_member_name(builder, "params");
  json_builder_begin_object(builder);

  if(module != NULL && module->have_introspection && params != NULL)
  {
    dt_introspection_t *introspection = module->get_introspection();
    if(introspection != NULL && introspection->field != NULL)
    {
      json_builder_set_member_name(builder, "encoding");
      json_builder_add_string_value(builder, "introspection-v1");
      json_builder_set_member_name(builder, "fields");
      json_builder_begin_array(builder);
      _live_snapshot_append_param_fields(builder, introspection->field, params, "");
      json_builder_end_array(builder);
      json_builder_end_object(builder);
      return;
    }
  }

  json_builder_set_member_name(builder, "encoding");
  json_builder_add_string_value(builder, "unsupported");
  json_builder_end_object(builder);
}

static gchar *_live_snapshot_instance_key(const char *module_op,
                                          const gint instance,
                                          const gint multi_priority,
                                          const char *multi_name)
{
  return g_strdup_printf("%s#%d#%d#%s",
                         module_op ? module_op : "unknown",
                         instance,
                         multi_priority,
                         multi_name ? multi_name : "");
}

static const gchar *_live_snapshot_blend_mode_name(const uint32_t blend_mode)
{
  switch(blend_mode & DEVELOP_BLEND_MODE_MASK)
  {
    case DEVELOP_BLEND_NORMAL2:
      return "normal";
    case DEVELOP_BLEND_AVERAGE:
      return "average";
    case DEVELOP_BLEND_DIFFERENCE2:
      return "difference";
    case DEVELOP_BLEND_BOUNDED:
      return "bounded";
    case DEVELOP_BLEND_LIGHTEN:
      return "lighten";
    case DEVELOP_BLEND_DARKEN:
      return "darken";
    case DEVELOP_BLEND_SCREEN:
      return "screen";
    case DEVELOP_BLEND_MULTIPLY:
      return "multiply";
    case DEVELOP_BLEND_DIVIDE:
      return "divide";
    case DEVELOP_BLEND_ADD:
      return "add";
    case DEVELOP_BLEND_SUBTRACT:
      return "subtract";
    case DEVELOP_BLEND_GEOMETRIC_MEAN:
      return "geometric-mean";
    case DEVELOP_BLEND_HARMONIC_MEAN:
      return "harmonic-mean";
    case DEVELOP_BLEND_OVERLAY:
      return "overlay";
    case DEVELOP_BLEND_SOFTLIGHT:
      return "softlight";
    case DEVELOP_BLEND_HARDLIGHT:
      return "hardlight";
    case DEVELOP_BLEND_VIVIDLIGHT:
      return "vividlight";
    case DEVELOP_BLEND_LINEARLIGHT:
      return "linearlight";
    case DEVELOP_BLEND_PINLIGHT:
      return "pinlight";
    case DEVELOP_BLEND_LIGHTNESS:
      return "lightness";
    case DEVELOP_BLEND_CHROMATICITY:
      return "chromaticity";
    case DEVELOP_BLEND_LAB_LIGHTNESS:
      return "lab-lightness";
    case DEVELOP_BLEND_LAB_A:
      return "lab-a";
    case DEVELOP_BLEND_LAB_B:
      return "lab-b";
    case DEVELOP_BLEND_LAB_COLOR:
      return "lab-color";
    case DEVELOP_BLEND_RGB_R:
      return "rgb-r";
    case DEVELOP_BLEND_RGB_G:
      return "rgb-g";
    case DEVELOP_BLEND_RGB_B:
      return "rgb-b";
    case DEVELOP_BLEND_HSV_VALUE:
      return "hsv-value";
    case DEVELOP_BLEND_HSV_COLOR:
      return "hsv-color";
    case DEVELOP_BLEND_HUE:
      return "hue";
    case DEVELOP_BLEND_COLOR:
      return "color";
    case DEVELOP_BLEND_COLORADJUST:
      return "coloradjust";
    case DEVELOP_BLEND_DIFFERENCE:
      return "difference-legacy";
    case DEVELOP_BLEND_SUBTRACT_INVERSE:
      return "subtract-inverse";
    case DEVELOP_BLEND_DIVIDE_INVERSE:
      return "divide-inverse";
    case DEVELOP_BLEND_LAB_L:
      return "lab-l";
    default:
      return "unknown";
  }
}

static const gchar *_live_snapshot_blend_colorspace_name(const dt_develop_blend_colorspace_t csp)
{
  switch(csp)
  {
    case DEVELOP_BLEND_CS_RAW:
      return "raw";
    case DEVELOP_BLEND_CS_LAB:
      return "lab";
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
      return "rgb-display";
    case DEVELOP_BLEND_CS_RGB_SCENE:
      return "rgb-scene";
    case DEVELOP_BLEND_CS_NONE:
    default:
      return "unknown";
  }
}

typedef enum dt_live_module_mask_action_t
{
  DT_LIVE_MODULE_MASK_ACTION_INVALID = 0,
  DT_LIVE_MODULE_MASK_ACTION_CLEAR_MASK,
  DT_LIVE_MODULE_MASK_ACTION_REUSE_SAME_SHAPES,
} dt_live_module_mask_action_t;

typedef struct dt_live_module_mask_form_t
{
  dt_mask_id_t formid;
  gint state;
  float opacity;
} dt_live_module_mask_form_t;

static dt_live_module_mask_action_t _live_module_mask_action_from_string(const gchar *action)
{
  if(g_strcmp0(action, "clear-mask") == 0) return DT_LIVE_MODULE_MASK_ACTION_CLEAR_MASK;
  if(g_strcmp0(action, "reuse-same-shapes") == 0) return DT_LIVE_MODULE_MASK_ACTION_REUSE_SAME_SHAPES;
  return DT_LIVE_MODULE_MASK_ACTION_INVALID;
}

static dt_masks_form_t *_live_mask_group_from_module(dt_develop_t *dev, dt_iop_module_t *module)
{
  if(dev == NULL || module == NULL || module->blend_params == NULL) return NULL;
  if(!dt_is_valid_maskid(module->blend_params->mask_id)) return NULL;

  dt_masks_form_t *group = dt_masks_get_from_id(dev, module->blend_params->mask_id);
  if(group == NULL || !(group->type & DT_MASKS_GROUP)) return NULL;
  return group;
}

static GArray *_live_mask_collect_forms(dt_masks_form_t *group)
{
  GArray *forms = g_array_new(FALSE, FALSE, sizeof(dt_live_module_mask_form_t));
  if(group == NULL || !(group->type & DT_MASKS_GROUP)) return forms;

  for(const GList *iter = group->points; iter != NULL; iter = g_list_next(iter))
  {
    const dt_masks_point_group_t *point = iter->data;
    if(point == NULL) continue;

    const dt_live_module_mask_form_t form = {
      .formid = point->formid,
      .state = point->state,
      .opacity = point->opacity,
    };
    g_array_append_val(forms, form);
  }

  return forms;
}

static gboolean _live_mask_forms_equal(const GArray *left, const GArray *right)
{
  const guint left_len = left != NULL ? left->len : 0;
  const guint right_len = right != NULL ? right->len : 0;
  if(left_len != right_len) return FALSE;

  for(guint index = 0; index < left_len; index++)
  {
    const dt_live_module_mask_form_t *left_form = &g_array_index(left, dt_live_module_mask_form_t, index);
    const dt_live_module_mask_form_t *right_form = &g_array_index(right, dt_live_module_mask_form_t, index);
    if(left_form->formid != right_form->formid) return FALSE;
    if(left_form->state != right_form->state) return FALSE;
    if(fabsf(left_form->opacity - right_form->opacity) > 1e-6f) return FALSE;
  }

  return TRUE;
}

static gboolean _live_mask_forms_have_entries(const GArray *forms)
{
  return forms != NULL && forms->len > 0;
}

static void _live_snapshot_add_mask_forms(JsonBuilder *builder,
                                          const gchar *member_name,
                                          const GArray *forms)
{
  json_builder_set_member_name(builder, member_name);
  json_builder_begin_array(builder);

  if(forms != NULL)
  {
    for(guint index = 0; index < forms->len; index++)
    {
      const dt_live_module_mask_form_t *form = &g_array_index(forms, dt_live_module_mask_form_t, index);
      json_builder_begin_object(builder);
      json_builder_set_member_name(builder, "formId");
      json_builder_add_int_value(builder, form->formid);
      json_builder_set_member_name(builder, "state");
      json_builder_add_int_value(builder, form->state);
      json_builder_set_member_name(builder, "opacity");
      json_builder_add_double_value(builder, form->opacity);
      json_builder_end_object(builder);
    }
  }

  json_builder_end_array(builder);
}

static gboolean _live_snapshot_parse_blend_mode_name(const gchar *blend_mode_name,
                                                     uint32_t *blend_mode_out)
{
  if(blend_mode_name == NULL || blend_mode_name[0] == '\0') return FALSE;

  struct dt_live_blend_mode_name_t
  {
    const gchar *name;
    uint32_t mode;
  };

  static const struct dt_live_blend_mode_name_t blend_modes[] = {
    { "normal", DEVELOP_BLEND_NORMAL2 },
    { "average", DEVELOP_BLEND_AVERAGE },
    { "difference", DEVELOP_BLEND_DIFFERENCE2 },
    { "bounded", DEVELOP_BLEND_BOUNDED },
    { "lighten", DEVELOP_BLEND_LIGHTEN },
    { "darken", DEVELOP_BLEND_DARKEN },
    { "screen", DEVELOP_BLEND_SCREEN },
    { "multiply", DEVELOP_BLEND_MULTIPLY },
    { "divide", DEVELOP_BLEND_DIVIDE },
    { "add", DEVELOP_BLEND_ADD },
    { "subtract", DEVELOP_BLEND_SUBTRACT },
    { "geometric-mean", DEVELOP_BLEND_GEOMETRIC_MEAN },
    { "harmonic-mean", DEVELOP_BLEND_HARMONIC_MEAN },
    { "overlay", DEVELOP_BLEND_OVERLAY },
    { "softlight", DEVELOP_BLEND_SOFTLIGHT },
    { "hardlight", DEVELOP_BLEND_HARDLIGHT },
    { "vividlight", DEVELOP_BLEND_VIVIDLIGHT },
    { "linearlight", DEVELOP_BLEND_LINEARLIGHT },
    { "pinlight", DEVELOP_BLEND_PINLIGHT },
    { "lightness", DEVELOP_BLEND_LIGHTNESS },
    { "chromaticity", DEVELOP_BLEND_CHROMATICITY },
    { "lab-lightness", DEVELOP_BLEND_LAB_LIGHTNESS },
    { "lab-a", DEVELOP_BLEND_LAB_A },
    { "lab-b", DEVELOP_BLEND_LAB_B },
    { "lab-color", DEVELOP_BLEND_LAB_COLOR },
    { "rgb-r", DEVELOP_BLEND_RGB_R },
    { "rgb-g", DEVELOP_BLEND_RGB_G },
    { "rgb-b", DEVELOP_BLEND_RGB_B },
    { "hsv-value", DEVELOP_BLEND_HSV_VALUE },
    { "hsv-color", DEVELOP_BLEND_HSV_COLOR },
    { "hue", DEVELOP_BLEND_HUE },
    { "color", DEVELOP_BLEND_COLOR },
    { "coloradjust", DEVELOP_BLEND_COLORADJUST },
    { "difference-legacy", DEVELOP_BLEND_DIFFERENCE },
    { "subtract-inverse", DEVELOP_BLEND_SUBTRACT_INVERSE },
    { "divide-inverse", DEVELOP_BLEND_DIVIDE_INVERSE },
    { "lab-l", DEVELOP_BLEND_LAB_L },
    { NULL, 0 },
  };

  for(const struct dt_live_blend_mode_name_t *blend_mode = blend_modes;
      blend_mode->name != NULL;
      blend_mode++)
  {
    if(g_strcmp0(blend_mode->name, blend_mode_name) == 0)
    {
      if(blend_mode_out != NULL) *blend_mode_out = blend_mode->mode;
      return TRUE;
    }
  }

  return FALSE;
}

static dt_develop_blend_colorspace_t _live_module_blend_colorspace(dt_iop_module_t *module)
{
  if(module == NULL || module->blend_params == NULL) return DEVELOP_BLEND_CS_NONE;

  const dt_develop_blend_colorspace_t default_csp =
    dt_develop_blend_default_module_blend_colorspace(module);
  switch(default_csp)
  {
    case DEVELOP_BLEND_CS_RAW:
      return DEVELOP_BLEND_CS_RAW;
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      switch(module->blend_params->blend_cst)
      {
        case DEVELOP_BLEND_CS_LAB:
        case DEVELOP_BLEND_CS_RGB_DISPLAY:
        case DEVELOP_BLEND_CS_RGB_SCENE:
          return module->blend_params->blend_cst;
        default:
          return default_csp;
      }
    case DEVELOP_BLEND_CS_NONE:
    default:
      return DEVELOP_BLEND_CS_NONE;
  }
}

static gboolean _live_module_blend_mode_supported(const dt_develop_blend_colorspace_t csp,
                                                   const uint32_t blend_mode)
{
  switch(csp)
  {
    case DEVELOP_BLEND_CS_LAB:
      return blend_mode == DEVELOP_BLEND_NORMAL2 || blend_mode == DEVELOP_BLEND_AVERAGE
             || blend_mode == DEVELOP_BLEND_DIFFERENCE || blend_mode == DEVELOP_BLEND_DIFFERENCE2
             || blend_mode == DEVELOP_BLEND_BOUNDED || blend_mode == DEVELOP_BLEND_LIGHTEN
             || blend_mode == DEVELOP_BLEND_DARKEN || blend_mode == DEVELOP_BLEND_SCREEN
             || blend_mode == DEVELOP_BLEND_MULTIPLY || blend_mode == DEVELOP_BLEND_ADD
             || blend_mode == DEVELOP_BLEND_SUBTRACT || blend_mode == DEVELOP_BLEND_OVERLAY
             || blend_mode == DEVELOP_BLEND_SOFTLIGHT || blend_mode == DEVELOP_BLEND_HARDLIGHT
             || blend_mode == DEVELOP_BLEND_VIVIDLIGHT || blend_mode == DEVELOP_BLEND_LINEARLIGHT
             || blend_mode == DEVELOP_BLEND_PINLIGHT || blend_mode == DEVELOP_BLEND_LIGHTNESS
             || blend_mode == DEVELOP_BLEND_CHROMATICITY || blend_mode == DEVELOP_BLEND_HUE
             || blend_mode == DEVELOP_BLEND_COLOR || blend_mode == DEVELOP_BLEND_COLORADJUST
             || blend_mode == DEVELOP_BLEND_LAB_LIGHTNESS || blend_mode == DEVELOP_BLEND_LAB_L
             || blend_mode == DEVELOP_BLEND_LAB_A || blend_mode == DEVELOP_BLEND_LAB_B
             || blend_mode == DEVELOP_BLEND_LAB_COLOR;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
      return blend_mode == DEVELOP_BLEND_NORMAL2 || blend_mode == DEVELOP_BLEND_AVERAGE
             || blend_mode == DEVELOP_BLEND_DIFFERENCE || blend_mode == DEVELOP_BLEND_DIFFERENCE2
             || blend_mode == DEVELOP_BLEND_BOUNDED || blend_mode == DEVELOP_BLEND_LIGHTEN
             || blend_mode == DEVELOP_BLEND_DARKEN || blend_mode == DEVELOP_BLEND_SCREEN
             || blend_mode == DEVELOP_BLEND_MULTIPLY || blend_mode == DEVELOP_BLEND_ADD
             || blend_mode == DEVELOP_BLEND_SUBTRACT || blend_mode == DEVELOP_BLEND_OVERLAY
             || blend_mode == DEVELOP_BLEND_SOFTLIGHT || blend_mode == DEVELOP_BLEND_HARDLIGHT
             || blend_mode == DEVELOP_BLEND_VIVIDLIGHT || blend_mode == DEVELOP_BLEND_LINEARLIGHT
             || blend_mode == DEVELOP_BLEND_PINLIGHT || blend_mode == DEVELOP_BLEND_LIGHTNESS
             || blend_mode == DEVELOP_BLEND_CHROMATICITY || blend_mode == DEVELOP_BLEND_HUE
             || blend_mode == DEVELOP_BLEND_COLOR || blend_mode == DEVELOP_BLEND_COLORADJUST
             || blend_mode == DEVELOP_BLEND_HSV_VALUE || blend_mode == DEVELOP_BLEND_HSV_COLOR
             || blend_mode == DEVELOP_BLEND_RGB_R || blend_mode == DEVELOP_BLEND_RGB_G
             || blend_mode == DEVELOP_BLEND_RGB_B;
    case DEVELOP_BLEND_CS_RAW:
      return blend_mode == DEVELOP_BLEND_NORMAL2 || blend_mode == DEVELOP_BLEND_AVERAGE
             || blend_mode == DEVELOP_BLEND_DIFFERENCE || blend_mode == DEVELOP_BLEND_DIFFERENCE2
             || blend_mode == DEVELOP_BLEND_BOUNDED || blend_mode == DEVELOP_BLEND_LIGHTEN
             || blend_mode == DEVELOP_BLEND_DARKEN || blend_mode == DEVELOP_BLEND_SCREEN
             || blend_mode == DEVELOP_BLEND_MULTIPLY || blend_mode == DEVELOP_BLEND_ADD
             || blend_mode == DEVELOP_BLEND_SUBTRACT || blend_mode == DEVELOP_BLEND_OVERLAY
             || blend_mode == DEVELOP_BLEND_SOFTLIGHT || blend_mode == DEVELOP_BLEND_HARDLIGHT
             || blend_mode == DEVELOP_BLEND_VIVIDLIGHT || blend_mode == DEVELOP_BLEND_LINEARLIGHT
             || blend_mode == DEVELOP_BLEND_PINLIGHT;
    case DEVELOP_BLEND_CS_RGB_SCENE:
      return blend_mode == DEVELOP_BLEND_NORMAL2 || blend_mode == DEVELOP_BLEND_AVERAGE
             || blend_mode == DEVELOP_BLEND_DIFFERENCE || blend_mode == DEVELOP_BLEND_DIFFERENCE2
             || blend_mode == DEVELOP_BLEND_MULTIPLY || blend_mode == DEVELOP_BLEND_DIVIDE
             || blend_mode == DEVELOP_BLEND_DIVIDE_INVERSE || blend_mode == DEVELOP_BLEND_ADD
             || blend_mode == DEVELOP_BLEND_SUBTRACT
             || blend_mode == DEVELOP_BLEND_SUBTRACT_INVERSE
             || blend_mode == DEVELOP_BLEND_GEOMETRIC_MEAN
             || blend_mode == DEVELOP_BLEND_HARMONIC_MEAN || blend_mode == DEVELOP_BLEND_RGB_R
             || blend_mode == DEVELOP_BLEND_RGB_G || blend_mode == DEVELOP_BLEND_RGB_B
             || blend_mode == DEVELOP_BLEND_LIGHTNESS || blend_mode == DEVELOP_BLEND_CHROMATICITY;
    case DEVELOP_BLEND_CS_NONE:
    default:
      return FALSE;
  }
}

static gboolean _live_module_blend_parameter_enabled(const dt_develop_blend_colorspace_t csp,
                                                     const uint32_t blend_mode)
{
  if(csp == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    switch(blend_mode & ~DEVELOP_BLEND_REVERSE)
    {
      case DEVELOP_BLEND_ADD:
      case DEVELOP_BLEND_MULTIPLY:
      case DEVELOP_BLEND_SUBTRACT:
      case DEVELOP_BLEND_SUBTRACT_INVERSE:
      case DEVELOP_BLEND_DIVIDE:
      case DEVELOP_BLEND_DIVIDE_INVERSE:
      case DEVELOP_BLEND_RGB_R:
      case DEVELOP_BLEND_RGB_G:
      case DEVELOP_BLEND_RGB_B:
        return TRUE;
      default:
        return FALSE;
    }
  }

  return FALSE;
}

static void _live_snapshot_add_blend(JsonBuilder *builder,
                                     const char *module_op,
                                     const dt_iop_module_t *module,
                                     const dt_develop_blend_params_t *blend_params)
{
  const int flags = module != NULL ? module->flags() : dt_iop_get_module_flags(module_op);
  const gboolean supported = blend_params != NULL && (flags & IOP_FLAGS_SUPPORTS_BLENDING);
  const gboolean masks_supported = supported && !(flags & IOP_FLAGS_NO_MASKS);

  json_builder_set_member_name(builder, "blend");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "supported");
  json_builder_add_boolean_value(builder, supported);
  json_builder_set_member_name(builder, "masksSupported");
  json_builder_add_boolean_value(builder, masks_supported);

  if(supported)
  {
    const dt_develop_blend_colorspace_t blend_csp = _live_module_blend_colorspace((dt_iop_module_t *)module);
    json_builder_set_member_name(builder, "opacity");
    json_builder_add_double_value(builder, blend_params->opacity);
    json_builder_set_member_name(builder, "blendMode");
    json_builder_add_string_value(builder, _live_snapshot_blend_mode_name(blend_params->blend_mode));
    json_builder_set_member_name(builder, "reverseOrder");
    json_builder_add_boolean_value(builder,
                                   (blend_params->blend_mode & DEVELOP_BLEND_REVERSE)
                                     == DEVELOP_BLEND_REVERSE);
    json_builder_set_member_name(builder, "blendColorspace");
    json_builder_add_string_value(builder, _live_snapshot_blend_colorspace_name(blend_csp));
  }

  json_builder_end_object(builder);
}

static void _live_snapshot_add_stack_item(JsonBuilder *builder, dt_iop_module_t *module)
{
  g_autofree gchar *instance_key =
    _live_snapshot_instance_key(module->op, module->instance, module->multi_priority, module->multi_name);

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "instanceKey");
  json_builder_add_string_value(builder, instance_key);
  json_builder_set_member_name(builder, "moduleOp");
  json_builder_add_string_value(builder, module->op);
  json_builder_set_member_name(builder, "enabled");
  json_builder_add_boolean_value(builder, module->enabled);
  json_builder_set_member_name(builder, "iopOrder");
  json_builder_add_int_value(builder, module->iop_order);
  json_builder_set_member_name(builder, "multiPriority");
  json_builder_add_int_value(builder, module->multi_priority);
  json_builder_set_member_name(builder, "multiName");
  json_builder_add_string_value(builder, module->multi_name);
  _live_snapshot_add_params(builder, module, module->params);
  _live_snapshot_add_blend(builder, module->op, module, module->blend_params);
  json_builder_end_object(builder);
}

static void _live_snapshot_add_history_item(JsonBuilder *builder,
                                            const dt_dev_history_item_t *history_item,
                                            const gint index,
                                            const gint history_end)
{
  const dt_iop_module_t *module = history_item->module;
  const char *module_op = module ? module->op : history_item->op_name;
  const gint instance = module ? module->instance : -1;
  g_autofree gchar *instance_key =
    _live_snapshot_instance_key(module_op, instance, history_item->multi_priority, history_item->multi_name);

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "index");
  json_builder_add_int_value(builder, index);
  json_builder_set_member_name(builder, "applied");
  json_builder_add_boolean_value(builder, index < history_end);
  json_builder_set_member_name(builder, "instanceKey");
  json_builder_add_string_value(builder, instance_key);
  json_builder_set_member_name(builder, "moduleOp");
  json_builder_add_string_value(builder, module_op ? module_op : "unknown");
  json_builder_set_member_name(builder, "enabled");
  json_builder_add_boolean_value(builder, history_item->enabled);
  json_builder_set_member_name(builder, "iopOrder");
  json_builder_add_int_value(builder, history_item->iop_order);
  json_builder_set_member_name(builder, "multiPriority");
  json_builder_add_int_value(builder, history_item->multi_priority);
  json_builder_set_member_name(builder, "multiName");
  json_builder_add_string_value(builder, history_item->multi_name);
  _live_snapshot_add_params(builder, history_item->module, history_item->params);
  _live_snapshot_add_blend(builder, module_op, module, history_item->blend_params);
  json_builder_end_object(builder);
}

static gchar *_live_snapshot_to_json(dt_develop_t *dev)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "appliedHistoryEnd");
  json_builder_add_int_value(builder, dev->history_end);

  json_builder_set_member_name(builder, "moduleStack");
  json_builder_begin_array(builder);
  for(const GList *iter = dev->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *module = iter->data;
    if(module == NULL || dt_iop_is_hidden(module)) continue;
    _live_snapshot_add_stack_item(builder, module);
  }
  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "historyItems");
  json_builder_begin_array(builder);
  dt_pthread_mutex_lock(&dev->history_mutex);
  int index = 0;
  for(const GList *iter = dev->history; iter; iter = g_list_next(iter), index++)
  {
    dt_dev_history_item_t *history_item = iter->data;
    if(history_item == NULL) continue;
    _live_snapshot_add_history_item(builder, history_item, index, dev->history_end);
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);
  json_builder_end_array(builder);
  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, FALSE);
  g_autofree gchar *json = json_generator_to_data(generator, NULL);
  json_node_free(root);
  return g_strdup(json);
}

static gchar *_live_json_builder_to_string(JsonBuilder *builder)
{
  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, FALSE);
  g_autofree gchar *json = json_generator_to_data(generator, NULL);
  json_node_free(root);
  return g_strdup(json);
}

static JsonNode *_live_json_copy_object_root(const gchar *json)
{
  if(json == NULL || json[0] == '\0') return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  if(!json_parser_load_from_data(parser, json, -1, NULL)) return NULL;

  JsonNode *root = json_parser_get_root(parser);
  if(root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;

  return json_node_copy(root);
}

static gboolean _live_snapshot_add_active_image(JsonBuilder *builder, dt_develop_t *dev)
{
  if(builder == NULL || dev == NULL || dev->image_storage.id == NO_IMGID) return FALSE;

  const dt_image_t *image = dt_image_cache_get(dev->image_storage.id, 'r');
  if(image == NULL) return FALSE;

  char directory_path[PATH_MAX] = { 0 };
  dt_image_film_roll_directory(image, directory_path, sizeof(directory_path));
  g_autofree gchar *source_asset_path = g_build_filename(directory_path, image->filename, NULL);

  json_builder_set_member_name(builder, "activeImage");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "imageId");
  json_builder_add_int_value(builder, image->id);
  json_builder_set_member_name(builder, "directoryPath");
  json_builder_add_string_value(builder, directory_path);
  json_builder_set_member_name(builder, "fileName");
  json_builder_add_string_value(builder, image->filename);
  json_builder_set_member_name(builder, "sourceAssetPath");
  json_builder_add_string_value(builder, source_asset_path);
  json_builder_end_object(builder);

  dt_image_cache_read_release(image);
  return TRUE;
}

static dt_iop_module_t *_live_snapshot_find_visible_module(dt_develop_t *dev, const gchar *instance_key)
{
  if(dev == NULL || instance_key == NULL || instance_key[0] == '\0') return NULL;

  for(const GList *iter = dev->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *module = iter->data;
    if(module == NULL || dt_iop_is_hidden(module)) continue;

    g_autofree gchar *candidate_key =
      _live_snapshot_instance_key(module->op, module->instance, module->multi_priority, module->multi_name);
    if(g_strcmp0(candidate_key, instance_key) == 0) return module;
  }

  return NULL;
}

typedef enum dt_live_module_instance_action_t
{
  DT_LIVE_MODULE_INSTANCE_ACTION_INVALID = 0,
  DT_LIVE_MODULE_INSTANCE_ACTION_ENABLE,
  DT_LIVE_MODULE_INSTANCE_ACTION_DISABLE,
  DT_LIVE_MODULE_INSTANCE_ACTION_CREATE,
  DT_LIVE_MODULE_INSTANCE_ACTION_DUPLICATE,
  DT_LIVE_MODULE_INSTANCE_ACTION_DELETE,
  DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_BEFORE,
  DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_AFTER,
} dt_live_module_instance_action_t;

typedef struct dt_live_module_action_payload_t
{
  const gchar *instance_key;
  const gchar *action;
  const gchar *anchor_instance_key;
  gboolean have_requested_enabled;
  gboolean requested_enabled;
  dt_iop_module_t *module;
  dt_iop_module_t *result_module;
  dt_iop_module_t *replacement_module;
  gboolean have_previous_enabled;
  gboolean previous_enabled;
  gboolean have_current_enabled;
  gboolean current_enabled;
  gboolean have_previous_iop_order;
  gint previous_iop_order;
  gboolean have_current_iop_order;
  gint current_iop_order;
  gboolean have_history;
  gint history_before;
  gint history_after;
} dt_live_module_action_payload_t;

typedef struct dt_live_module_blend_payload_t
{
  const gchar *instance_key;
  dt_iop_module_t *module;
  gboolean have_previous_opacity;
  double previous_opacity;
  gboolean have_requested_opacity;
  double requested_opacity;
  gboolean have_current_opacity;
  double current_opacity;
  gboolean have_previous_blend_mode;
  const gchar *previous_blend_mode;
  gboolean have_requested_blend_mode;
  const gchar *requested_blend_mode;
  gboolean have_current_blend_mode;
  const gchar *current_blend_mode;
  gboolean have_previous_reverse_order;
  gboolean previous_reverse_order;
  gboolean have_requested_reverse_order;
  gboolean requested_reverse_order;
  gboolean have_current_reverse_order;
  gboolean current_reverse_order;
  gboolean have_history;
  gint history_before;
  gint history_after;
} dt_live_module_blend_payload_t;

typedef struct dt_live_module_blend_request_t
{
  gboolean have_opacity;
  double opacity;
  gboolean have_blend_mode;
  uint32_t blend_mode;
  gboolean have_reverse_order;
  gboolean reverse_order;
} dt_live_module_blend_request_t;

typedef struct dt_live_module_mask_payload_t
{
  const gchar *instance_key;
  const gchar *action;
  const gchar *source_instance_key;
  dt_iop_module_t *module;
  gboolean have_previous_has_mask;
  gboolean previous_has_mask;
  gboolean have_current_has_mask;
  gboolean current_has_mask;
  gboolean have_changed;
  gboolean changed;
  GArray *previous_forms;
  GArray *source_forms;
  GArray *current_forms;
  gboolean have_history;
  gint history_before;
  gint history_after;
} dt_live_module_mask_payload_t;

typedef struct dt_live_module_mask_request_t
{
  dt_live_module_mask_action_t action;
  gchar *source_instance_key;
} dt_live_module_mask_request_t;

typedef enum dt_live_module_reorder_check_t
{
  DT_LIVE_MODULE_REORDER_CHECK_OK = 0,
  DT_LIVE_MODULE_REORDER_CHECK_NO_OP,
  DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_FENCE,
  DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_RULE,
} dt_live_module_reorder_check_t;

static dt_live_module_instance_action_t _live_module_instance_action_from_string(const gchar *action,
                                                                                 gboolean *enabled_out)
{
  if(g_strcmp0(action, "enable") == 0)
  {
    if(enabled_out != NULL) *enabled_out = TRUE;
    return DT_LIVE_MODULE_INSTANCE_ACTION_ENABLE;
  }
  if(g_strcmp0(action, "disable") == 0)
  {
    if(enabled_out != NULL) *enabled_out = FALSE;
    return DT_LIVE_MODULE_INSTANCE_ACTION_DISABLE;
  }
  if(g_strcmp0(action, "create") == 0)
    return DT_LIVE_MODULE_INSTANCE_ACTION_CREATE;
  if(g_strcmp0(action, "duplicate") == 0)
    return DT_LIVE_MODULE_INSTANCE_ACTION_DUPLICATE;
  if(g_strcmp0(action, "delete") == 0)
    return DT_LIVE_MODULE_INSTANCE_ACTION_DELETE;
  if(g_strcmp0(action, "move-before") == 0)
    return DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_BEFORE;
  if(g_strcmp0(action, "move-after") == 0)
    return DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_AFTER;
  return DT_LIVE_MODULE_INSTANCE_ACTION_INVALID;
}

static gboolean _live_module_instance_action_requires_anchor(const dt_live_module_instance_action_t action_kind)
{
  return action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_BEFORE
         || action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_AFTER;
}

static void _live_snapshot_add_module_action(JsonBuilder *builder,
                                             const dt_live_module_action_payload_t *payload)
{
  json_builder_set_member_name(builder, "moduleAction");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "targetInstanceKey");
  json_builder_add_string_value(builder, payload != NULL && payload->instance_key ? payload->instance_key : "");
  json_builder_set_member_name(builder, "action");
  json_builder_add_string_value(builder, payload != NULL && payload->action ? payload->action : "");

  if(payload != NULL && payload->anchor_instance_key != NULL)
  {
    json_builder_set_member_name(builder, "anchorInstanceKey");
    json_builder_add_string_value(builder, payload->anchor_instance_key);
  }

  if(payload != NULL && payload->have_requested_enabled)
  {
    json_builder_set_member_name(builder, "requestedEnabled");
    json_builder_add_boolean_value(builder, payload->requested_enabled);
  }

  if(payload != NULL && payload->module != NULL)
  {
    json_builder_set_member_name(builder, "moduleOp");
    json_builder_add_string_value(builder, payload->module->op);
    json_builder_set_member_name(builder, "iopOrder");
    json_builder_add_int_value(builder, payload->module->iop_order);
    json_builder_set_member_name(builder, "multiPriority");
    json_builder_add_int_value(builder, payload->module->multi_priority);
    json_builder_set_member_name(builder, "multiName");
    json_builder_add_string_value(builder, payload->module->multi_name);
  }

  if(payload != NULL && payload->result_module != NULL)
  {
    g_autofree gchar *result_instance_key =
      _live_snapshot_instance_key(payload->result_module->op, payload->result_module->instance,
                                  payload->result_module->multi_priority, payload->result_module->multi_name);
    json_builder_set_member_name(builder, "resultInstanceKey");
    json_builder_add_string_value(builder, result_instance_key);
    if(payload->module == NULL)
    {
      json_builder_set_member_name(builder, "moduleOp");
      json_builder_add_string_value(builder, payload->result_module->op);
      json_builder_set_member_name(builder, "iopOrder");
      json_builder_add_int_value(builder, payload->result_module->iop_order);
      json_builder_set_member_name(builder, "multiPriority");
      json_builder_add_int_value(builder, payload->result_module->multi_priority);
      json_builder_set_member_name(builder, "multiName");
      json_builder_add_string_value(builder, payload->result_module->multi_name);
    }
  }

  if(payload != NULL && payload->replacement_module != NULL)
  {
    g_autofree gchar *replacement_instance_key =
      _live_snapshot_instance_key(payload->replacement_module->op, payload->replacement_module->instance,
                                  payload->replacement_module->multi_priority,
                                  payload->replacement_module->multi_name);
    json_builder_set_member_name(builder, "replacementInstanceKey");
    json_builder_add_string_value(builder, replacement_instance_key);
    json_builder_set_member_name(builder, "replacementIopOrder");
    json_builder_add_int_value(builder, payload->replacement_module->iop_order);
    json_builder_set_member_name(builder, "replacementMultiPriority");
    json_builder_add_int_value(builder, payload->replacement_module->multi_priority);
    json_builder_set_member_name(builder, "replacementMultiName");
    json_builder_add_string_value(builder, payload->replacement_module->multi_name);
  }

  if(payload != NULL && payload->have_previous_enabled)
  {
    json_builder_set_member_name(builder, "previousEnabled");
    json_builder_add_boolean_value(builder, payload->previous_enabled);
  }

  if(payload != NULL && payload->have_current_enabled)
  {
    json_builder_set_member_name(builder, "currentEnabled");
    json_builder_add_boolean_value(builder, payload->current_enabled);
  }

  if(payload != NULL && payload->have_previous_enabled && payload->have_current_enabled)
  {
    json_builder_set_member_name(builder, "changed");
    json_builder_add_boolean_value(builder, payload->previous_enabled != payload->current_enabled);
  }

  if(payload != NULL && payload->have_previous_iop_order)
  {
    json_builder_set_member_name(builder, "previousIopOrder");
    json_builder_add_int_value(builder, payload->previous_iop_order);
  }

  if(payload != NULL && payload->have_current_iop_order)
  {
    json_builder_set_member_name(builder, "currentIopOrder");
    json_builder_add_int_value(builder, payload->current_iop_order);
  }

  if(payload != NULL && payload->have_history)
  {
    json_builder_set_member_name(builder, "historyBefore");
    json_builder_add_int_value(builder, payload->history_before);
    json_builder_set_member_name(builder, "historyAfter");
    json_builder_add_int_value(builder, payload->history_after);
    json_builder_set_member_name(builder, "requestedHistoryEnd");
    json_builder_add_int_value(builder, payload->history_after);
  }

  json_builder_end_object(builder);
}

static void _live_snapshot_add_module_blend(JsonBuilder *builder,
                                            const dt_live_module_blend_payload_t *payload)
{
  json_builder_set_member_name(builder, "moduleBlend");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "targetInstanceKey");
  json_builder_add_string_value(builder, payload != NULL && payload->instance_key ? payload->instance_key : "");

  if(payload != NULL && payload->module != NULL)
  {
    json_builder_set_member_name(builder, "moduleOp");
    json_builder_add_string_value(builder, payload->module->op);
    json_builder_set_member_name(builder, "iopOrder");
    json_builder_add_int_value(builder, payload->module->iop_order);
    json_builder_set_member_name(builder, "multiPriority");
    json_builder_add_int_value(builder, payload->module->multi_priority);
    json_builder_set_member_name(builder, "multiName");
    json_builder_add_string_value(builder, payload->module->multi_name);
  }

  if(payload != NULL && payload->have_previous_opacity)
  {
    json_builder_set_member_name(builder, "previousOpacity");
    json_builder_add_double_value(builder, payload->previous_opacity);
  }

  if(payload != NULL && payload->have_requested_opacity)
  {
    json_builder_set_member_name(builder, "requestedOpacity");
    json_builder_add_double_value(builder, payload->requested_opacity);
  }

  if(payload != NULL && payload->have_current_opacity)
  {
    json_builder_set_member_name(builder, "currentOpacity");
    json_builder_add_double_value(builder, payload->current_opacity);
  }

  if(payload != NULL && payload->have_previous_blend_mode)
  {
    json_builder_set_member_name(builder, "previousBlendMode");
    json_builder_add_string_value(builder, payload->previous_blend_mode);
  }

  if(payload != NULL && payload->have_requested_blend_mode)
  {
    json_builder_set_member_name(builder, "requestedBlendMode");
    json_builder_add_string_value(builder, payload->requested_blend_mode);
  }

  if(payload != NULL && payload->have_current_blend_mode)
  {
    json_builder_set_member_name(builder, "currentBlendMode");
    json_builder_add_string_value(builder, payload->current_blend_mode);
  }

  if(payload != NULL && payload->have_previous_reverse_order)
  {
    json_builder_set_member_name(builder, "previousReverseOrder");
    json_builder_add_boolean_value(builder, payload->previous_reverse_order);
  }

  if(payload != NULL && payload->have_requested_reverse_order)
  {
    json_builder_set_member_name(builder, "requestedReverseOrder");
    json_builder_add_boolean_value(builder, payload->requested_reverse_order);
  }

  if(payload != NULL && payload->have_current_reverse_order)
  {
    json_builder_set_member_name(builder, "currentReverseOrder");
    json_builder_add_boolean_value(builder, payload->current_reverse_order);
  }

  if(payload != NULL && payload->have_history)
  {
    json_builder_set_member_name(builder, "historyBefore");
    json_builder_add_int_value(builder, payload->history_before);
    json_builder_set_member_name(builder, "historyAfter");
    json_builder_add_int_value(builder, payload->history_after);
    json_builder_set_member_name(builder, "requestedHistoryEnd");
    json_builder_add_int_value(builder, payload->history_after);
  }

  json_builder_end_object(builder);
}

static void _live_snapshot_add_module_mask(JsonBuilder *builder,
                                           const dt_live_module_mask_payload_t *payload)
{
  json_builder_set_member_name(builder, "moduleMask");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "targetInstanceKey");
  json_builder_add_string_value(builder, payload != NULL && payload->instance_key ? payload->instance_key : "");
  json_builder_set_member_name(builder, "action");
  json_builder_add_string_value(builder, payload != NULL && payload->action ? payload->action : "");

  if(payload != NULL && payload->source_instance_key != NULL)
  {
    json_builder_set_member_name(builder, "sourceInstanceKey");
    json_builder_add_string_value(builder, payload->source_instance_key);
  }

  if(payload != NULL && payload->module != NULL)
  {
    json_builder_set_member_name(builder, "moduleOp");
    json_builder_add_string_value(builder, payload->module->op);
    json_builder_set_member_name(builder, "iopOrder");
    json_builder_add_int_value(builder, payload->module->iop_order);
    json_builder_set_member_name(builder, "multiPriority");
    json_builder_add_int_value(builder, payload->module->multi_priority);
    json_builder_set_member_name(builder, "multiName");
    json_builder_add_string_value(builder, payload->module->multi_name);
  }

  if(payload != NULL && payload->have_previous_has_mask)
  {
    json_builder_set_member_name(builder, "previousHasMask");
    json_builder_add_boolean_value(builder, payload->previous_has_mask);
  }

  if(payload != NULL && payload->have_current_has_mask)
  {
    json_builder_set_member_name(builder, "currentHasMask");
    json_builder_add_boolean_value(builder, payload->current_has_mask);
  }

  if(payload != NULL && payload->have_changed)
  {
    json_builder_set_member_name(builder, "changed");
    json_builder_add_boolean_value(builder, payload->changed);
  }

  _live_snapshot_add_mask_forms(builder, "previousForms", payload != NULL ? payload->previous_forms : NULL);
  _live_snapshot_add_mask_forms(builder, "sourceForms", payload != NULL ? payload->source_forms : NULL);
  _live_snapshot_add_mask_forms(builder, "currentForms", payload != NULL ? payload->current_forms : NULL);

  if(payload != NULL && payload->have_history)
  {
    json_builder_set_member_name(builder, "historyBefore");
    json_builder_add_int_value(builder, payload->history_before);
    json_builder_set_member_name(builder, "historyAfter");
    json_builder_add_int_value(builder, payload->history_after);
    json_builder_set_member_name(builder, "requestedHistoryEnd");
    json_builder_add_int_value(builder, payload->history_after);
  }

  json_builder_end_object(builder);
}

static gboolean _live_parse_module_instance_blend_request(const gchar *json_text,
                                                          dt_live_module_blend_request_t *request_out)
{
  if(json_text == NULL || json_text[0] == '\0' || request_out == NULL) return FALSE;

  g_autoptr(JsonParser) parser = json_parser_new();
  if(!json_parser_load_from_data(parser, json_text, -1, NULL)) return FALSE;

  JsonNode *root = json_parser_get_root(parser);
  if(root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) return FALSE;

  JsonObject *object = json_node_get_object(root);
  if(object == NULL) return FALSE;

  dt_live_module_blend_request_t request = { 0 };
  GList *members = json_object_get_members(object);
  for(const GList *iter = members; iter != NULL; iter = g_list_next(iter))
  {
    const gchar *key = iter->data;
    JsonNode *member = json_object_get_member(object, key);

    if(g_strcmp0(key, "opacity") == 0)
    {
      const GType value_type = member != NULL && JSON_NODE_HOLDS_VALUE(member)
                                  ? json_node_get_value_type(member)
                                  : G_TYPE_INVALID;
      if(value_type != G_TYPE_DOUBLE && value_type != G_TYPE_INT64 && value_type != G_TYPE_INT
         && value_type != G_TYPE_UINT64 && value_type != G_TYPE_UINT && value_type != G_TYPE_LONG
         && value_type != G_TYPE_ULONG)
      {
        g_list_free(members);
        return FALSE;
      }

      request.have_opacity = TRUE;
      request.opacity = json_node_get_double(member);
    }
    else if(g_strcmp0(key, "blendMode") == 0)
    {
      if(member == NULL || !JSON_NODE_HOLDS_VALUE(member)
         || json_node_get_value_type(member) != G_TYPE_STRING)
      {
        g_list_free(members);
        return FALSE;
      }

      request.have_blend_mode =
        _live_snapshot_parse_blend_mode_name(json_node_get_string(member), &request.blend_mode);
      if(!request.have_blend_mode)
      {
        g_list_free(members);
        return FALSE;
      }
    }
    else if(g_strcmp0(key, "reverseOrder") == 0)
    {
      if(member == NULL || !JSON_NODE_HOLDS_VALUE(member)
         || json_node_get_value_type(member) != G_TYPE_BOOLEAN)
      {
        g_list_free(members);
        return FALSE;
      }

      request.have_reverse_order = TRUE;
      request.reverse_order = json_node_get_boolean(member);
    }
    else
    {
      g_list_free(members);
      return FALSE;
    }
  }
  g_list_free(members);

  if(!request.have_opacity && !request.have_blend_mode && !request.have_reverse_order) return FALSE;

  *request_out = request;
  return TRUE;
}

static gboolean _live_parse_module_instance_mask_request(const gchar *json_text,
                                                         dt_live_module_mask_request_t *request_out)
{
  if(json_text == NULL || json_text[0] == '\0' || request_out == NULL) return FALSE;

  g_autoptr(JsonParser) parser = json_parser_new();
  if(!json_parser_load_from_data(parser, json_text, -1, NULL)) return FALSE;

  JsonNode *root = json_parser_get_root(parser);
  if(root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) return FALSE;

  JsonObject *object = json_node_get_object(root);
  if(object == NULL) return FALSE;

  dt_live_module_mask_request_t request = { 0 };
  GList *members = json_object_get_members(object);
  for(const GList *iter = members; iter != NULL; iter = g_list_next(iter))
  {
    const gchar *key = iter->data;
    JsonNode *member = json_object_get_member(object, key);

    if(g_strcmp0(key, "action") == 0)
    {
      if(member == NULL || !JSON_NODE_HOLDS_VALUE(member)
         || json_node_get_value_type(member) != G_TYPE_STRING)
      {
        g_list_free(members);
        return FALSE;
      }

      request.action = _live_module_mask_action_from_string(json_node_get_string(member));
      if(request.action == DT_LIVE_MODULE_MASK_ACTION_INVALID)
      {
        g_free(request.source_instance_key);
        g_list_free(members);
        return FALSE;
      }
    }
    else if(g_strcmp0(key, "sourceInstanceKey") == 0)
    {
      if(member == NULL || !JSON_NODE_HOLDS_VALUE(member)
         || json_node_get_value_type(member) != G_TYPE_STRING)
      {
        g_list_free(members);
        return FALSE;
      }

      request.source_instance_key = g_strdup(json_node_get_string(member));
    }
    else
    {
      g_free(request.source_instance_key);
      g_list_free(members);
      return FALSE;
    }
  }
  g_list_free(members);

  if(request.action == DT_LIVE_MODULE_MASK_ACTION_INVALID)
  {
    g_free(request.source_instance_key);
    return FALSE;
  }
  if(request.action == DT_LIVE_MODULE_MASK_ACTION_REUSE_SAME_SHAPES
     && (request.source_instance_key == NULL || request.source_instance_key[0] == '\0'))
  {
    g_free(request.source_instance_key);
    return FALSE;
  }
  if(request.action != DT_LIVE_MODULE_MASK_ACTION_REUSE_SAME_SHAPES
     && request.source_instance_key != NULL)
  {
    g_free(request.source_instance_key);
    return FALSE;
  }

  *request_out = request;
  return TRUE;
}

static gchar *_live_apply_module_instance_blend_to_json(dt_develop_t *dev,
                                                        const gchar *instance_key,
                                                        const dt_live_module_blend_request_t *request)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  dt_live_module_blend_payload_t blend_payload = {
    .instance_key = instance_key,
  };

  if(request != NULL && request->have_opacity)
  {
    blend_payload.have_requested_opacity = TRUE;
    blend_payload.requested_opacity = request->opacity;
  }
  if(request != NULL && request->have_blend_mode)
  {
    blend_payload.have_requested_blend_mode = TRUE;
    blend_payload.requested_blend_mode = _live_snapshot_blend_mode_name(request->blend_mode);
  }
  if(request != NULL && request->have_reverse_order)
  {
    blend_payload.have_requested_reverse_order = TRUE;
    blend_payload.requested_reverse_order = request->reverse_order;
  }

  if(dt_view_get_current() != DT_VIEW_DARKROOM)
  {
    _live_snapshot_add_module_blend(builder, &blend_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unsupported-view");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  if(dev == NULL || dev->image_storage.id == NO_IMGID)
  {
    _live_snapshot_add_module_blend(builder, &blend_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "no-active-image");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  _live_snapshot_add_active_image(builder, dev);

  dt_iop_module_t *module = _live_snapshot_find_visible_module(dev, instance_key);
  if(module == NULL)
  {
    _live_snapshot_add_module_blend(builder, &blend_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unknown-instance-key");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  blend_payload.module = module;

  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || module->blend_params == NULL)
  {
    blend_payload.have_history = TRUE;
    blend_payload.history_before = dev->history_end;
    blend_payload.history_after = dev->history_end;
    _live_snapshot_add_module_blend(builder, &blend_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unsupported-module-blend");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  const double previous_opacity = module->blend_params->opacity;
  const uint32_t previous_blend_mode = module->blend_params->blend_mode & DEVELOP_BLEND_MODE_MASK;
  const gboolean previous_reverse_order =
    (module->blend_params->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE;
  const gint history_before = dev->history_end;
  const dt_develop_blend_colorspace_t blend_csp = _live_module_blend_colorspace(module);

  if(request != NULL && request->have_opacity)
  {
    blend_payload.have_previous_opacity = TRUE;
    blend_payload.previous_opacity = previous_opacity;
  }
  if(request != NULL && request->have_blend_mode)
  {
    blend_payload.have_previous_blend_mode = TRUE;
    blend_payload.previous_blend_mode = _live_snapshot_blend_mode_name(previous_blend_mode);
  }
  if(request != NULL && request->have_reverse_order)
  {
    blend_payload.have_previous_reverse_order = TRUE;
    blend_payload.previous_reverse_order = previous_reverse_order;
  }

  if(request != NULL && request->have_blend_mode
     && !_live_module_blend_mode_supported(blend_csp, request->blend_mode))
  {
    if(request->have_opacity)
    {
      blend_payload.have_current_opacity = TRUE;
      blend_payload.current_opacity = previous_opacity;
    }
    if(request->have_blend_mode)
    {
      blend_payload.have_current_blend_mode = TRUE;
      blend_payload.current_blend_mode = _live_snapshot_blend_mode_name(previous_blend_mode);
    }
    if(request->have_reverse_order)
    {
      blend_payload.have_current_reverse_order = TRUE;
      blend_payload.current_reverse_order = previous_reverse_order;
    }
    blend_payload.have_history = TRUE;
    blend_payload.history_before = history_before;
    blend_payload.history_after = history_before;
    _live_snapshot_add_module_blend(builder, &blend_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unsupported-module-blend-mode");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  dt_develop_blend_params_t updated_blend_params = *module->blend_params;

  if(request != NULL && request->have_opacity)
  {
    updated_blend_params.opacity = request->opacity;
  }

  if(request != NULL && request->have_blend_mode)
  {
    updated_blend_params.blend_mode =
      request->blend_mode | (updated_blend_params.blend_mode & DEVELOP_BLEND_REVERSE);
    if(!_live_module_blend_parameter_enabled(blend_csp, updated_blend_params.blend_mode))
    {
      updated_blend_params.blend_parameter = 0.0f;
    }
  }

  if(request != NULL && request->have_reverse_order)
  {
    updated_blend_params.blend_mode &= ~DEVELOP_BLEND_REVERSE;
    if(request->reverse_order) updated_blend_params.blend_mode |= DEVELOP_BLEND_REVERSE;
  }

  const gboolean blend_changed =
    (request != NULL && request->have_opacity && fabs(previous_opacity - updated_blend_params.opacity) > 1e-6)
    || (request != NULL && request->have_blend_mode
        && previous_blend_mode != (updated_blend_params.blend_mode & DEVELOP_BLEND_MODE_MASK))
    || (request != NULL && request->have_reverse_order
        && previous_reverse_order
             != ((updated_blend_params.blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE));

  if(blend_changed)
  {
    dt_iop_commit_blend_params(module, &updated_blend_params);
    dt_dev_add_history_item(dev, module, module->enabled);
    dt_iop_gui_update_blending(module);
  }

  const double current_opacity = module->blend_params->opacity;
  const uint32_t current_blend_mode = module->blend_params->blend_mode & DEVELOP_BLEND_MODE_MASK;
  const gboolean current_reverse_order =
    (module->blend_params->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE;
  const gint history_after = dev->history_end;
  g_autofree gchar *snapshot_json = _live_snapshot_to_json(dev);

  if(request != NULL && request->have_opacity)
  {
    blend_payload.have_current_opacity = TRUE;
    blend_payload.current_opacity = current_opacity;
  }
  if(request != NULL && request->have_blend_mode)
  {
    blend_payload.have_current_blend_mode = TRUE;
    blend_payload.current_blend_mode = _live_snapshot_blend_mode_name(current_blend_mode);
  }
  if(request != NULL && request->have_reverse_order)
  {
    blend_payload.have_current_reverse_order = TRUE;
    blend_payload.current_reverse_order = current_reverse_order;
  }
  blend_payload.have_history = TRUE;
  blend_payload.history_before = history_before;
  blend_payload.history_after = history_after;
  _live_snapshot_add_module_blend(builder, &blend_payload);

  if((request != NULL && request->have_opacity && fabs(current_opacity - request->opacity) > 1e-6)
     || (request != NULL && request->have_blend_mode && current_blend_mode != request->blend_mode)
     || (request != NULL && request->have_reverse_order && current_reverse_order != request->reverse_order))
  {
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "module-blend-failed");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  JsonNode *snapshot_root = _live_json_copy_object_root(snapshot_json);
  if(snapshot_root == NULL)
  {
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "snapshot-unavailable");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  json_builder_set_member_name(builder, "snapshot");
  json_builder_add_value(builder, snapshot_root);
  json_builder_set_member_name(builder, "status");
  json_builder_add_string_value(builder, "ok");
  json_builder_end_object(builder);
  return _live_json_builder_to_string(builder);
}

static gboolean _live_module_masks_supported(dt_iop_module_t *module)
{
  return module != NULL && module->blend_params != NULL
         && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
         && !(module->flags() & IOP_FLAGS_NO_MASKS);
}

static gchar *_live_apply_module_instance_mask_to_json(dt_develop_t *dev,
                                                       const gchar *instance_key,
                                                       const dt_live_module_mask_request_t *request)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  dt_live_module_mask_payload_t mask_payload = {
    .instance_key = instance_key,
    .action = request != NULL && request->action == DT_LIVE_MODULE_MASK_ACTION_REUSE_SAME_SHAPES
                ? "reuse-same-shapes"
                : "clear-mask",
    .source_instance_key = request != NULL ? request->source_instance_key : NULL,
    .previous_forms = g_array_new(FALSE, FALSE, sizeof(dt_live_module_mask_form_t)),
    .source_forms = g_array_new(FALSE, FALSE, sizeof(dt_live_module_mask_form_t)),
    .current_forms = g_array_new(FALSE, FALSE, sizeof(dt_live_module_mask_form_t)),
  };

  if(dt_view_get_current() != DT_VIEW_DARKROOM)
  {
    _live_snapshot_add_module_mask(builder, &mask_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unsupported-view");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    gchar *result = _live_json_builder_to_string(builder);
    g_array_unref(mask_payload.previous_forms);
    g_array_unref(mask_payload.source_forms);
    g_array_unref(mask_payload.current_forms);
    return result;
  }

  if(dev == NULL || dev->image_storage.id == NO_IMGID)
  {
    _live_snapshot_add_module_mask(builder, &mask_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "no-active-image");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    gchar *result = _live_json_builder_to_string(builder);
    g_array_unref(mask_payload.previous_forms);
    g_array_unref(mask_payload.source_forms);
    g_array_unref(mask_payload.current_forms);
    return result;
  }

  _live_snapshot_add_active_image(builder, dev);

  dt_iop_module_t *module = _live_snapshot_find_visible_module(dev, instance_key);
  if(module == NULL)
  {
    _live_snapshot_add_module_mask(builder, &mask_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unknown-instance-key");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    gchar *result = _live_json_builder_to_string(builder);
    g_array_unref(mask_payload.previous_forms);
    g_array_unref(mask_payload.source_forms);
    g_array_unref(mask_payload.current_forms);
    return result;
  }

  mask_payload.module = module;
  const gint history_before = dev->history_end;

  if(!_live_module_masks_supported(module))
  {
    mask_payload.have_previous_has_mask = TRUE;
    mask_payload.previous_has_mask = FALSE;
    mask_payload.have_current_has_mask = TRUE;
    mask_payload.current_has_mask = FALSE;
    mask_payload.have_changed = TRUE;
    mask_payload.changed = FALSE;
    mask_payload.have_history = TRUE;
    mask_payload.history_before = history_before;
    mask_payload.history_after = history_before;
    _live_snapshot_add_module_mask(builder, &mask_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unsupported-module-mask");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    gchar *result = _live_json_builder_to_string(builder);
    g_array_unref(mask_payload.previous_forms);
    g_array_unref(mask_payload.source_forms);
    g_array_unref(mask_payload.current_forms);
    return result;
  }

  dt_masks_form_t *previous_group = _live_mask_group_from_module(dev, module);
  GArray *previous_forms = _live_mask_collect_forms(previous_group);
  g_array_unref(mask_payload.previous_forms);
  mask_payload.previous_forms = previous_forms;
  mask_payload.have_previous_has_mask = TRUE;
  mask_payload.previous_has_mask = _live_mask_forms_have_entries(previous_forms);

  dt_iop_module_t *source_module = NULL;
  GArray *source_forms = mask_payload.source_forms;
  if(request != NULL && request->action == DT_LIVE_MODULE_MASK_ACTION_REUSE_SAME_SHAPES)
  {
    source_module = _live_snapshot_find_visible_module(dev, request->source_instance_key);
    if(source_module == NULL)
    {
      g_array_unref(mask_payload.current_forms);
      mask_payload.current_forms = _live_mask_collect_forms(previous_group);
      mask_payload.have_current_has_mask = TRUE;
      mask_payload.current_has_mask = mask_payload.previous_has_mask;
      mask_payload.have_changed = TRUE;
      mask_payload.changed = FALSE;
      mask_payload.have_history = TRUE;
      mask_payload.history_before = history_before;
      mask_payload.history_after = history_before;
      _live_snapshot_add_module_mask(builder, &mask_payload);
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "unknown-source-instance-key");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      gchar *result = _live_json_builder_to_string(builder);
      g_array_unref(mask_payload.previous_forms);
      g_array_unref(mask_payload.source_forms);
      g_array_unref(mask_payload.current_forms);
      return result;
    }

    if(!_live_module_masks_supported(source_module))
    {
      g_array_unref(mask_payload.current_forms);
      mask_payload.current_forms = _live_mask_collect_forms(previous_group);
      mask_payload.have_current_has_mask = TRUE;
      mask_payload.current_has_mask = mask_payload.previous_has_mask;
      mask_payload.have_changed = TRUE;
      mask_payload.changed = FALSE;
      mask_payload.have_history = TRUE;
      mask_payload.history_before = history_before;
      mask_payload.history_after = history_before;
      _live_snapshot_add_module_mask(builder, &mask_payload);
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "source-module-mask-unavailable");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      gchar *result = _live_json_builder_to_string(builder);
      g_array_unref(mask_payload.previous_forms);
      g_array_unref(mask_payload.source_forms);
      g_array_unref(mask_payload.current_forms);
      return result;
    }

    dt_masks_form_t *source_group = _live_mask_group_from_module(dev, source_module);
    source_forms = _live_mask_collect_forms(source_group);
    g_array_unref(mask_payload.source_forms);
    mask_payload.source_forms = source_forms;

    if(!_live_mask_forms_have_entries(source_forms))
    {
      g_array_unref(mask_payload.current_forms);
      mask_payload.current_forms = _live_mask_collect_forms(previous_group);
      mask_payload.have_current_has_mask = TRUE;
      mask_payload.current_has_mask = mask_payload.previous_has_mask;
      mask_payload.have_changed = TRUE;
      mask_payload.changed = FALSE;
      mask_payload.have_history = TRUE;
      mask_payload.history_before = history_before;
      mask_payload.history_after = history_before;
      _live_snapshot_add_module_mask(builder, &mask_payload);
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "source-module-mask-unavailable");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      gchar *result = _live_json_builder_to_string(builder);
      g_array_unref(mask_payload.previous_forms);
      g_array_unref(mask_payload.source_forms);
      g_array_unref(mask_payload.current_forms);
      return result;
    }

    if(mask_payload.previous_has_mask && !_live_mask_forms_equal(previous_forms, source_forms))
    {
      g_array_unref(mask_payload.current_forms);
      mask_payload.current_forms = _live_mask_collect_forms(previous_group);
      mask_payload.have_current_has_mask = TRUE;
      mask_payload.current_has_mask = mask_payload.previous_has_mask;
      mask_payload.have_changed = TRUE;
      mask_payload.changed = FALSE;
      mask_payload.have_history = TRUE;
      mask_payload.history_before = history_before;
      mask_payload.history_after = history_before;
      _live_snapshot_add_module_mask(builder, &mask_payload);
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "target-module-mask-not-clear");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      gchar *result = _live_json_builder_to_string(builder);
      g_array_unref(mask_payload.previous_forms);
      g_array_unref(mask_payload.source_forms);
      g_array_unref(mask_payload.current_forms);
      return result;
    }
  }

  gboolean changed = FALSE;
  if(request != NULL && request->action == DT_LIVE_MODULE_MASK_ACTION_CLEAR_MASK)
  {
    if(mask_payload.previous_has_mask)
    {
      if(previous_group != NULL) dt_masks_form_remove(module, NULL, previous_group);
      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
      changed = TRUE;
    }
  }
  else if(request != NULL && request->action == DT_LIVE_MODULE_MASK_ACTION_REUSE_SAME_SHAPES)
  {
    if(!_live_mask_forms_equal(previous_forms, source_forms))
    {
      dt_masks_iop_use_same_as(module, source_module);
      dt_masks_iop_update(module);
      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      changed = TRUE;
    }
  }

  dt_masks_form_t *current_group = _live_mask_group_from_module(dev, module);
  GArray *current_forms = _live_mask_collect_forms(current_group);
  g_array_unref(mask_payload.current_forms);
  mask_payload.current_forms = current_forms;
  mask_payload.have_current_has_mask = TRUE;
  mask_payload.current_has_mask = _live_mask_forms_have_entries(current_forms);
  mask_payload.have_changed = TRUE;
  mask_payload.changed = changed;
  mask_payload.have_history = TRUE;
  mask_payload.history_before = history_before;
  mask_payload.history_after = dev->history_end;
  _live_snapshot_add_module_mask(builder, &mask_payload);

  if((request != NULL && request->action == DT_LIVE_MODULE_MASK_ACTION_CLEAR_MASK && mask_payload.current_has_mask)
     || (request != NULL && request->action == DT_LIVE_MODULE_MASK_ACTION_REUSE_SAME_SHAPES
         && !_live_mask_forms_equal(mask_payload.current_forms, mask_payload.source_forms)))
  {
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "module-mask-failed");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    gchar *result = _live_json_builder_to_string(builder);
    g_array_unref(mask_payload.previous_forms);
    g_array_unref(mask_payload.source_forms);
    g_array_unref(mask_payload.current_forms);
    return result;
  }

  g_autofree gchar *snapshot_json = _live_snapshot_to_json(dev);
  JsonNode *snapshot_root = _live_json_copy_object_root(snapshot_json);
  if(snapshot_root == NULL)
  {
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "snapshot-unavailable");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    gchar *result = _live_json_builder_to_string(builder);
    g_array_unref(mask_payload.previous_forms);
    g_array_unref(mask_payload.source_forms);
    g_array_unref(mask_payload.current_forms);
    return result;
  }

  json_builder_set_member_name(builder, "snapshot");
  json_builder_add_value(builder, snapshot_root);
  json_builder_set_member_name(builder, "status");
  json_builder_add_string_value(builder, "ok");
  json_builder_end_object(builder);
  gchar *result = _live_json_builder_to_string(builder);
  g_array_unref(mask_payload.previous_forms);
  g_array_unref(mask_payload.source_forms);
  g_array_unref(mask_payload.current_forms);
  return result;
}

static dt_live_module_reorder_check_t _live_module_instance_check_reorder(dt_develop_t *dev,
                                                                          dt_iop_module_t *module,
                                                                          dt_iop_module_t *anchor,
                                                                          const dt_live_module_instance_action_t action_kind)
{
  if(dev == NULL || module == NULL || anchor == NULL) return DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_RULE;
  if(module == anchor) return DT_LIVE_MODULE_REORDER_CHECK_NO_OP;
  if(module->flags() & IOP_FLAGS_FENCE) return DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_FENCE;

  gint module_index = -1;
  gint anchor_index = -1;
  gint index = 0;
  for(const GList *iter = dev->iop; iter; iter = g_list_next(iter), index++)
  {
    dt_iop_module_t *candidate = iter->data;
    if(candidate == module) module_index = index;
    if(candidate == anchor) anchor_index = index;
  }

  if(module_index < 0 || anchor_index < 0) return DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_RULE;

  gboolean moving_forward = FALSE;
  gint range_start = 0;
  gint range_end = -1;

  if(action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_BEFORE)
  {
    if(module_index + 1 == anchor_index) return DT_LIVE_MODULE_REORDER_CHECK_NO_OP;
    moving_forward = module_index < anchor_index;
    range_start = moving_forward ? module_index + 1 : anchor_index;
    range_end = moving_forward ? anchor_index - 1 : module_index - 1;
  }
  else if(action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_AFTER)
  {
    if(module_index == anchor_index + 1) return DT_LIVE_MODULE_REORDER_CHECK_NO_OP;
    moving_forward = module_index < anchor_index;
    range_start = moving_forward ? module_index + 1 : anchor_index + 1;
    range_end = moving_forward ? anchor_index : module_index - 1;
  }
  else
    return DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_RULE;

  index = 0;
  for(const GList *iter = dev->iop; iter; iter = g_list_next(iter), index++)
  {
    dt_iop_module_t *candidate = iter->data;
    if(index < range_start || index > range_end || candidate == NULL) continue;

    if(candidate->flags() & IOP_FLAGS_FENCE) return DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_FENCE;

    for(const GList *rules = darktable.iop_order_rules; rules; rules = g_list_next(rules))
    {
      const dt_iop_order_rule_t *rule = rules->data;
      if(rule == NULL) continue;

      if(moving_forward)
      {
        if(dt_iop_module_is(module->so, rule->op_prev)
           && dt_iop_module_is(candidate->so, rule->op_next))
          return DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_RULE;
      }
      else
      {
        if(dt_iop_module_is(candidate->so, rule->op_prev)
           && dt_iop_module_is(module->so, rule->op_next))
          return DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_RULE;
      }
    }
  }

  return DT_LIVE_MODULE_REORDER_CHECK_OK;
}

static guint _live_module_visible_family_count(const dt_develop_t *dev, const dt_iop_module_t *module)
{
  if(dev == NULL || module == NULL) return 0;

  guint count = 0;
  for(const GList *iter = dev->iop; iter; iter = g_list_next(iter))
  {
    const dt_iop_module_t *candidate = iter->data;
    if(candidate == NULL || dt_iop_is_hidden((dt_iop_module_t *)candidate)) continue;
    if(candidate->instance == module->instance) count++;
  }

  return count;
}

static dt_iop_module_t *_live_module_delete_sibling(const dt_iop_module_t *module)
{
  if(module == NULL || module->dev == NULL) return NULL;

  dt_iop_module_t *next = NULL;
  gboolean found = FALSE;
  for(const GList *modules = module->dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *candidate = modules->data;
    if(candidate == NULL || dt_iop_is_hidden(candidate)) continue;

    if(candidate == module)
    {
      found = TRUE;
      if(next != NULL) break;
    }
    else if(candidate->instance == module->instance)
    {
      next = candidate;
      if(found) break;
    }
  }

  return next;
}

static gboolean _live_delete_module_instance(dt_develop_t *dev,
                                             dt_iop_module_t *module,
                                             dt_iop_module_t **replacement_out)
{
  if(replacement_out != NULL) *replacement_out = NULL;
  if(dev == NULL || module == NULL) return FALSE;

  dt_iop_module_t *replacement = _live_module_delete_sibling(module);
  dt_iop_module_t *replacement_for_response = NULL;
  if(replacement == NULL) return FALSE;

  if(dev->gui_attached)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE);

  if(darktable.develop->gui_module == module)
    dt_iop_request_focus(NULL);

  const gboolean is_zero = (module->multi_priority == 0);

  ++darktable.gui->reset;

  if(!dt_iop_is_hidden(module))
    dt_iop_gui_cleanup_module(module);

  dt_dev_module_remove(dev, module);

  if(is_zero)
  {
    dt_iop_module_t *first = NULL;
    for(GList *history = dev->history; history; history = g_list_next(history))
    {
      dt_dev_history_item_t *hist = history->data;
      if(hist->module != NULL && !dt_iop_is_hidden(hist->module)
         && hist->module->instance == module->instance && hist->module != module)
      {
        first = hist->module;
        break;
      }
    }
    if(first == NULL) first = replacement;

    if(first != NULL)
    {
      dt_iop_update_multi_priority(first, 0);
      for(GList *history = dev->history; history; history = g_list_next(history))
      {
        dt_dev_history_item_t *hist = history->data;
        if(hist->module == first) hist->multi_priority = 0;
      }
      replacement = first;
      replacement_for_response = first;
    }
  }

  if(dev->gui_attached)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);

  dt_iop_connect_accels_multi(module->so);
  dt_action_cleanup_instance_iop(module);
  dev->alliop = g_list_append(dev->alliop, module);
  dt_dev_pixelpipe_rebuild(dev);
  dt_control_queue_redraw_center();

  --darktable.gui->reset;

  if(replacement_out != NULL) *replacement_out = replacement_for_response;
  return TRUE;
}

static const gchar *_live_module_instance_reorder_reason(const dt_live_module_reorder_check_t check)
{
  switch(check)
  {
    case DT_LIVE_MODULE_REORDER_CHECK_NO_OP:
      return "module-reorder-no-op";
    case DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_FENCE:
      return "module-reorder-blocked-by-fence";
    case DT_LIVE_MODULE_REORDER_CHECK_BLOCKED_BY_RULE:
      return "module-reorder-blocked-by-rule";
    case DT_LIVE_MODULE_REORDER_CHECK_OK:
    default:
      return NULL;
  }
}

static dt_iop_module_t *_live_create_module_instance(dt_iop_module_t *base, const gboolean copy_params)
{
  if(base == NULL) return NULL;

  dt_dev_add_history_item(base->dev, base, FALSE);

  if(darktable.gui != NULL) ++darktable.gui->reset;
  dt_iop_module_t *module = dt_dev_module_duplicate(base->dev, base);
  if(darktable.gui != NULL) --darktable.gui->reset;
  if(module == NULL) return NULL;

  if(!dt_iop_is_hidden(module))
  {
    dt_iop_gui_init(module);
    dt_iop_gui_set_expander(module);
    dt_iop_gui_set_expanded(module, FALSE, FALSE);

    if(base->expander != NULL && module->expander != NULL)
    {
      GList *modules = module->dev->iop;
      int pos_module = 0;
      int pos_base = 0;
      int pos = 0;

      while(modules)
      {
        dt_iop_module_t *current = modules->data;
        if(current == module)
          pos_module = pos;
        else if(current == base)
          pos_base = pos;
        modules = g_list_next(modules);
        pos++;
      }

      GValue position = G_VALUE_INIT;
      g_value_init(&position, G_TYPE_INT);
      gtk_container_child_get_property(
          GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
          base->expander, "position", &position);
      gtk_box_reorder_child(
          dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
          module->expander, g_value_get_int(&position) + pos_base - pos_module + 1);
      g_value_unset(&position);
    }

    dt_iop_reload_defaults(module);

    if(copy_params)
    {
      memcpy(module->params, base->params, module->params_size);
      if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
      {
        dt_iop_commit_blend_params(module, base->blend_params);
        if(dt_is_valid_maskid(base->blend_params->mask_id))
        {
          module->blend_params->mask_id = NO_MASKID;
          dt_masks_iop_use_same_as(module, base);
        }
      }
    }

    dt_dev_add_history_item(module->dev, module, TRUE);
    dt_iop_gui_update_blending(module);
    dt_iop_gui_update(module);
  }

  dt_iop_connect_accels_multi(base->so);
  if(module->dev->gui_attached)
  {
    dt_dev_pixelpipe_rebuild(module->dev);
  }
  dt_dev_modulegroups_update_visibility(module->dev);

  return module;
}

static gchar *_live_apply_module_instance_action_to_json(dt_develop_t *dev,
                                                         const gchar *instance_key,
                                                         const gchar *action,
                                                         const gchar *anchor_instance_key)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  gboolean requested_enabled = FALSE;
  const dt_live_module_instance_action_t action_kind =
    _live_module_instance_action_from_string(action, &requested_enabled);
  const gboolean supported_action = action_kind != DT_LIVE_MODULE_INSTANCE_ACTION_INVALID;
  const gboolean requires_anchor = _live_module_instance_action_requires_anchor(action_kind);
  dt_live_module_action_payload_t action_payload = {
    .instance_key = instance_key,
    .action = action,
    .anchor_instance_key = requires_anchor ? anchor_instance_key : NULL,
    .have_requested_enabled = action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_ENABLE
                             || action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_DISABLE,
    .requested_enabled = requested_enabled,
  };

  if(dt_view_get_current() != DT_VIEW_DARKROOM)
  {
    _live_snapshot_add_module_action(builder, &action_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unsupported-view");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  if(dev == NULL || dev->image_storage.id == NO_IMGID)
  {
    _live_snapshot_add_module_action(builder, &action_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "no-active-image");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  _live_snapshot_add_active_image(builder, dev);

  if(!supported_action)
  {
    _live_snapshot_add_module_action(builder, &action_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unsupported-module-action");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  dt_iop_module_t *module = _live_snapshot_find_visible_module(dev, instance_key);
  if(module == NULL)
  {
    _live_snapshot_add_module_action(builder, &action_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unknown-instance-key");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  const gboolean previous_enabled = module->enabled;
  const gint previous_iop_order = module->iop_order;
  const gint history_before = dev->history_end;
  action_payload.module = module;

  if(action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_CREATE
      || action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_DUPLICATE)
  {
    if(module->flags() & IOP_FLAGS_ONE_INSTANCE)
    {
      action_payload.have_history = TRUE;
      action_payload.history_before = history_before;
      action_payload.history_after = history_before;
      _live_snapshot_add_module_action(builder, &action_payload);
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "unsupported-module-state");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    dt_iop_module_t *result_module =
      _live_create_module_instance(module, action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_DUPLICATE);

    const gint history_after = dev->history_end;
    g_autofree gchar *snapshot_json = _live_snapshot_to_json(dev);

    action_payload.module = result_module;
    action_payload.result_module = result_module;
    action_payload.have_history = TRUE;
    action_payload.history_before = history_before;
    action_payload.history_after = history_after;
    _live_snapshot_add_module_action(builder, &action_payload);

    if(result_module == NULL)
    {
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "module-action-failed");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    JsonNode *snapshot_root = _live_json_copy_object_root(snapshot_json);
    if(snapshot_root == NULL)
    {
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "snapshot-unavailable");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    json_builder_set_member_name(builder, "snapshot");
    json_builder_add_value(builder, snapshot_root);
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "ok");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  if(action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_DELETE)
  {
    action_payload.have_history = TRUE;
    action_payload.history_before = history_before;
    action_payload.history_after = history_before;

    if(_live_module_visible_family_count(dev, module) <= 1)
    {
      _live_snapshot_add_module_action(builder, &action_payload);
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "module-delete-blocked-last-instance");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    dt_iop_module_t *replacement_module = NULL;
    const gboolean deleted = _live_delete_module_instance(dev, module, &replacement_module);
    const gint history_after = dev->history_end;
    g_autofree gchar *snapshot_json = deleted ? _live_snapshot_to_json(dev) : NULL;

    action_payload.replacement_module = replacement_module;
    action_payload.have_history = TRUE;
    action_payload.history_before = history_before;
    action_payload.history_after = history_after;
    _live_snapshot_add_module_action(builder, &action_payload);

    if(!deleted)
    {
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "module-action-failed");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    JsonNode *snapshot_root = _live_json_copy_object_root(snapshot_json);
    if(snapshot_root == NULL)
    {
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "snapshot-unavailable");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    json_builder_set_member_name(builder, "snapshot");
    json_builder_add_value(builder, snapshot_root);
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "ok");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  if(requires_anchor)
  {
    dt_iop_module_t *anchor_module = _live_snapshot_find_visible_module(dev, anchor_instance_key);
    action_payload.have_previous_iop_order = TRUE;
    action_payload.previous_iop_order = previous_iop_order;
    action_payload.have_current_iop_order = TRUE;
    action_payload.current_iop_order = previous_iop_order;
    action_payload.have_history = TRUE;
    action_payload.history_before = history_before;
    action_payload.history_after = history_before;

    if(anchor_module == NULL)
    {
      _live_snapshot_add_module_action(builder, &action_payload);
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "unknown-anchor-instance-key");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    const dt_live_module_reorder_check_t reorder_check =
      _live_module_instance_check_reorder(dev, module, anchor_module, action_kind);
    const gchar *reorder_reason = _live_module_instance_reorder_reason(reorder_check);
    if(reorder_reason != NULL)
    {
      _live_snapshot_add_module_action(builder, &action_payload);
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, reorder_reason);
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    const gboolean moved = (action_kind == DT_LIVE_MODULE_INSTANCE_ACTION_MOVE_BEFORE)
                             ? dt_ioppr_move_iop_before(dev, module, anchor_module)
                             : dt_ioppr_move_iop_after(dev, module, anchor_module);
    if(moved)
    {
      dt_dev_reorder_gui_module_list(dev);
      dt_dev_add_history_item(dev, module, TRUE);
      dt_iop_connect_accels_multi(module->so);
      if(dev->gui_attached) dt_dev_pixelpipe_rebuild(dev);
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MODULE_MOVED);
    }

    const gint history_after = dev->history_end;
    const gint current_iop_order = module->iop_order;
    g_autofree gchar *snapshot_json = moved ? _live_snapshot_to_json(dev) : NULL;

    action_payload.have_current_iop_order = TRUE;
    action_payload.current_iop_order = current_iop_order;
    action_payload.have_history = TRUE;
    action_payload.history_before = history_before;
    action_payload.history_after = history_after;
    _live_snapshot_add_module_action(builder, &action_payload);

    if(!moved)
    {
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "module-action-failed");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    JsonNode *snapshot_root = _live_json_copy_object_root(snapshot_json);
    if(snapshot_root == NULL)
    {
      json_builder_set_member_name(builder, "reason");
      json_builder_add_string_value(builder, "snapshot-unavailable");
      json_builder_set_member_name(builder, "status");
      json_builder_add_string_value(builder, "unavailable");
      json_builder_end_object(builder);
      return _live_json_builder_to_string(builder);
    }

    json_builder_set_member_name(builder, "snapshot");
    json_builder_add_value(builder, snapshot_root);
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "ok");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  if(module->hide_enable_button || module->off == NULL)
  {
    action_payload.have_requested_enabled = TRUE;
    action_payload.requested_enabled = requested_enabled;
    action_payload.have_previous_enabled = TRUE;
    action_payload.previous_enabled = previous_enabled;
    action_payload.have_current_enabled = TRUE;
    action_payload.current_enabled = module->enabled;
    action_payload.have_history = TRUE;
    action_payload.history_before = history_before;
    action_payload.history_after = history_before;
    _live_snapshot_add_module_action(builder, &action_payload);
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "unsupported-module-state");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  if(previous_enabled != requested_enabled)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), requested_enabled);

  const gboolean current_enabled = module->enabled;
  const gint history_after = dev->history_end;
  g_autofree gchar *snapshot_json = _live_snapshot_to_json(dev);

  action_payload.have_requested_enabled = TRUE;
  action_payload.requested_enabled = requested_enabled;
  action_payload.have_previous_enabled = TRUE;
  action_payload.previous_enabled = previous_enabled;
  action_payload.have_current_enabled = TRUE;
  action_payload.current_enabled = current_enabled;
  action_payload.have_history = TRUE;
  action_payload.history_before = history_before;
  action_payload.history_after = history_after;
  _live_snapshot_add_module_action(builder, &action_payload);

  if(current_enabled != requested_enabled)
  {
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "module-action-failed");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  JsonNode *snapshot_root = _live_json_copy_object_root(snapshot_json);
  if(snapshot_root == NULL)
  {
    json_builder_set_member_name(builder, "reason");
    json_builder_add_string_value(builder, "snapshot-unavailable");
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, "unavailable");
    json_builder_end_object(builder);
    return _live_json_builder_to_string(builder);
  }

  json_builder_set_member_name(builder, "snapshot");
  json_builder_add_value(builder, snapshot_root);
  json_builder_set_member_name(builder, "status");
  json_builder_add_string_value(builder, "ok");
  json_builder_end_object(builder);
  return _live_json_builder_to_string(builder);
}

static int display_image_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  dt_lua_image_t imgid = NO_IMGID;
  if(luaL_testudata(L, 1, "dt_lua_image_t"))
  {
    luaA_to(L, dt_lua_image_t, &imgid, 1);
    _dev_change_image(dev, imgid);
  }
  else
  {
    // ensure the image infos in db is up to date
    dt_dev_write_history(dev);
  }
  luaA_push(L, dt_lua_image_t, &dev->image_storage.id);
  return 1;
}

static int live_snapshot_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  g_autofree gchar *snapshot_json = _live_snapshot_to_json(dev);
  lua_pushstring(L, snapshot_json ? snapshot_json : "{}");
  return 1;
}

static int live_apply_module_instance_action_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  const gchar *instance_key = luaL_checkstring(L, 1);
  const gchar *action = luaL_checkstring(L, 2);
  const gchar *anchor_instance_key = lua_gettop(L) >= 3 && !lua_isnil(L, 3) ? luaL_checkstring(L, 3) : NULL;
  g_autofree gchar *response_json =
    _live_apply_module_instance_action_to_json(dev, instance_key, action, anchor_instance_key);
  lua_pushstring(L, response_json ? response_json : "{}");
  return 1;
}

static int live_apply_module_instance_blend_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  const gchar *instance_key = luaL_checkstring(L, 1);
  const gchar *blend_json = luaL_checkstring(L, 2);
  dt_live_module_blend_request_t request = { 0 };
  if(!_live_parse_module_instance_blend_request(blend_json, &request))
  {
    return luaL_error(L, "invalid blend request");
  }

  g_autofree gchar *response_json =
    _live_apply_module_instance_blend_to_json(dev, instance_key, &request);
  lua_pushstring(L, response_json ? response_json : "{}");
  return 1;
}

static int live_apply_module_instance_mask_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  const gchar *instance_key = luaL_checkstring(L, 1);
  const gchar *mask_json = luaL_checkstring(L, 2);
  dt_live_module_mask_request_t request = { 0 };
  if(!_live_parse_module_instance_mask_request(mask_json, &request))
  {
    return luaL_error(L, "invalid mask request");
  }

  g_autofree gchar *source_instance_key = request.source_instance_key;
  g_autofree gchar *response_json =
    _live_apply_module_instance_mask_to_json(dev, instance_key, &request);
  lua_pushstring(L, response_json ? response_json : "{}");
  return 1;
}

#endif

// helpers to let us get the pointer's zoom position only when actually
// used, while only calling the underlying function once per GUI
// callback if it happens to be used more than once
// the variable passed as zoom_x/zbound_x must be initialized to FLT_MAX as
// a signal that the zoom values are not yet valid and need to be computed
static void _get_zoom_pos(dt_dev_viewport_t *port,
                          const double x,
                          const double y,
                          float *zoom_x,
                          float *zoom_y,
                          float *zoom_scale)
{
  if(*zoom_x == FLT_MAX)
  {
    dt_dev_get_pointer_zoom_pos(port, x, y, zoom_x, zoom_y, zoom_scale);
  }
}

static void _get_zoom_pos_bnd(dt_dev_viewport_t *port,
                              const double x,
                              const double y,
                              float zbound_x,
                              float zbound_y,
                              float *zoom_x,
                              float *zoom_y,
                              float *zoom_scale)
{
  if(zbound_x == FLT_MAX)
  {
    dt_dev_get_pointer_zoom_pos(port, x, y, zoom_x, zoom_y, zoom_scale);
  }
  else
  {
    dt_dev_get_pointer_zoom_pos_from_bounds(port, x, y, zbound_x, zbound_y, zoom_x, zoom_y, zoom_scale);
  }
}

void init(dt_view_t *self)
{
  self->data = darktable.develop;
  darktable.view_manager->proxy.darkroom.view = self;

#ifdef USE_LUA
  lua_State *L = darktable.lua_state.state;
  const int my_type = dt_lua_module_entry_get_type(L, "view", self->module_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, display_image_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "display_image");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, live_snapshot_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "live_snapshot");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, live_apply_module_instance_action_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "live_apply_module_instance_action");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, live_apply_module_instance_blend_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "live_apply_module_instance_blend");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, live_apply_module_instance_mask_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "live_apply_module_instance_mask");
#endif
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_DARKROOM;
}

void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = self->data;

  // unref the grid lines popover if needed
  if(darktable.view_manager->guides_popover) g_object_unref(darktable.view_manager->guides_popover);

  if(dev->second_wnd)
  {
    GtkWidget *wnd = dev->second_wnd;
    
    if(gtk_widget_is_visible(wnd))
    {
      dt_conf_set_bool("second_window/last_visible", TRUE);
      _darkroom_ui_second_window_write_config(wnd);
    }
    else
      dt_conf_set_bool("second_window/last_visible", FALSE);

    _darkroom_ui_second_window_cleanup(dev);
    gtk_widget_hide(wnd);
    gtk_widget_destroy(wnd);
  }
  else
  {
    dt_conf_set_bool("second_window/last_visible", FALSE);
  }

  dt_dev_cleanup(dev);
  free(dev);
}

static dt_darkroom_layout_t _lib_darkroom_get_layout(dt_view_t *self)
{
  return DT_DARKROOM_LAYOUT_EDITING;
}

void _display_module_trouble_message_callback(gpointer instance,
                                              dt_iop_module_t *module,
                                              const char *const trouble_msg,
                                              const char *const trouble_tooltip)
{
  GtkWidget *label_widget = NULL;

  if(module && module->has_trouble && module->widget)
  {
    label_widget = dt_gui_container_first_child(GTK_CONTAINER(gtk_widget_get_parent(module->widget)));
    if(g_strcmp0(gtk_widget_get_name(label_widget), "iop-plugin-warning"))
      label_widget = NULL;
  }

  if(trouble_msg && *trouble_msg)
  {
    if(module && module->widget)
    {
      if(label_widget)
      {
        // set the warning message in the module's message area just below the header
        gtk_label_set_text(GTK_LABEL(label_widget), trouble_msg);
      }
      else
      {
        label_widget = gtk_label_new(trouble_msg);;
        gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
        gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0);
        gtk_widget_set_name(label_widget, "iop-plugin-warning");
        dt_gui_add_class(label_widget, "dt_warning");

        GtkWidget *iopw = gtk_widget_get_parent(module->widget);
        gtk_box_pack_start(GTK_BOX(iopw), label_widget, TRUE, TRUE, 0);
        gtk_box_reorder_child(GTK_BOX(iopw), label_widget, 0);
        gtk_widget_show(label_widget);
      }

      gtk_widget_set_tooltip_text(GTK_WIDGET(label_widget), trouble_tooltip);

      // set the module's trouble flag
      module->has_trouble = TRUE;

      dt_iop_gui_update_header(module);
    }
  }
  else if(module && module->has_trouble)
  {
    // no more trouble, so clear the trouble flag and remove the message area
    module->has_trouble = FALSE;

    dt_iop_gui_update_header(module);

    if(label_widget) gtk_widget_destroy(label_widget);
  }
}


static void _darkroom_pickers_draw(dt_view_t *self,
                                   cairo_t *cri,
                                   const float wd,
                                   const float ht,
                                   const float zoom_scale,
                                   GSList *samples,
                                   const gboolean is_primary_sample)
{
  if(!samples) return;

  dt_develop_t *dev = self->data;

  cairo_save(cri);
  const double lw = 1.0 / zoom_scale;
  const double dashes[1] = { lw * 4.0 };

  // makes point sample crosshair gap look nicer
  cairo_set_line_cap(cri, CAIRO_LINE_CAP_SQUARE);

  dt_colorpicker_sample_t *selected_sample = darktable.lib->proxy.colorpicker.selected_sample;
  const gboolean only_selected_sample = !is_primary_sample && selected_sample
    && !darktable.lib->proxy.colorpicker.display_samples;

  for( ; samples; samples = g_slist_next(samples))
  {
    dt_colorpicker_sample_t *sample = samples->data;
    if(only_selected_sample && (sample != selected_sample))
      continue;

    // The picker is at the resolution of the preview pixelpipe. This
    // is width/2 of a preview-pipe pixel in (scaled) user space
    // coordinates. Use half pixel width so rounding to nearest device
    // pixel doesn't make uneven centering.
    double half_px = 0.5;
    const double min_half_px_device = 4.0;
    // FIXME: instead of going to all this effort to show how error-prone a preview pipe sample can be, just produce a better point sample
    gboolean show_preview_pixel_scale = TRUE;

    double x = 0.0;
    double y = 0.0;
    // overlays are aligned with pixels for a clean look
    if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      dt_boundingbox_t fbox;
      dt_color_picker_transform_box(dev, 2, sample->box, fbox, FALSE);
      x = fbox[0];
      y = fbox[1];
      double w = fbox[2];
      double h = fbox[3];
      cairo_user_to_device(cri, &x, &y);
      cairo_user_to_device(cri, &w, &h);
      x=round(x+0.5)-0.5;
      y=round(y+0.5)-0.5;
      w=round(w+0.5)-0.5;
      h=round(h+0.5)-0.5;
      cairo_device_to_user(cri, &x, &y);
      cairo_device_to_user(cri, &w, &h);
      cairo_rectangle(cri, x, y, w - x, h - y);
      if(is_primary_sample)
      {
        // handles
        const double hw = 5. / zoom_scale;
        cairo_rectangle(cri, x - hw, y - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, x - hw, h - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, w - hw, y - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, w - hw, h - hw, 2. * hw, 2. * hw);
      }
    }
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      /* FIXME: to be really accurate, the colorpicker should render precisely over the nearest pixelpipe pixel
         but this gets particularly tricky to do with iop pickers with transformations after them in the pipeline
      */
      dt_boundingbox_t fbox;
      dt_color_picker_transform_box(dev, 1, sample->point, fbox, FALSE);
      x = fbox[0];
      y = fbox[1];
      cairo_user_to_device(cri, &x, &y);
      x=round(x+0.5)-0.5;
      y=round(y+0.5)-0.5;
      // render picker center a reasonable size in device pixels
      half_px = round(half_px * zoom_scale);
      if(half_px < min_half_px_device)
      {
        half_px = min_half_px_device;
        show_preview_pixel_scale = FALSE;
      }
      // crosshair radius
      double cr = (is_primary_sample ? 4. : 5.) * half_px;
      if(sample == selected_sample) cr *= 2;
      cairo_device_to_user(cri, &x, &y);
      cairo_device_to_user_distance(cri, &cr, &half_px);

      // "handles"
      if(is_primary_sample)
        cairo_arc(cri, x, y, cr, 0., 2. * M_PI);
      // crosshair
      cairo_move_to(cri, x - cr, y);
      cairo_line_to(cri, x + cr, y);
      cairo_move_to(cri, x, y - cr);
      cairo_line_to(cri, x, y + cr);
    }

    // default is to draw 1 (logical) pixel light lines with 1
    // (logical) pixel dark outline for legibility
    const double line_scale = (sample == selected_sample ? 2.0 : 1.0);
    cairo_set_line_width(cri, lw * 3.0 * line_scale);
    cairo_set_source_rgba(cri, 0.0, 0.0, 0.0, 0.4);
    cairo_stroke_preserve(cri);

    cairo_set_line_width(cri, lw * line_scale);
    cairo_set_dash(cri, dashes,
                   !is_primary_sample
                   && sample != selected_sample
                   && sample->size == DT_LIB_COLORPICKER_SIZE_BOX,
                   0.0);
    cairo_set_source_rgba(cri, 1.0, 1.0, 1.0, 0.8);
    cairo_stroke(cri);

    // draw the actual color sampled
    // FIXME: if an area sample is selected, when selected should fill it with colorpicker color?
    // NOTE: The sample may be based on outdated data, but still
    // display as it will update eventually. If we only drew on valid
    // data, swatches on point live samples would flicker when the
    // primary sample was drawn, and the primary sample swatch would
    // flicker when an iop is adjusted.
    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      if(sample == selected_sample)
        cairo_arc(cri, x, y, half_px * 2., 0., 2. * M_PI);
      else if(show_preview_pixel_scale)
        cairo_rectangle(cri, x - half_px, y, half_px * 2., half_px * 2.);
      else
        cairo_arc(cri, x, y, half_px, 0., 2. * M_PI);

      set_color(cri, sample->swatch);
      cairo_fill(cri);
    }
  }

  cairo_restore(cri);
}

static inline gboolean _full_request(dt_develop_t *dev)
{
  return
        dev->full.pipe->status == DT_DEV_PIXELPIPE_DIRTY
     || dev->full.pipe->status == DT_DEV_PIXELPIPE_INVALID
     || dev->full.pipe->input_timestamp < dev->preview_pipe->input_timestamp;
}

static inline gboolean _preview_request(dt_develop_t *dev)
{
  return
        dev->preview_pipe->status == DT_DEV_PIXELPIPE_DIRTY
     || dev->preview_pipe->status == DT_DEV_PIXELPIPE_INVALID
     || dev->full.pipe->input_timestamp > dev->preview_pipe->input_timestamp;
}

static inline gboolean _preview2_request(dt_develop_t *dev)
{
  return
     (dev->preview2.pipe->status == DT_DEV_PIXELPIPE_DIRTY
       || dev->preview2.pipe->status == DT_DEV_PIXELPIPE_INVALID
       || dev->full.pipe->input_timestamp > dev->preview2.pipe->input_timestamp)
     && dev->gui_attached
     && dev->preview2.widget
     && GTK_IS_WIDGET(dev->preview2.widget);
}

static void _module_gui_post_expose(dt_iop_module_t *module,
                                    cairo_t *cri,
                                    const float width,
                                    const float height,
                                    const float x,
                                    const float y,
                                    const float zoom_scale)
{
  if(!module || !module->gui_post_expose || width < 1.0f || height < 1.0f) return;

  cairo_save(cri);
  module->gui_post_expose(module, cri, width, height, x, y, zoom_scale);
  cairo_restore(cri);
}

static void _view_paint_surface(cairo_t *cr,
                                const size_t width,
                                const size_t height,
                                dt_dev_viewport_t *port,
                                const dt_window_t window)
{
  dt_dev_pixelpipe_t *p = port->pipe;

  dt_pthread_mutex_lock(&p->backbuf_mutex);

  dt_view_paint_surface(cr, width, height,
                        port, window,
                        p->backbuf, p->backbuf_scale,
                        p->backbuf_width, p->backbuf_height,
                        p->backbuf_zoom_pos);

  dt_pthread_mutex_unlock(&p->backbuf_mutex);
}

void expose(dt_view_t *self,
            cairo_t *cri,
            const int32_t width,
            const int32_t height,
            int32_t pointerx,
            int32_t pointery)
{
  cairo_set_source_rgb(cri, .2, .2, .2);

  dt_develop_t *dev = self->data;
  dt_dev_viewport_t *port = &dev->full;

  if(dev->gui_synch && !port->pipe->loading)
  {
    // synch module guis from gtk thread:
    ++darktable.gui->reset;
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = modules->data;
      dt_iop_gui_update(module);
    }
    --darktable.gui->reset;
    dev->gui_synch = FALSE;
  }

  // adjust scroll bars
  float zoom_x, zoom_y, boxw, boxh;
  float zbound_x = FLT_MAX, zbound_y = 0.0f;
  if(!dt_dev_get_zoom_bounds(port, &zoom_x, &zoom_y, &boxw, &boxh))
    boxw = boxh = 1.0f;
  else
  {
    zbound_x = zoom_x;
    zbound_y = zoom_y;
  }
  /* If boxw and boxh very closely match the zoomed size in the
      darktable window we might have resizing with every expose
      because adding a slider will change the image area and might
      force a resizing in next expose.  So we disable in cases close
      to full.
  */
  if(boxw > 0.95f)
  {
    zoom_x = .0f;
    boxw = 1.01f;
  }
  if(boxh > 0.95f)
  {
    zoom_y = .0f;
    boxh = 1.01f;
  }

  dt_view_set_scrollbar(self, zoom_x, -0.5 + boxw/2, 0.5,
                        boxw/2, zoom_y, -0.5+ boxh/2, 0.5, boxh/2);

  const gboolean expose_full =
        port->pipe->backbuf                                // do we have an image?
     && port->pipe->output_imgid == dev->image_storage.id; // same image?

  if(expose_full)
  {
    // draw image
    _view_paint_surface(cri, width, height, port, DT_WINDOW_MAIN);
    // clean up cached rendering; do this unconditionally in case user toggles the preference
    if(darktable.gui->surface)
    {
      cairo_surface_destroy(darktable.gui->surface);
      darktable.gui->surface = NULL;
    }
    if(!dt_conf_get_bool("darkroom/ui/loading_screen"))
    {
      // cache the rendered bitmap for use while loading the next image
      darktable.gui->surface = cairo_get_target(cri);
      cairo_surface_reference(darktable.gui->surface);
    }
  }
  else if(dev->preview_pipe->output_imgid != dev->image_storage.id)
  {
    gchar *load_txt;
    float fontsize;
    dt_image_t *img = dt_image_cache_get(dev->image_storage.id, 'r');;
    dt_imageio_retval_t status = img->load_status;
    dt_image_cache_read_release(img);

    if(dev->image_invalid_cnt)
    {
      fontsize = DT_PIXEL_APPLY_DPI(16);
      switch(status)
      {
      case DT_IMAGEIO_FILE_NOT_FOUND:
        load_txt = g_strdup_printf(
          _("file `%s' is not available, switching to lighttable now.\n\n"
            "if stored on an external drive, ensure that the drive is connected and files\n"
            "can be accessed in the same locations as when you imported this image."),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_FILE_CORRUPTED:
        load_txt = g_strdup_printf(
          _("file `%s' appears corrupt, switching to lighttable now.\n\n"
            "please check that it was correctly and completely copied from the camera."),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_UNSUPPORTED_FORMAT:
        load_txt = g_strdup_printf(
          _("file `%s' is not in any recognized format, switching to lighttable now."),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_UNSUPPORTED_CAMERA:
        load_txt = g_strdup_printf(
          _("file `%s' is from an unsupported camera model, switching to lighttable now."),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_UNSUPPORTED_FEATURE:
        load_txt = g_strdup_printf(
          _("file `%s' uses an unsupported feature, switching to lighttable now.\n\n"
            "please check that the image format and compression mode you selected in your\n"
            "camera's menus is supported (see https://www.darktable.org/resources/camera-support/\n"
            "and the release notes for this version of darktable)"),
          dev->image_storage.filename);
        break;
      case DT_IMAGEIO_IOERROR:
        load_txt = g_strdup_printf(
          _("error while reading file `%s', switching to lighttable now.\n\n"
            "please check that the file has not been truncated."),
          dev->image_storage.filename);
        break;
      default:
        load_txt = g_strdup_printf(
          _("darktable could not load `%s', switching to lighttable now.\n\n"
            "please check that the camera model that produced the image is supported in darktable\n"
            "(list of supported cameras is at https://www.darktable.org/resources/camera-support/).\n"
            "if you are sure that the camera model is supported, please consider opening an issue\n"
            "at https://github.com/darktable-org/darktable"),
          dev->image_storage.filename);
        break;
      }
      // if we already saw an error, retry a FEW more times with a bit
      // of delay in between it would be better if we could just put
      // the delay after the first occurrence, but that resulted in
      // the error message not showing
      if(dev->image_invalid_cnt > 1)
      {
        g_usleep(1000000); // one second
        if(dev->image_invalid_cnt > 8)
        {
          dev->image_invalid_cnt = 0;
          dt_view_manager_switch(darktable.view_manager, "lighttable");
          g_free(load_txt);
          return;
        }
      }
    }
    else
    {
      fontsize = DT_PIXEL_APPLY_DPI(14);
      if(dt_conf_get_bool("darkroom/ui/loading_screen"))
        load_txt = g_strdup_printf(C_("darkroom", "loading `%s' ..."),
                                   dev->image_storage.filename);
      else
        load_txt = g_strdup(dev->image_storage.filename);
    }

    if(dt_conf_get_bool("darkroom/ui/loading_screen"))
    {
      dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_DARKROOM_BG);
      cairo_paint(cri);

      // waiting message
      PangoRectangle ink;
      PangoLayout *layout;
      PangoFontDescription *desc =
        pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      layout = pango_cairo_create_layout(cri);
      pango_layout_set_font_description(layout, desc);
      pango_layout_set_text(layout, load_txt, -1);
      pango_layout_get_pixel_extents(layout, &ink, NULL);
      const double xc = width / 2.0;
      const double yc = height * 0.88 - DT_PIXEL_APPLY_DPI(10);
      const double wd = ink.width * 0.5;
      cairo_move_to(cri, xc - wd, yc + 1.0 / 3.0 * fontsize - fontsize);
      pango_cairo_layout_path(cri, layout);
      cairo_set_line_width(cri, 2.0);
      dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_LOG_BG);
      cairo_stroke_preserve(cri);
      dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_LOG_FG);
      cairo_fill(cri);
      pango_font_description_free(desc);
      g_object_unref(layout);
    }
    else
    {
      // repaint the image we are switching away from, to avoid a
      // flash of the background color
      if(darktable.gui->surface)
      {
        cairo_save(cri);
        cairo_identity_matrix(cri);
        cairo_set_source_surface(cri, darktable.gui->surface, 0, 0);
        cairo_paint(cri);
        cairo_restore(cri);
      }
      dt_toast_log("%s", load_txt);
    }
    g_free(load_txt);
  }

  if(_full_request(dev)) dt_dev_process_image(dev);
  if(_preview_request(dev)) dt_dev_process_preview(dev);
  if(_preview2_request(dev)) dt_dev_process_preview2(dev);

  /* if we are in full preview mode, we don"t want anything else than the image */
  if(dev->full_preview || !darktable.develop->preview_pipe->processed_width)
    return;

  float wd, ht;
  if(!dt_dev_get_preview_size(dev, &wd, &ht)) return;

  const double tb = port->border_size;
  // account for border, make it transparent for other modules called below:

  cairo_save(cri);

  float pzx = FLT_MAX, pzy = 0.0f;
  float zoom_scale = dt_dev_get_zoom_scale(&dev->full, port->zoom, 1 << port->closeup, TRUE);

  // don't draw guides and color pickers on image margins
  cairo_rectangle(cri, tb, tb, width - 2.0 * tb, height - 2.0 * tb);

  cairo_translate(cri, 0.5 * width, 0.5 * height);
  cairo_scale(cri, zoom_scale, zoom_scale);
  cairo_translate(cri, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  cairo_save(cri);
  cairo_clip(cri);

  // Displaying sample areas if enabled
  if(darktable.lib->proxy.colorpicker.live_samples
     && (darktable.lib->proxy.colorpicker.display_samples
         || (darktable.lib->proxy.colorpicker.selected_sample &&
             darktable.lib->proxy.colorpicker.selected_sample != darktable.lib->proxy.colorpicker.primary_sample)))
  {
    dt_print_pipe(DT_DEBUG_EXPOSE,
        "expose livesamples",
         port->pipe, NULL, DT_DEVICE_NONE, NULL, NULL, "%dx%d, px=%d py=%d",
         width, height, pointerx, pointery);
    _darkroom_pickers_draw(self, cri, wd, ht, zoom_scale,
                           darktable.lib->proxy.colorpicker.live_samples, FALSE);
  }

  // draw colorpicker for in focus module or execute module callback hook
  // FIXME: draw picker in gui_post_expose() hook in
  // libs/colorpicker.c -- catch would be that live samples would
  // appear over guides, softproof/gamut text overlay would be hidden
  // by picker
  if(dt_iop_color_picker_is_visible(dev))
  {
    dt_print_pipe(DT_DEBUG_EXPOSE,
        "expose picker",
         port->pipe, NULL, DT_DEVICE_NONE, NULL, NULL, "%dx%d, px=%d py=%d",
         width, height, pointerx, pointery);
    GSList samples = { .data = darktable.lib->proxy.colorpicker.primary_sample,
                       .next = NULL };
    _darkroom_pickers_draw(self, cri, wd, ht, zoom_scale, &samples, TRUE);
  }

  cairo_restore(cri);

  dt_iop_module_t *dmod = dev->gui_module;

  // display mask if we have a current module activated or if the
  // masks manager module is expanded
  const gboolean display_masks =
    (dmod && dmod->enabled && dt_dev_modulegroups_test_activated(darktable.develop))
    || dt_lib_gui_get_expanded(dt_lib_get_module("masks"));

  if(dev->form_visible && display_masks)
  {
    dt_print_pipe(DT_DEBUG_EXPOSE,
        "expose masks",
         port->pipe, dev->gui_module, DT_DEVICE_NONE, NULL, NULL, "%dx%d, px=%d py=%d",
         width, height, pointerx, pointery);
    dt_masks_events_post_expose(dmod, cri, width, height, 0.0f, 0.0f, zoom_scale);
  }

  // if dragging the rotation line, do it and nothing else
  if(dev->proxy.rotate
     && (darktable.control->button_down_which == GDK_BUTTON_SECONDARY
         || dmod == dev->proxy.rotate))
  {
    // reminder, we want this to be exposed always for guidings
    if(dev->proxy.rotate && dev->proxy.rotate->gui_post_expose)
    {
      _get_zoom_pos_bnd(&dev->full, pointerx, pointery, zbound_x, zbound_y, &pzx, &pzy, &zoom_scale);
      _module_gui_post_expose(dev->proxy.rotate, cri, wd, ht, pzx, pzy, zoom_scale);
    }
  }
  else
  {
    gboolean guides = TRUE;
    // true if anything could be exposed
    if(dmod && dmod != dev->proxy.rotate)
    {
      // the cropping.exposer->gui_post_expose needs special care
      if(expose_full
        && (dmod->operation_tags_filter() & IOP_TAG_CROPPING)
        && dev->cropping.exposer
        && (dmod->iop_order < dev->cropping.exposer->iop_order))
      {
        dt_print_pipe(DT_DEBUG_EXPOSE,
                      "expose cropper",
                      port->pipe, dev->cropping.exposer,
                      DT_DEVICE_NONE, NULL, NULL, "%dx%d, px=%d py=%d",
                      width, height, pointerx, pointery);
        if(dev->cropping.exposer && dev->cropping.exposer->gui_post_expose)
        {
          _get_zoom_pos_bnd(&dev->full, pointerx, pointery, zbound_x, zbound_y, &pzx, &pzy, &zoom_scale);
          _module_gui_post_expose(dev->cropping.exposer, cri, wd, ht, pzx, pzy, zoom_scale);
        }
        guides = FALSE;
      }

      // gui active module
      if(dmod->gui_post_expose && dt_dev_modulegroups_test_activated(darktable.develop))
      {
        dt_print_pipe(DT_DEBUG_EXPOSE,
                      "expose module",
                      port->pipe, dmod,
                      DT_DEVICE_NONE, NULL, NULL,
                      "%dx%d, px=%d py=%d",
                      width, height, pointerx, pointery);
        _get_zoom_pos_bnd(&dev->full, pointerx, pointery, zbound_x, zbound_y, &pzx, &pzy, &zoom_scale);
        _module_gui_post_expose(dmod, cri, wd, ht, pzx, pzy, zoom_scale);
        // avoid drawing later if we just did via post_expose
        if(dmod->flags() & IOP_FLAGS_GUIDES_SPECIAL_DRAW)
          guides = FALSE;
      }
    }
    if(guides)
      dt_guides_draw(cri, 0.0f, 0.0f, wd, ht, zoom_scale);
  }

  cairo_restore(cri);

  // indicate if we are in gamut check or softproof mode
  if(darktable.color_profiles->mode != DT_PROFILE_NORMAL)
  {
    gchar *label = darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK
      ? _("gamut check")
      : _("soft proof");

    dt_print_pipe(DT_DEBUG_EXPOSE,
        "expose profile",
         port->pipe, NULL, port->pipe->devid, NULL, NULL, "%dx%d, px=%d py=%d. proof: %s",
         width, height, pointerx, pointery, label);

    cairo_set_source_rgba(cri, 0.5, 0.5, 0.5, 0.5);
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc =
      pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cri);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(20) * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, label, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cri, ink.height * 2, height - (ink.height * 3));
    pango_cairo_layout_path(cri, layout);
    cairo_set_source_rgb(cri, 0.7, 0.7, 0.7);
    cairo_fill_preserve(cri);
    cairo_set_line_width(cri, 0.7);
    cairo_set_source_rgb(cri, 0.3, 0.3, 0.3);
    cairo_stroke(cri);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
}

void reset(dt_view_t *self)
{
  dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_FIT, 0.0f, 0, -1.0f, -1.0f, TRUE);
}

gboolean try_enter(dt_view_t *self)
{
  const dt_imgid_t imgid = dt_act_on_get_main_image();

  if(!dt_is_valid_imgid(imgid))
  {
    // fail :(
    dt_control_log(_("no image to open!"));
    return TRUE;
  }

  // we want to wait for terminated backthumbs crawler for pipeline memory
  dt_stop_backthumbs_crawler(TRUE);

  // this loads the image from db if needed:
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  // get image and check if it has been deleted from disk first!

  char imgfilename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(img->id, imgfilename, sizeof(imgfilename), &from_cache);
  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image `%s' is currently unavailable"), img->filename);
    dt_image_cache_read_release(img);
    return TRUE;
  }
  else if(img->load_status != DT_IMAGEIO_OK)
  {
    const char *reason;
    switch(img->load_status)
    {
    case DT_IMAGEIO_FILE_NOT_FOUND:
      reason = _("file not found");
      break;
    case DT_IMAGEIO_LOAD_FAILED:
    default:
      reason = _("unspecified failure");
      break;
    case DT_IMAGEIO_UNSUPPORTED_FORMAT:
      reason = _("unsupported file format");
      break;
    case DT_IMAGEIO_UNSUPPORTED_CAMERA:
      reason = _("unsupported camera model");
      break;
    case DT_IMAGEIO_UNSUPPORTED_FEATURE:
      reason = _("unsupported feature in file");
      break;
    case DT_IMAGEIO_FILE_CORRUPTED:
      reason = _("file appears corrupt");
      break;
    case DT_IMAGEIO_IOERROR:
      reason = _("I/O error");
      break;
    case DT_IMAGEIO_CACHE_FULL:
      reason = _("cache full");
      break;
    }
    dt_control_log(_("image `%s' could not be loaded\n%s"), img->filename, reason);
    dt_image_cache_read_release(img);
    return TRUE;
  }
  // and drop the lock again.
  dt_image_cache_read_release(img);
  darktable.develop->image_storage.id = imgid;

  dt_dev_reset_chroma(darktable.develop);

  // possible enable autosaving due to conf setting but wait for some
  // seconds for first save
  darktable.develop->autosaving = (double)dt_conf_get_int("autosave_interval") > 1.0;
  darktable.develop->autosave_time = dt_get_wtime() + 10.0;
  return FALSE;
}

#ifdef USE_LUA

static void _fire_darkroom_image_loaded_event(const bool clean,
                                              const dt_imgid_t imgid)
{
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-loaded",
      LUA_ASYNC_TYPENAME, "bool", clean,
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(imgid),
      LUA_ASYNC_DONE);
}

#endif

static gboolean _dev_load_requested_image(gpointer user_data);

static void _dev_change_image(dt_develop_t *dev,
                              const dt_imgid_t imgid)
{
  if(dt_check_gimpmode("file"))
  {
    dt_control_log(_("can't change image in GIMP plugin mode"));
    return;
  }
  // Pipe reset needed when changing image
  // FIXME: synch with dev_init() and dev_cleanup() instead of redoing it

  // change active image
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = g_slist_prepend(NULL, GINT_TO_POINTER(imgid));
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // if the previous shown image is selected and the selection is unique
  // then we change the selected image to the new one
  if(dt_is_valid_imgid(dev->requested_id))
  {
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "SELECT m.imgid"
       " FROM memory.collected_images as m, main.selected_images as s"
       " WHERE m.imgid=s.imgid",
       -1, &stmt, NULL);
    // clang-format on
    gboolean follow = FALSE;
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_int(stmt, 0) == dev->requested_id
         && sqlite3_step(stmt) != SQLITE_ROW)
      {
        follow = TRUE;
      }
    }
    sqlite3_finalize(stmt);
    if(follow)
    {
      dt_selection_select_single(darktable.selection, imgid);
    }
  }

  // disable color picker when changing image
  if(darktable.lib->proxy.colorpicker.picker_proxy)
    dt_iop_color_picker_reset(darktable.lib->proxy.colorpicker.picker_proxy->module, FALSE);

  // update aspect ratio
  if(dev->preview_pipe->backbuf
     && dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID)
  {
    const float aspect_ratio =
      (float)dev->preview_pipe->backbuf_width / (float)dev->preview_pipe->backbuf_height;
    dt_image_set_aspect_ratio_to(dev->preview_pipe->image.id, aspect_ratio, TRUE);
  }
  else
  {
    dt_image_set_aspect_ratio(dev->image_storage.id, TRUE);
  }

  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;

  // get current plugin in focus before defocus
  const dt_iop_module_t *gui_module = dt_dev_gui_module();
  if(gui_module)
  {
    dt_conf_set_string("plugins/darkroom/active",
                       gui_module->op);
  }

  // store last active group
  dt_conf_set_int("plugins/darkroom/groups", dt_dev_modulegroups_get(dev));

  // commit any pending changes in focused module
  dt_iop_request_focus(NULL);

  g_assert(dev->gui_attached);

  // commit image ops to db
  dt_dev_write_history(dev);

  dev->requested_id = imgid;
  dt_dev_clear_chroma_troubles(dev);

  // possible enable autosaving due to conf setting but wait for some
  // seconds for first save
  darktable.develop->autosaving = (double)dt_conf_get_int("autosave_interval") > 1.0;
  darktable.develop->autosave_time = dt_get_wtime() + 10.0;

  g_idle_add(_dev_load_requested_image, dev);
}

static gboolean _dev_load_requested_image(gpointer user_data)
{
  dt_develop_t *dev = user_data;
  const dt_imgid_t imgid = dev->requested_id;

  if(dev->image_storage.id == NO_IMGID
     && dev->image_storage.id == imgid) return G_SOURCE_REMOVE;

  // make sure we can destroy and re-setup the pixel pipes.
  // we acquire the pipe locks, which will block the processing threads
  // in darkroom mode before they touch the pipes (init buffers etc).
  // we don't block here, since we hold the gdk lock, which will
  // result in circular locking when background threads emit signals
  // which in turn try to acquire the gdk lock.
  //
  // worst case, it'll drop some change image events. sorry.
  if(dt_pthread_mutex_BAD_trylock(&dev->preview_pipe->mutex))
  {

#ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif
  return G_SOURCE_CONTINUE;
  }
  if(dt_pthread_mutex_BAD_trylock(&dev->full.pipe->mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe->mutex);

 #ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif

   return G_SOURCE_CONTINUE;
  }
  if(dt_pthread_mutex_BAD_trylock(&dev->preview2.pipe->mutex))
  {
    dt_pthread_mutex_BAD_unlock(&dev->full.pipe->mutex);
    dt_pthread_mutex_BAD_unlock(&dev->preview_pipe->mutex);

 #ifdef USE_LUA

  _fire_darkroom_image_loaded_event(FALSE, imgid);

#endif

   return G_SOURCE_CONTINUE;
  }

  const dt_imgid_t old_imgid = dev->image_storage.id;

  dt_overlay_add_from_history(old_imgid);

  // be sure light table will update the thumbnail
  if(!dt_history_hash_is_mipmap_synced(old_imgid))
  {
    dt_mipmap_cache_remove(old_imgid);
    dt_image_update_final_size(old_imgid);
    dt_image_synch_xmp(old_imgid);
    dt_history_hash_set_mipmap(old_imgid);
#ifdef USE_LUA
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
        0, NULL, NULL,
        LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-history-changed",
        LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(old_imgid),
        LUA_ASYNC_DONE);
#endif
    // update the lighttable metadata_view with any changes
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_METADATA_CHANGED);
  }

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);

  // cleanup visible masks
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }
  dt_masks_change_form_gui(NULL);

  while(dev->history)
  {
    // clear history of old image
    dt_dev_history_item_t *hist = dev->history->data;
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  // get new image:
  dt_dev_reload_image(dev, imgid);

  // make sure no signals propagate here:
  ++darktable.gui->reset;

  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->full.pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview2.pipe);

  // chroma data will be fixed by reading whitebalance data from history
  dt_dev_reset_chroma(dev);

  const guint nb_iop = g_list_length(dev->iop);
  for(int i = nb_iop - 1; i >= 0; i--)
  {
    dt_iop_module_t *module = (g_list_nth_data(dev->iop, i));

    // the base module is the one with the lowest multi_priority
    int base_multi_priority = 0;
    for(const GList *l = dev->iop; l; l = g_list_next(l))
    {
      dt_iop_module_t *mod = l->data;
      if(dt_iop_module_is(module->so, mod->op))
        base_multi_priority = MIN(base_multi_priority, mod->multi_priority);
    }

    if(module->multi_priority == base_multi_priority) // if the module is the "base" instance, we keep it
    {
      module->iop_order =
        dt_ioppr_get_iop_order(dev->iop_order_list, module->op, module->multi_priority);
      module->multi_priority = 0;
      module->multi_name[0] = '\0';
      dt_iop_reload_defaults(module);
    }
    else // else we delete it and remove it from the panel
    {
      if(!dt_iop_is_hidden(module))
      {
        dt_iop_gui_cleanup_module(module);
      }

      // we remove the module from the list
      dev->iop = g_list_remove_link(dev->iop, g_list_nth(dev->iop, i));

      // we cleanup the module
      dt_action_cleanup_instance_iop(module);

      free(module);
    }
  }
  dev->iop = g_list_sort(dev->iop, dt_sort_iop_by_order);

  // we also clear the saved modules
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  // and masks
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);
  dev->allforms = NULL;

  dt_dev_pixelpipe_create_nodes(dev->full.pipe, dev);
  dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
  if(dev->preview2.widget && GTK_IS_WIDGET(dev->preview2.widget))
    dt_dev_pixelpipe_create_nodes(dev->preview2.pipe, dev);
  dt_dev_read_history(dev);

  // we have to init all module instances other than "base" instance
  char option[1024];
  for(const GList *modules = g_list_last(dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = modules->data;
    if(module->multi_priority > 0)
    {
      if(!dt_iop_is_hidden(module))
      {
        dt_iop_gui_init(module);

        /* add module to right panel with safe header buttons */
        dt_iop_gui_set_expander(module);
        dt_iop_gui_update_blending(module);
      }
    }
    else
    {
      //  update the module header to ensure proper multi-name display
      if(!dt_iop_is_hidden(module))
      {
        // Make sure module header buttons are reset to a safe state
        dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
        snprintf(option, sizeof(option), "plugins/darkroom/%s/expanded", module->op);
        module->expanded = dt_conf_get_bool(option);
        dt_iop_gui_update_expanded(module);
        if(module->change_image) module->change_image(module);
        dt_iop_gui_update_header(module);
      }
    }
  }

  dt_dev_pop_history_items(dev, dev->history_end);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  // set the module list order
  dt_dev_reorder_gui_module_list(dev);

  /* cleanup histograms */
  g_list_foreach(dev->iop, (GFunc)dt_iop_cleanup_histogram, (gpointer)NULL);

  /* make signals work again, we can't restore the active_plugin while signals
     are blocked due to implementation of dt_iop_request_focus so we do it now
     A double history entry is not generated.
  */
  --darktable.gui->reset;

  dt_dev_masks_list_change(dev);

  /* Now we can request focus again and write a safe plugins/darkroom/active */
  const char *active_plugin = dt_conf_get_string_const("plugins/darkroom/active");
  if(active_plugin)
  {
    gboolean valid = FALSE;
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = modules->data;
      if(dt_iop_module_is(module->so, active_plugin))
      {
        valid = TRUE;
        dt_iop_request_focus(module);
      }
    }
    if(!valid)
    {
      dt_conf_set_string("plugins/darkroom/active", "");
    }
  }

  // Signal develop initialize
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED);

  // release pixel pipe mutices
  dt_pthread_mutex_BAD_unlock(&dev->preview2.pipe->mutex);
  dt_pthread_mutex_BAD_unlock(&dev->preview_pipe->mutex);
  dt_pthread_mutex_BAD_unlock(&dev->full.pipe->mutex);

  // update hint message
  dt_collection_hint_message(darktable.collection);

  // update accels_window
  darktable.view_manager->accels_window.prevent_refresh = FALSE;
  if(darktable.view_manager->accels_window.window
     && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);

  // just make sure at this stage we have only history info into the undo, all automatic
  // tagging should be ignored.
  dt_undo_clear(darktable.undo, DT_UNDO_TAGS);

  //connect iop accelerators
  dt_iop_connect_accels_all();

  /* last set the group to update visibility of iop modules for new pipe */
  dt_dev_modulegroups_set(dev, dt_conf_get_int("plugins/darkroom/groups"));

  dt_image_check_camera_missing_sample(&dev->image_storage);

#ifdef USE_LUA

  _fire_darkroom_image_loaded_event(TRUE, imgid);

#endif

  return G_SOURCE_REMOVE;
}

static void _view_darkroom_filmstrip_activate_callback(gpointer instance,
                                                       const dt_imgid_t imgid,
                                                       const dt_view_t *self)
{
  if(dt_is_valid_imgid(imgid))
  {
    // switch images in darkroom mode:
    dt_develop_t *dev = self->data;

    _dev_change_image(dev, imgid);
    // move filmstrip
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), imgid, TRUE);
    // force redraw
    dt_control_queue_redraw();
  }
}

static void _dev_jump_image(dt_develop_t *dev, int diff, gboolean by_key)
{
  if(dt_check_gimpmode("file"))
  {
    dt_control_log(_("can't change image in GIMP plugin mode"));
    return;
  }
  const dt_imgid_t imgid = dev->requested_id;
  int new_offset = 1;
  dt_imgid_t new_id = NO_IMGID;

  // we new offset and imgid after the jump
  sqlite3_stmt *stmt;
  // clang-format off
  gchar *query =
    g_strdup_printf("SELECT rowid, imgid "
                    "FROM memory.collected_images "
                    "WHERE rowid=(SELECT rowid "
                    "               FROM memory.collected_images"
                    "               WHERE imgid=%d)+%d",
                    imgid, diff);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    new_offset = sqlite3_column_int(stmt, 0);
    new_id = sqlite3_column_int(stmt, 1);
  }
  else if(diff > 0)
  {
    // if we are here, that means that the current is not anymore in
    // the list in this case, let's use the current offset image
    new_id = dt_ui_thumbtable(darktable.gui->ui)->offset_imgid;
    new_offset = dt_ui_thumbtable(darktable.gui->ui)->offset;
  }
  else
  {
    // if we are here, that means that the current is not anymore in
    // the list in this case, let's use the image before current
    // offset
    new_offset = MAX(1, dt_ui_thumbtable(darktable.gui->ui)->offset - 1);
    sqlite3_stmt *stmt2;
    gchar *query2 =
      g_strdup_printf("SELECT imgid FROM memory.collected_images WHERE rowid=%d",
                      new_offset);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query2, -1, &stmt2, NULL);
    if(sqlite3_step(stmt2) == SQLITE_ROW)
    {
      new_id = sqlite3_column_int(stmt2, 0);
    }
    else
    {
      new_id = dt_ui_thumbtable(darktable.gui->ui)->offset_imgid;
      new_offset = dt_ui_thumbtable(darktable.gui->ui)->offset;
    }
    g_free(query2);
    sqlite3_finalize(stmt2);
  }
  g_free(query);
  sqlite3_finalize(stmt);

  if(!dt_is_valid_imgid(new_id) || new_id == imgid) return;

  // if id seems valid, we change the image and move filmstrip
  _dev_change_image(dev, new_id);
  dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), new_offset, TRUE);

  // if it's a change by key_press, we set mouse_over to the active image
  if(by_key) dt_control_set_mouse_over_id(new_id);
}

static void zoom_key_accel(dt_action_t *action)
{
  // flip closeup/no closeup, no difference whether it was 1 or larger
  dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_1, 0.0f, -1, -1.0f, -1.0f, TRUE);
}

static void zoom_in_callback(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_develop_t *dev = self->data;

  scrolled(self, dev->full.width / 2, dev->full.height / 2, 1, GDK_CONTROL_MASK);
}

static void zoom_out_callback(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_develop_t *dev = self->data;

  scrolled(self, dev->full.width / 2, dev->full.height / 2, 0, GDK_CONTROL_MASK);
}

static void skip_f_key_accel_callback(dt_action_t *action)
{
  _dev_jump_image(dt_action_view(action)->data, 1, TRUE);
}

// Toggles the pinned state in the 2nd window.  Has no effect if the 2nd
// window is not open.
static void _toggle_pin_second_window_action(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_develop_t *dev = self->data;

  if(!dev->second_wnd) return;

  dt_dev_toggle_preview2_pinned(dev);
}

static void skip_b_key_accel_callback(dt_action_t *action)
{
  _dev_jump_image(dt_action_view(action)->data, -1, TRUE);
}

static void _darkroom_ui_pipe_finish_signal_callback(gpointer instance,
                                                     gpointer data)
{
  dt_control_queue_redraw_center();
}

static void _darkroom_ui_preview2_pipe_finish_signal_callback(gpointer instance,
                                                              gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_develop_t *dev = self->data;
  if(dev->preview2.widget)
    gtk_widget_queue_draw(dev->preview2.widget);
}

static void _darkroom_ui_favorite_presets_popupmenu(GtkWidget *w,
                                                    gpointer user_data)
{
  /* create favorites menu and popup */
  dt_gui_favorite_presets_menu_show(w);
}

static void _darkroom_ui_apply_style_activate_callback(GtkMenuItem *menuitem,
                                                       const dt_stylemenu_data_t *menu_data)
{
  GdkEvent *event = gtk_get_current_event();
  if(event->type == GDK_KEY_PRESS)
    dt_styles_apply_to_dev(menu_data->name, darktable.develop->image_storage.id);
  gdk_event_free(event);
}

static gboolean _darkroom_ui_apply_style_button_callback
  (GtkMenuItem *menuitem,
   GdkEventButton *event,
   const dt_stylemenu_data_t *menu_data)
{
  if(event->button == GDK_BUTTON_PRIMARY)
    dt_styles_apply_to_dev(menu_data->name, darktable.develop->image_storage.id);
  else
    dt_shortcut_copy_lua(NULL, menu_data->name);

  return FALSE;
}

static void _darkroom_ui_apply_style_popupmenu(GtkWidget *w,
                                               gpointer user_data)
{
  /* if we got any styles, lets popup menu for selection */
  GtkMenuShell *menu =
    dtgtk_build_style_menu_hierarchy(FALSE,
                                     _darkroom_ui_apply_style_activate_callback,
                                     _darkroom_ui_apply_style_button_callback,
                                     user_data);
  if(menu)
  {
    dt_gui_menu_popup(GTK_MENU(menu), w, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST);
  }
  else
    dt_control_log(_("no styles have been created yet"));
}

static void _second_window_quickbutton_clicked(GtkWidget *w,
                                               dt_develop_t *dev)
{
  if(dev->second_wnd && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
  {
    GtkWidget *wnd = dev->second_wnd;

    // Disable the button for the duration of close+destroy to fix possible
    // race condition when re-opening the 2nd window while the cleanup code
    // is still running.
    gtk_widget_set_sensitive(w, FALSE);

    _darkroom_ui_second_window_write_config(wnd);
    dt_conf_set_bool("second_window/last_visible", FALSE);
    _darkroom_ui_second_window_cleanup(dev);
    gtk_widget_hide(wnd);

    // Flush pending events to let macOS process the hide before destroy
    while(gtk_events_pending())
      gtk_main_iteration_do(FALSE);

    gtk_widget_destroy(wnd);

    // Re-enable the button when cleanup is done.
    gtk_widget_set_sensitive(w, TRUE);
  }
  else if(dev->second_wnd == NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
    _darkroom_display_second_window(dev);
}

/** toolbar buttons */

static gboolean _toolbar_show_popup(gpointer user_data)
{
  GtkPopover *popover = GTK_POPOVER(user_data);

  GtkWidget *button = gtk_popover_get_relative_to(popover);
  GdkDevice *pointer =
    gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));

  int x, y;
  GdkWindow *pointer_window = gdk_device_get_window_at_position(pointer, &x, &y);
  gpointer   pointer_widget = NULL;
  if(pointer_window)
    gdk_window_get_user_data(pointer_window, &pointer_widget);

  GdkRectangle rect = { gtk_widget_get_allocated_width(button) / 2, 0, 1, 1 };

  if(pointer_widget && button != pointer_widget)
    gtk_widget_translate_coordinates(pointer_widget, button, x, y, &rect.x, &rect.y);

  gtk_popover_set_pointing_to(popover, &rect);

  // for the guides popover, it need to be updated before we show it
  if(darktable.view_manager
     && GTK_WIDGET(popover) == darktable.view_manager->guides_popover)
    dt_guides_update_popover_values();

  gtk_widget_show_all(GTK_WIDGET(popover));

  // cancel glib timeout if invoked by long button press
  return FALSE;
}

static void _full_color_assessment_callback(GtkToggleButton *checkbutton, dt_develop_t *dev)
{
  dev->full.color_assessment = gtk_toggle_button_get_active(checkbutton);
  dt_conf_set_bool("full_window/color_assessment", dev->full.color_assessment);
  dt_dev_configure(&dev->full);
}

static void _color_assessment_border_width_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *) user_data;
  dt_conf_set_float("darkroom/ui/color_assessment_total_border_width", dt_bauhaus_slider_get(slider));
  if (dev->full.color_assessment)
  {
    dt_dev_configure(&dev->full);
  }
  else
  {
    gtk_button_clicked(GTK_BUTTON(dev->color_assessment.button));
  }
}

static void _color_assessment_border_white_ratio_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *) user_data;
  dt_conf_set_float("darkroom/ui/color_assessment_border_white_ratio", dt_bauhaus_slider_get(slider));
  if (dev->full.color_assessment)
  {
    dt_dev_reprocess_center(dev);
  }
  else
  {
    gtk_button_clicked(GTK_BUTTON(dev->color_assessment.button));
  }
}

static void _latescaling_quickbutton_clicked(GtkWidget *w,
                                             gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  if(!dev->gui_attached) return;

  dev->late_scaling.enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
  dt_conf_set_bool("darkroom/ui/late_scaling/enabled", dev->late_scaling.enabled);

  // we just toggled off and had one of HQ pipelines running
  if(!dev->late_scaling.enabled
      && (dev->full.pipe->processing
          || (dev->second_wnd && dev->preview2.pipe->processing)))
  {
    if(dev->full.pipe->processing)
      dt_atomic_set_int(&dev->full.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_HQ);
    if(dev->second_wnd && dev->preview2.pipe->processing)
      dt_atomic_set_int(&dev->preview2.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_HQ);

    // do it the hard way for safety
    dt_dev_pixelpipe_rebuild(dev);
  }
  else
  {
    if(dev->second_wnd)
      dt_dev_reprocess_all(dev);
    else
      dt_dev_reprocess_center(dev);
  }
}

/* overlay color */
static void _guides_quickbutton_clicked(GtkWidget *widget,
                                        gpointer user_data)
{
  dt_guides_button_toggled(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  dt_control_queue_redraw_center();
}

static void _guides_view_changed(gpointer instance,
                                 dt_view_t *old_view,
                                 dt_view_t *new_view,
                                 dt_lib_module_t *self)
{
  dt_guides_update_button_state();
}

/* overexposed */
static void _overexposed_quickbutton_clicked(GtkWidget *w,
                                             gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.enabled = !d->overexposed.enabled;
  dt_conf_set_bool("darkroom/ui/overexposed/enabled", d->overexposed.enabled);
  dt_dev_reprocess_center(d);
}

static void _colorscheme_callback(GtkWidget *combo,
                                  gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _lower_callback(GtkWidget *slider,
                            gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.lower = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _upper_callback(GtkWidget *slider,
                            gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.upper = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _mode_callback(GtkWidget *slider,
                           gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.mode = dt_bauhaus_combobox_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));
  else
    dt_dev_reprocess_center(d);
}

/* rawoverexposed */
static void _rawoverexposed_quickbutton_clicked(GtkWidget *w,
                                                gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.enabled = !d->rawoverexposed.enabled;
  dt_conf_set_bool("darkroom/ui/rawoverexposed/enabled", d->rawoverexposed.enabled);
  dt_dev_reprocess_center(d);
}

static void _rawoverexposed_mode_callback(GtkWidget *combo,
                                          gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.mode = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _rawoverexposed_colorscheme_callback(GtkWidget *combo,
                                                 gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

static void _rawoverexposed_threshold_callback(GtkWidget *slider,
                                               gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.threshold = dt_bauhaus_slider_get(slider);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));
  else
    dt_dev_reprocess_center(d);
}

/* softproof */
static void _softproof_quickbutton_clicked(GtkWidget *w,
                                           gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF)
    darktable.color_profiles->mode = DT_PROFILE_NORMAL;
  else
    darktable.color_profiles->mode = DT_PROFILE_SOFTPROOF;

  _update_softproof_gamut_checking(d);

  dt_dev_reprocess_center(d);
}

/* gamut */
static void _gamut_quickbutton_clicked(GtkWidget *w,
                                       gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK)
    darktable.color_profiles->mode = DT_PROFILE_NORMAL;
  else
    darktable.color_profiles->mode = DT_PROFILE_GAMUTCHECK;

  _update_softproof_gamut_checking(d);

  dt_dev_reprocess_center(d);
}

/* set the gui state for both softproof and gamut checking */
static void _update_softproof_gamut_checking(dt_develop_t *d)
{
  g_signal_handlers_block_by_func(d->profile.softproof_button,
                                  _softproof_quickbutton_clicked, d);
  g_signal_handlers_block_by_func(d->profile.gamut_button,
                                  _gamut_quickbutton_clicked, d);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.softproof_button),
                               darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.gamut_button),
                               darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK);

  g_signal_handlers_unblock_by_func(d->profile.softproof_button,
                                    _softproof_quickbutton_clicked, d);
  g_signal_handlers_unblock_by_func(d->profile.gamut_button,
                                    _gamut_quickbutton_clicked, d);
}

static void _display_intent_callback(GtkWidget *combo,
                                     gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const int pos = dt_bauhaus_combobox_get(combo);

  dt_iop_color_intent_t new_intent = darktable.color_profiles->display_intent;

  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display_intent)
  {
    darktable.color_profiles->display_intent = new_intent;
    dt_dev_reprocess_all(d);
  }
}

static void _display2_intent_callback(GtkWidget *combo,
                                      gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  const int pos = dt_bauhaus_combobox_get(combo);

  dt_iop_color_intent_t new_intent = darktable.color_profiles->display2_intent;

  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display2_intent)
  {
    darktable.color_profiles->display2_intent = new_intent;
    dt_dev_reprocess_all(d);
  }
}

static void _softproof_profile_callback(GtkWidget *combo,
                                        gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles;
      profiles;
      profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
    if(pp->out_pos == pos)
    {
      if(darktable.color_profiles->softproof_type != pp->type
        || (darktable.color_profiles->softproof_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->softproof_filename, pp->filename)))

      {
        darktable.color_profiles->softproof_type = pp->type;
        g_strlcpy(darktable.color_profiles->softproof_filename, pp->filename,
                  sizeof(darktable.color_profiles->softproof_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to sRGB. shouldn't happen
  dt_print(DT_DEBUG_ALWAYS,
           "can't find softproof profile `%s', using sRGB instead",
           dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->softproof_type != DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_type = DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_filename[0] = '\0';

end:
  if(profile_changed)
  {
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF);
    dt_dev_reprocess_all(d);
  }
}

static void _display_profile_callback(GtkWidget *combo,
                                      gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);

  for(GList *profiles = darktable.color_profiles->profiles;
      profiles;
      profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
    if(pp->display_pos == pos)
    {
      if(darktable.color_profiles->display_type != pp->type
        || (darktable.color_profiles->display_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->display_filename, pp->filename)))
      {
        darktable.color_profiles->display_type = pp->type;
        g_strlcpy(darktable.color_profiles->display_filename, pp->filename,
                  sizeof(darktable.color_profiles->display_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to system display profile. shouldn't happen
  dt_print(DT_DEBUG_ALWAYS,
           "can't find display profile `%s', using system display profile instead",
           dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display_type != DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_type = DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_DISPLAY);
    dt_dev_reprocess_all(d);
  }
}

static void _display2_profile_callback(GtkWidget *combo,
                                       gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);

  for(GList *profiles = darktable.color_profiles->profiles;
      profiles;
      profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
    if(pp->display2_pos == pos)
    {
      if(darktable.color_profiles->display2_type != pp->type
         || (darktable.color_profiles->display2_type == DT_COLORSPACE_FILE
             && strcmp(darktable.color_profiles->display2_filename, pp->filename)))
      {
        darktable.color_profiles->display2_type = pp->type;
        g_strlcpy(darktable.color_profiles->display2_filename, pp->filename,
                  sizeof(darktable.color_profiles->display2_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to system display2 profile. shouldn't happen
  dt_print(DT_DEBUG_ALWAYS,
           "can't find preview display profile `%s', using system display profile instead",
           dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display2_type != DT_COLORSPACE_DISPLAY2;
  darktable.color_profiles->display2_type = DT_COLORSPACE_DISPLAY2;
  darktable.color_profiles->display2_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display2_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_DISPLAY2);
    dt_dev_reprocess_all(d);
  }
}

static void _display2_color_assessment_callback(GtkToggleButton *checkbutton, dt_develop_t *dev)
{
  dev->preview2.color_assessment = gtk_toggle_button_get_active(checkbutton);
  dt_conf_set_bool("second_window/color_assessment", dev->preview2.color_assessment);
  dt_dev_configure(&dev->preview2);
}

static void _histogram_profile_callback(GtkWidget *combo,
                                        gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);

  for(GList *profiles = darktable.color_profiles->profiles;
      profiles;
      profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = profiles->data;
    if(pp->category_pos == pos)
    {
      if(darktable.color_profiles->histogram_type != pp->type
        || (darktable.color_profiles->histogram_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->histogram_filename, pp->filename)))
      {
        darktable.color_profiles->histogram_type = pp->type;
        g_strlcpy(darktable.color_profiles->histogram_filename, pp->filename,
                  sizeof(darktable.color_profiles->histogram_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to export profile. shouldn't happen
  dt_print(DT_DEBUG_ALWAYS,
           "can't find histogram profile `%s', using export profile instead",
           dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->histogram_type != DT_COLORSPACE_WORK;
  darktable.color_profiles->histogram_type = DT_COLORSPACE_WORK;
  darktable.color_profiles->histogram_filename[0] = '\0';

end:
  if(profile_changed)
  {
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            DT_COLORSPACES_PROFILE_TYPE_HISTOGRAM);
    dt_dev_reprocess_all(d);
  }
}

static void _preference_changed(gpointer instance,
                                gpointer user_data)
{
  GtkWidget *display_intent = GTK_WIDGET(user_data);

  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  gtk_widget_set_no_show_all(display_intent, !force_lcms2);
  gtk_widget_set_visible(display_intent, force_lcms2);

  dt_get_sysresource_level();
  dt_opencl_update_settings();
  dt_configure_ppd_dpi(darktable.gui);
}

static void _preference_changed_button_hide(gpointer instance,
                                            const dt_view_t *self)
{
  dt_develop_t *dev = self->data;
  for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = modules->data;

    if(module->header)
      dt_iop_add_remove_mask_indicator
        (module,
         (module->blend_params->mask_mode != DEVELOP_MASK_DISABLED) &&
         (module->blend_params->mask_mode != DEVELOP_MASK_ENABLED));
  }
}

static void _update_display_profile_cmb(GtkWidget *cmb_display_profile)
{
  for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = l->data;
    if(prof->display_pos > -1)
    {
      if(prof->type == darktable.color_profiles->display_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
      {
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display_pos);
          break;
        }
      }
    }
  }
}

static void _update_display2_profile_cmb(GtkWidget *cmb_display_profile)
{
  for(const GList *l = darktable.color_profiles->profiles;
      l;
      l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = l->data;
    if(prof->display2_pos > -1)
    {
      if(prof->type == darktable.color_profiles->display2_type
         && (prof->type != DT_COLORSPACE_FILE
             || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
      {
        if(dt_bauhaus_combobox_get(cmb_display_profile) != prof->display2_pos)
        {
          dt_bauhaus_combobox_set(cmb_display_profile, prof->display2_pos);
          break;
        }
      }
    }
  }
}

static void _display_profile_changed(gpointer instance,
                                     const uint8_t profile_type,
                                     gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);

  _update_display_profile_cmb(cmb_display_profile);
}

static void _display2_profile_changed(gpointer instance,
                                      const uint8_t profile_type,
                                      gpointer user_data)
{
  GtkWidget *cmb_display_profile = GTK_WIDGET(user_data);

  _update_display2_profile_cmb(cmb_display_profile);
}

/** end of toolbox */

static void _brush_size_up_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible)
    dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, 0);
}
static void _brush_size_down_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible)
    dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, 0);
}

static void _brush_hardness_up_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible)
    dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, GDK_SHIFT_MASK);
}
static void _brush_hardness_down_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible)
    dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, GDK_SHIFT_MASK);
}

static void _brush_opacity_up_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible)
    dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 1, GDK_CONTROL_MASK);
}
static void _brush_opacity_down_callback(dt_action_t *action)
{
  dt_develop_t *dev = dt_action_view(action)->data;

  if(dev->form_visible)
    dt_masks_events_mouse_scrolled(dev->gui_module, 0, 0, 0, GDK_CONTROL_MASK);
}

static void _overlay_cycle_callback(dt_action_t *action)
{
  const int currentval = dt_conf_get_int("darkroom/ui/overlay_color");
  const int nextval = (currentval + 1) % DT_DEV_OVERLAY_LAST; // colors can go from 0 to DT_DEV_OVERLAY_LAST-1
  dt_conf_set_int("darkroom/ui/overlay_color", nextval);
  dt_guides_set_overlay_colors();
  dt_control_queue_redraw_center();
}

static void _toggle_mask_visibility_callback(dt_action_t *action)
{
  if(darktable.gui->reset) return;

  dt_develop_t *dev = dt_action_view(action)->data;
  dt_iop_module_t *mod = dev->gui_module;

  //retouch and spot removal module use masks differently and have
  //different buttons associated keep the shortcuts independent
  if(mod
     && !dt_iop_module_is(mod->so, "spots")
     && !dt_iop_module_is(mod->so, "retouch"))
  {
    dt_iop_gui_blend_data_t *bd = mod->blend_data;

    ++darktable.gui->reset;

    dt_iop_color_picker_reset(mod, TRUE);

    dt_masks_form_t *grp =
      dt_masks_get_from_id(darktable.develop, mod->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
    {
      if(bd->masks_shown == DT_MASKS_EDIT_OFF)
        bd->masks_shown = DT_MASKS_EDIT_FULL;
      else
        bd->masks_shown = DT_MASKS_EDIT_OFF;

      gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
      dt_masks_set_edit_mode(mod, bd->masks_shown);

      // set all add shape buttons to inactive
      for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);
    }

    --darktable.gui->reset;
  }
}

static void _darkroom_undo_callback(dt_action_t *action)
{
  dt_undo_do_undo(darktable.undo, DT_UNDO_DEVELOP);
}

static void _darkroom_redo_callback(dt_action_t *action)
{
  dt_undo_do_redo(darktable.undo, DT_UNDO_DEVELOP);
}

static void _darkroom_do_synchronize_selection_callback(dt_action_t *action)
{
  dt_gui_cursor_set_busy();

  GList *sel = dt_selection_get_list(darktable.selection, FALSE, FALSE);

  // write histroy for edited picture
  dt_dev_write_history(darktable.develop);

  const dt_imgid_t imgid = darktable.develop->image_storage.id;

  // get first item in list, last edited iop
  GList *hist = dt_history_get_items(imgid, FALSE, FALSE, FALSE);
  dt_history_item_t *first_item = (dt_history_item_t *)g_list_first(hist)->data;

  // the iop num in the history
  GList *op = g_list_append(NULL, GINT_TO_POINTER(first_item->num));

  g_list_free_full(hist, g_free);

  // group all changes for atomic undo/redo
  dt_undo_start_group(darktable.undo, DT_UNDO_HISTORY);

  // copy history item into the all selected items
  for(GList *l = sel;
      l;
      l = g_list_next(l))
  {
    // target picture
    const dt_imgid_t dest_imgid = GPOINTER_TO_INT(l->data);
    if(dest_imgid != imgid)
    {
      dt_history_copy_and_paste_on_image(imgid,
                                         dest_imgid,
                                         TRUE,
                                         op,
                                         TRUE,
                                         FALSE,
                                         TRUE);
    }
  }

  dt_undo_end_group(darktable.undo);

  g_list_free(op);
  g_list_free(sel);

  dt_gui_cursor_clear_busy();
}

static void _change_slider_accel_precision(dt_action_t *action);

static float _action_process_skip_mouse(gpointer target,
                                        const dt_action_element_t element,
                                        const dt_action_effect_t effect,
                                        const float move_size)
{
  if(DT_PERFORM_ACTION(move_size))
  {
    switch(effect)
    {
    case DT_ACTION_EFFECT_ON:
      darktable.develop->darkroom_skip_mouse_events = TRUE;
      break;
    case DT_ACTION_EFFECT_OFF:
      darktable.develop->darkroom_skip_mouse_events = FALSE;
      break;
    default:
      darktable.develop->darkroom_skip_mouse_events ^= TRUE;
    }

    // don't turn on if drag underway; would not receive button_released
    if(darktable.control->button_down)
      darktable.develop->darkroom_skip_mouse_events = FALSE;
  }

  return darktable.develop->darkroom_skip_mouse_events;
}

const dt_action_def_t dt_action_def_skip_mouse
  = { N_("hold"),
      _action_process_skip_mouse,
      dt_action_elements_hold,
      NULL, TRUE };

static float _action_process_preview(gpointer target,
                                     const dt_action_element_t element,
                                     const dt_action_effect_t effect,
                                     const float move_size)
{
  dt_develop_t *lib = darktable.view_manager->proxy.darkroom.view->data;

  if(DT_PERFORM_ACTION(move_size))
  {
    if(lib->full_preview)
    {
      if(effect != DT_ACTION_EFFECT_ON)
      {
        dt_ui_restore_panels(darktable.gui->ui);
        // restore previously stored zoom settings
        dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_RESTORE,
                         0.0f, 0, -1.0f, -1.0f, TRUE);
        lib->full_preview = FALSE;
        dt_iop_request_focus(lib->full_preview_last_module);
        dt_masks_set_edit_mode(dt_dev_gui_module(), lib->full_preview_masks_state);
        dt_dev_invalidate(darktable.develop);
        dt_control_queue_redraw_center();
        dt_control_navigation_redraw();
      }
    }
    else
    {
      if(effect != DT_ACTION_EFFECT_OFF &&
         lib->preview_pipe->status != DT_DEV_PIXELPIPE_DIRTY &&
         lib->preview_pipe->status != DT_DEV_PIXELPIPE_INVALID)
      {
        lib->full_preview = TRUE;
        // we hide all panels
        for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
          dt_ui_panel_show(darktable.gui->ui, k, FALSE, FALSE);
        // we remember the masks edit state
        dt_iop_module_t *gui_module = dt_dev_gui_module();
        if(gui_module)
        {
          dt_iop_gui_blend_data_t *bd = gui_module->blend_data;
          if(bd) lib->full_preview_masks_state = bd->masks_shown;
        }
        // we set the zoom values to "fit" after storing previous settings
        dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_FULL_PREVIEW,
                         0.0f, 0, -1.0f, -1.0f, TRUE);
        // we quit the active iop if any
        lib->full_preview_last_module = gui_module;
        dt_iop_request_focus(NULL);
        gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
        dt_dev_invalidate(darktable.develop);
        dt_control_queue_redraw_center();
      }
    }
  }

  return (float)lib->full_preview;
}

const dt_action_def_t dt_action_def_preview
  = { N_("preview"),
      _action_process_preview,
      dt_action_elements_hold,
      NULL, TRUE };

static float _action_process_move(gpointer target,
                                  const dt_action_element_t element,
                                  const dt_action_effect_t effect,
                                  const float move_size)
{
  dt_develop_t *dev = darktable.view_manager->proxy.darkroom.view->data;

  if(DT_PERFORM_ACTION(move_size))
  {
    // For each cursor press, move fifth of screen by default
    float factor = 0.2f * move_size;
    if(effect == DT_ACTION_EFFECT_DOWN)
      factor *= -1;

    dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, factor, 0,
                     target ? dev->full.width : 0,
                     target ? 0 : - dev->full.height, TRUE);
  }

  return 0; // FIXME return position (%)
}

const dt_action_element_def_t _action_elements_move[]
  = { { NULL, dt_action_effect_value } };

const dt_action_def_t _action_def_move
  = { N_("move"),
      _action_process_move,
      _action_elements_move,
      NULL, TRUE };

static gboolean _quickbutton_press_release(GtkWidget *button,
                                           GdkEventButton *event,
                                           GtkWidget *popover)
{
  static guint start_time = 0;

  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-long-press-time", &delay, NULL);

  if((event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_SECONDARY) ||
     (event->type == GDK_BUTTON_RELEASE && event->time - start_time > delay))
  {
    gtk_popover_set_relative_to(GTK_POPOVER(popover), button);

    g_object_set(G_OBJECT(popover), "transitions-enabled", FALSE, NULL);

    _toolbar_show_popup(popover);
    return TRUE;
  }
  else
  {
    start_time = event->time;
    return FALSE;
  }
}

void connect_button_press_release(GtkWidget *w, GtkWidget *p)
{
  g_signal_connect(w, "button-press-event",
                   G_CALLBACK(_quickbutton_press_release), p);
  g_signal_connect(w, "button-release-event",
                   G_CALLBACK(_quickbutton_press_release), p);
}

void gui_init(dt_view_t *self)
{
  dt_develop_t *dev = self->data;
  dev->full.ppd = darktable.gui->ppd;
  dev->full.dpi = darktable.gui->dpi;
  dev->full.dpi_factor = darktable.gui->dpi_factor;
  dev->full.widget = dt_ui_center(darktable.gui->ui);

  dt_action_t *sa = &self->actions, *ac = NULL;

  /*
   * Add view specific tool buttons
   */

  /* create favorite plugin preset popup tool */
  GtkWidget *favorite_presets = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
  dt_action_define(sa, NULL, N_("quick access to presets"),
                   favorite_presets, &dt_action_def_button);
  gtk_widget_set_tooltip_text(favorite_presets, _("quick access to presets"));
  g_signal_connect(G_OBJECT(favorite_presets), "clicked",
                   G_CALLBACK(_darkroom_ui_favorite_presets_popupmenu),
                   NULL);
  dt_gui_add_help_link(favorite_presets, "favorite_presets");
  dt_view_manager_view_toolbox_add(darktable.view_manager,
                                   favorite_presets, DT_VIEW_DARKROOM);

  /* create quick styles popup menu tool */
  GtkWidget *styles = dtgtk_button_new(dtgtk_cairo_paint_styles, 0, NULL);
  dt_action_define(sa, NULL, N_("quick access to styles"), styles, &dt_action_def_button);
  g_signal_connect(G_OBJECT(styles), "clicked",
                   G_CALLBACK(_darkroom_ui_apply_style_popupmenu), NULL);
  gtk_widget_set_tooltip_text(styles, _("quick access for applying any of your styles"));
  dt_gui_add_help_link(styles, "bottom_panel_styles");
  dt_view_manager_view_toolbox_add(darktable.view_manager, styles, DT_VIEW_DARKROOM);
  /* ensure that we get strings from the style files shipped with darktable localized */

  /* create second window display button */
  dev->second_wnd_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_display2, 0, NULL);
  dt_action_define(sa, NULL, N_("second window"),
                   dev->second_wnd_button, &dt_action_def_toggle);
  g_signal_connect(G_OBJECT(dev->second_wnd_button), "clicked",
                   G_CALLBACK(_second_window_quickbutton_clicked),dev);
  gtk_widget_set_tooltip_text(dev->second_wnd_button,
                              _("display a second darkroom image window"));
  dt_view_manager_view_toolbox_add(darktable.view_manager,
                                   dev->second_wnd_button, DT_VIEW_DARKROOM);

  /* Register a toggle-pin action for the second window as a command so the
     shortcut works independently of the pin button widget.  The pin button
     is wired to this same action in _darkroom_ui_second_window_init() for
     right-click shortcut assignment and tooltip display. */
  dt_action_register(DT_ACTION(self), N_("toggle pinned state in second window"),
                     _toggle_pin_second_window_action, 0, 0);

  /* Enable color assessment conditions */
  {
    dev->color_assessment.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_bulb, 0, NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->color_assessment.button), dev->full.color_assessment);
    ac = dt_action_define(DT_ACTION(self), NULL, N_("color assessment"), dev->color_assessment.button,
                          &dt_action_def_toggle);
    gtk_widget_set_tooltip_text(dev->color_assessment.button, _("toggle color assessment conditions\nright-click for options"));
    dt_shortcut_register(ac, 0, 0, GDK_KEY_b, GDK_CONTROL_MASK);
    g_signal_connect(G_OBJECT(dev->color_assessment.button), "toggled",
                     G_CALLBACK(_full_color_assessment_callback), dev);

    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->color_assessment.button, DT_VIEW_DARKROOM);
    /* add pop-up window */
    dev->color_assessment.floating_window = gtk_popover_new(dev->color_assessment.button);
    connect_button_press_release(dev->color_assessment.button, dev->color_assessment.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->color_assessment.floating_window), vbox);

    /* total border width */
    GtkWidget *border_width_slider = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.05, 0.4, 0.05, 0.2, 2);
    dt_bauhaus_slider_set(border_width_slider, dt_conf_get_float("darkroom/ui/color_assessment_total_border_width"));
    dt_bauhaus_slider_set_format(border_width_slider, "%");
    dt_bauhaus_widget_set_label(border_width_slider, N_("color_assessment"), N_("total border width relative to screen"));
    gtk_widget_set_tooltip_text(border_width_slider,
                                _("total border width in relation to the screen size for the assessment mode.\n"
                                  "this includes the outer gray part plus the inner white frame."));
    g_signal_connect(G_OBJECT(border_width_slider), "value-changed",
                     G_CALLBACK(_color_assessment_border_width_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(border_width_slider), TRUE, TRUE, 0);

    /* white border ratio */
    GtkWidget *border_ratio_slider = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.1, 0.95, 0.05, 0.4, 2);
    dt_bauhaus_slider_set(border_ratio_slider,
                          dt_conf_get_float("darkroom/ui/color_assessment_border_white_ratio"));
    dt_bauhaus_slider_set_format(border_ratio_slider, "%");
    dt_bauhaus_widget_set_label(border_ratio_slider, N_("color_assessment"), N_("white border ratio"));
    gtk_widget_set_tooltip_text(border_ratio_slider,
                                _("the border ratio specifies the fraction of the white part of the border."));
    g_signal_connect(G_OBJECT(border_ratio_slider), "value-changed",
                     G_CALLBACK(_color_assessment_border_white_ratio_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(border_ratio_slider), TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
  }

  /* Enable late-scaling button */
  dev->late_scaling.button =
    dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_fullpreview, 0, NULL);
  ac = dt_action_define(sa, NULL, N_("high quality processing"),
                        dev->late_scaling.button, &dt_action_def_toggle);
  gtk_widget_set_tooltip_text
    (dev->late_scaling.button,
     _("toggle high quality processing."
       " if activated darktable processes image data as it does while exporting"));
  g_signal_connect(G_OBJECT(dev->late_scaling.button), "clicked",
                   G_CALLBACK(_latescaling_quickbutton_clicked), dev);
  dt_view_manager_module_toolbox_add(darktable.view_manager,
                                     dev->late_scaling.button, DT_VIEW_DARKROOM);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->late_scaling.button),
                               dt_conf_get_bool("darkroom/ui/late_scaling/enabled"));

  GtkWidget *colorscheme, *mode;

  /* create rawoverexposed popup tool */
  {
    // the button
    dev->rawoverexposed.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_rawoverexposed, 0, NULL);
    ac = dt_action_define(sa, N_("raw overexposed"), N_("toggle"),
                          dev->rawoverexposed.button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_o, GDK_SHIFT_MASK);
    gtk_widget_set_tooltip_text(dev->rawoverexposed.button,
                                _("toggle indication of raw overexposure\nright-click for options"));
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "clicked",
                     G_CALLBACK(_rawoverexposed_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager,
                                       dev->rawoverexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->rawoverexposed.button, "rawoverexposed");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->rawoverexposed.button),
                                 dt_conf_get_bool("darkroom/ui/rawoverexposed/enabled"));

    // and the popup window
    dev->rawoverexposed.floating_window = gtk_popover_new(dev->rawoverexposed.button);
    connect_button_press_release(dev->rawoverexposed.button,
                                 dev->rawoverexposed.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->rawoverexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* mode of operation */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(mode, self, N_("raw overexposed"),
                                 N_("mode"),
                                 _("select how to mark the clipped pixels"),
                                 dev->rawoverexposed.mode,
                                 _rawoverexposed_mode_callback, dev,
                                 N_("mark with CFA color"), N_("mark with solid color"),
                                 N_("false color"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

    DT_BAUHAUS_COMBOBOX_NEW_FULL(colorscheme, self,
                                 N_("raw overexposed"),
                                 N_("color scheme"),
                                _("select the solid color to indicate overexposure.\nwill only be used if mode = mark with solid color"),
                                dev->rawoverexposed.colorscheme,
                                _rawoverexposed_colorscheme_callback, dev,
                                NC_("solidcolor", "red"),
                                NC_("solidcolor", "green"),
                                NC_("solidcolor", "blue"),
                                NC_("solidcolor", "black"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);

    /* threshold */
    GtkWidget *threshold = dt_bauhaus_slider_new_action(DT_ACTION(self),
                                                        0.0, 2.0, 0.01, 1.0, 3);
    dt_bauhaus_slider_set(threshold, dev->rawoverexposed.threshold);
    dt_bauhaus_widget_set_label(threshold,
                                N_("raw overexposed"),
                                N_("clipping threshold"));
    gtk_widget_set_tooltip_text(
        threshold, _("threshold of what shall be considered overexposed\n1.0 - white level\n0.0 - black level"));
    g_signal_connect(G_OBJECT(threshold), "value-changed",
                     G_CALLBACK(_rawoverexposed_threshold_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(threshold), TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
  }

  /* create overexposed popup tool */
  {
    // the button
    dev->overexposed.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_overexposed, 0, NULL);
    ac = dt_action_define(DT_ACTION(self),
                          N_("overexposed"),
                          N_("toggle"),
                          dev->overexposed.button,
                          &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_o, 0);
    gtk_widget_set_tooltip_text(dev->overexposed.button,
                                _("toggle clipping indication\nright-click for options"));
    g_signal_connect(G_OBJECT(dev->overexposed.button), "clicked",
                     G_CALLBACK(_overexposed_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager,
                                       dev->overexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->overexposed.button, "overexposed");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->overexposed.button),
                                 dt_conf_get_bool("darkroom/ui/overexposed/enabled"));

    // and the popup window
    dev->overexposed.floating_window = gtk_popover_new(dev->overexposed.button);
    connect_button_press_release(dev->overexposed.button,
                                 dev->overexposed.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->overexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* preview mode */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(mode, self,
                                 N_("overexposed"),
                                 N_("clipping preview mode"),
                                 _("select the metric you want to preview\nfull gamut is the combination of all other modes"),
                                 dev->overexposed.mode,
                                 _mode_callback,
                                 dev,
                                 N_("full gamut"),
                                 N_("any RGB channel"),
                                 N_("luminance only"), N_("saturation only"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

    /* color scheme */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(colorscheme, self, N_("overexposed"),
                                 N_("color scheme"),
                                 _("select colors to indicate clipping"),
                                 dev->overexposed.colorscheme,
                                 _colorscheme_callback, dev,
                                 N_("black & white"),
                                 N_("red & blue"),
                                 N_("purple & green"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);

    /* lower */
    GtkWidget *lower = dt_bauhaus_slider_new_action(DT_ACTION(self),
                                                    -32., -4., 1., -12.69, 2);
    dt_bauhaus_slider_set(lower, dev->overexposed.lower);
    dt_bauhaus_slider_set_format(lower, _(" EV"));
    dt_bauhaus_widget_set_label(lower, N_("overexposed"), N_("lower threshold"));
    gtk_widget_set_tooltip_text(lower, _("clipping threshold for the black point,\n"
                                         "in EV, relatively to white (0 EV).\n"
                                         "8 bits sRGB clips blacks at -12.69 EV,\n"
                                         "8 bits Adobe RGB clips blacks at -19.79 EV,\n"
                                         "16 bits sRGB clips blacks at -20.69 EV,\n"
                                         "typical fine-art mat prints produce black at -5.30 EV,\n"
                                         "typical color glossy prints produce black at -8.00 EV,\n"
                                         "typical B&W glossy prints produce black at -9.00 EV."
                                         ));
    g_signal_connect(G_OBJECT(lower), "value-changed",
                     G_CALLBACK(_lower_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lower), TRUE, TRUE, 0);

    /* upper */
    GtkWidget *upper = dt_bauhaus_slider_new_action(DT_ACTION(self), 0.0, 100.0, 0.1, 99.99, 2);
    dt_bauhaus_slider_set(upper, dev->overexposed.upper);
    dt_bauhaus_slider_set_format(upper, "%");
    dt_bauhaus_widget_set_label(upper, N_("overexposed"), N_("upper threshold"));
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(upper, _("clipping threshold for the white point.\n"
                                         "100% is peak medium luminance."));
    g_signal_connect(G_OBJECT(upper), "value-changed", G_CALLBACK(_upper_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(upper), TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
  }

  /* create profile popup tool & buttons (softproof + gamut) */
  {
    // the softproof button
    dev->profile.softproof_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_softproof, 0, NULL);
    ac = dt_action_define(sa, NULL, N_("softproof"),
                          dev->profile.softproof_button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_s, GDK_CONTROL_MASK);
    gtk_widget_set_tooltip_text(dev->profile.softproof_button,
                                _("toggle softproofing\nright-click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "clicked",
                     G_CALLBACK(_softproof_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager,
                                       dev->profile.softproof_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.softproof_button, "softproof");

    // the gamut check button
    dev->profile.gamut_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_warning, 0, NULL);
    ac = dt_action_define(sa, NULL, N_("gamut check"),
                          dev->profile.gamut_button, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_g, GDK_CONTROL_MASK);
    gtk_widget_set_tooltip_text(dev->profile.gamut_button,
                 _("toggle gamut checking\nright-click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "clicked",
                     G_CALLBACK(_gamut_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager,
                                       dev->profile.gamut_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.gamut_button, "gamut");

    // and the popup window, which is shared between the two profile buttons
    dev->profile.floating_window = gtk_popover_new(NULL);
    connect_button_press_release(dev->second_wnd_button, dev->profile.floating_window);
    connect_button_press_release(dev->profile.softproof_button,
                                 dev->profile.floating_window);
    connect_button_press_release(dev->profile.gamut_button, dev->profile.floating_window);
    // randomly connect to one of the buttons, so widgets can be realized
    gtk_popover_set_relative_to(GTK_POPOVER(dev->profile.floating_window),
                                dev->second_wnd_button);

    /** let's fill the encapsulating widgets */
    const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

    static const gchar *intents_list[]
      = { N_("perceptual"),
          N_("relative colorimetric"),
          NC_("rendering intent", "saturation"),
          N_("absolute colorimetric"),
          NULL };

    GtkWidget *display_intent = dt_bauhaus_combobox_new_full(DT_ACTION(self),
                                                             N_("profiles"),
                                                             N_("intent"),
                                                             "",
                                                             0,
                                                             _display_intent_callback,
                                                             dev, intents_list);
    GtkWidget *display2_intent = dt_bauhaus_combobox_new_full(DT_ACTION(self),
                                                              N_("profiles"),
                                                              N_("preview intent"),
                                                              "",
                                                              0,
                                                              _display2_intent_callback,
                                                              dev, intents_list);

    if(!force_lcms2)
    {
      gtk_widget_set_no_show_all(display_intent, TRUE);
      gtk_widget_set_no_show_all(display2_intent, TRUE);
    }

    GtkWidget *display_profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
    GtkWidget *display2_profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
    GtkWidget *softproof_profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));
    GtkWidget *histogram_profile = dt_bauhaus_combobox_new_action(DT_ACTION(self));

    dt_bauhaus_widget_set_label(display_profile,
                                N_("profiles"), N_("display profile"));
    dt_bauhaus_widget_set_label(display2_profile,
                                N_("profiles"), N_("preview display profile"));
    dt_bauhaus_widget_set_label(softproof_profile,
                                N_("profiles"), N_("softproof profile"));
    dt_bauhaus_widget_set_label(histogram_profile,
                                N_("profiles"), N_("histogram profile"));

    dt_bauhaus_combobox_set_entries_ellipsis(display_profile, PANGO_ELLIPSIZE_MIDDLE);
    dt_bauhaus_combobox_set_entries_ellipsis(display2_profile, PANGO_ELLIPSIZE_MIDDLE);
    dt_bauhaus_combobox_set_entries_ellipsis(softproof_profile, PANGO_ELLIPSIZE_MIDDLE);
    dt_bauhaus_combobox_set_entries_ellipsis(histogram_profile, PANGO_ELLIPSIZE_MIDDLE);

    GtkWidget *display2_color_assessment = gtk_check_button_new_with_label(_("second preview window color assessment"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(display2_color_assessment),
                                 dev->preview2.color_assessment);
    ac = dt_action_define(DT_ACTION(self), NULL, N_("color assessment second preview"),
                          display2_color_assessment, &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_b, GDK_MOD1_MASK);

    for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
    {
      dt_colorspaces_color_profile_t *prof = l->data;
      if(prof->display_pos > -1)
      {
        dt_bauhaus_combobox_add(display_profile, prof->name);
        if(prof->type == darktable.color_profiles->display_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
        {
          dt_bauhaus_combobox_set(display_profile, prof->display_pos);
        }
      }
      if(prof->display2_pos > -1)
      {
        dt_bauhaus_combobox_add(display2_profile, prof->name);
        if(prof->type == darktable.color_profiles->display2_type
           && (prof->type != DT_COLORSPACE_FILE
               || !strcmp(prof->filename, darktable.color_profiles->display2_filename)))
        {
          dt_bauhaus_combobox_set(display2_profile, prof->display2_pos);
        }
      }
      // the system display profile is only suitable for display purposes
      if(prof->out_pos > -1)
      {
        dt_bauhaus_combobox_add(softproof_profile, prof->name);
        if(prof->type == darktable.color_profiles->softproof_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->softproof_filename)))
          dt_bauhaus_combobox_set(softproof_profile, prof->out_pos);
      }
      if(prof->category_pos > -1)
      {
        dt_bauhaus_combobox_add(histogram_profile, prof->name);
        if(prof->type == darktable.color_profiles->histogram_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->histogram_filename)))
        {
          dt_bauhaus_combobox_set(histogram_profile, prof->category_pos);
        }
      }
    }

    char *tooltip = dt_ioppr_get_location_tooltip("out", _("display ICC profiles"));
    gtk_widget_set_tooltip_markup(display_profile, tooltip);
    g_free(tooltip);

    tooltip = dt_ioppr_get_location_tooltip("out", _("preview display ICC profiles"));
    gtk_widget_set_tooltip_markup(display2_profile, tooltip);
    g_free(tooltip);

    tooltip = dt_ioppr_get_location_tooltip("out", _("softproof ICC profiles"));
    gtk_widget_set_tooltip_markup(softproof_profile, tooltip);
    g_free(tooltip);

    tooltip = dt_ioppr_get_location_tooltip("out",
                                            _("histogram and color picker ICC profiles"));
    gtk_widget_set_tooltip_markup(histogram_profile, tooltip);
    g_free(tooltip);

    g_signal_connect(G_OBJECT(display_profile), "value-changed",
                     G_CALLBACK(_display_profile_callback), dev);
    g_signal_connect(G_OBJECT(display2_profile), "value-changed",
                     G_CALLBACK(_display2_profile_callback), dev);
    g_signal_connect(G_OBJECT(display2_color_assessment), "toggled",
                     G_CALLBACK(_display2_color_assessment_callback), dev);
    g_signal_connect(G_OBJECT(softproof_profile), "value-changed",
                     G_CALLBACK(_softproof_profile_callback), dev);
    g_signal_connect(G_OBJECT(histogram_profile), "value-changed",
                     G_CALLBACK(_histogram_profile_callback), dev);

    _update_softproof_gamut_checking(dev);

    // update the gui when the preferences changed (i.e. show intent when using lcms2)
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE,
                              _preference_changed, display_intent);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE,
                              _preference_changed, display2_intent);
    // and when profiles change
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                              _display_profile_changed, display_profile);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                              _display2_profile_changed, display2_profile);

    GtkWidget *vbox = dt_gui_vbox
      (display_profile, display_intent,
       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
       display2_profile, display2_intent, display2_color_assessment,
       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
       softproof_profile, histogram_profile);

    gtk_widget_show_all(vbox);
    gtk_container_add(GTK_CONTAINER(dev->profile.floating_window), vbox);
  }

  /* create grid changer popup tool */
  {
    // the button
    darktable.view_manager->guides_toggle =
      dtgtk_togglebutton_new(dtgtk_cairo_paint_grid, 0, NULL);
    ac = dt_action_define(sa, N_("guide lines"), N_("toggle"),
                          darktable.view_manager->guides_toggle,
                          &dt_action_def_toggle);
    dt_shortcut_register(ac, 0, 0, GDK_KEY_g, 0);
    gtk_widget_set_tooltip_text(darktable.view_manager->guides_toggle,
                                _("toggle guide lines\nright-click for guides options"));
    darktable.view_manager->guides_popover =
      dt_guides_popover(self, darktable.view_manager->guides_toggle);
    g_object_ref(darktable.view_manager->guides_popover);
    g_signal_connect(G_OBJECT(darktable.view_manager->guides_toggle), "clicked",
                     G_CALLBACK(_guides_quickbutton_clicked), dev);
    connect_button_press_release(darktable.view_manager->guides_toggle,
                                 darktable.view_manager->guides_popover);
    dt_view_manager_module_toolbox_add(darktable.view_manager,
                                       darktable.view_manager->guides_toggle,
                                       DT_VIEW_DARKROOM | DT_VIEW_TETHERING);
    // we want to update button state each time the view change
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, _guides_view_changed);
  }

  darktable.view_manager->proxy.darkroom.get_layout = _lib_darkroom_get_layout;
  dev->full.border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));

  // Fullscreen preview key
  ac = dt_action_define(sa, NULL, N_("full preview"), NULL, &dt_action_def_preview);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_HOLD, GDK_KEY_w, 0);

  // add an option to allow skip mouse events while other overlays are
  // consuming mouse actions
  ac = dt_action_define(sa, NULL, N_("force pan/zoom/rotate with mouse"),
                        NULL, &dt_action_def_skip_mouse);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_HOLD, GDK_KEY_a, 0);

  // move left/right/up/down
  ac = dt_action_define(sa, N_("move"), N_("horizontal"), GINT_TO_POINTER(1),
                        &_action_def_move);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_DOWN, GDK_KEY_Left , 0);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_UP  , GDK_KEY_Right, 0);
  ac = dt_action_define(sa, N_("move"), N_("vertical"), GINT_TO_POINTER(0),
                        &_action_def_move);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_DOWN, GDK_KEY_Down , 0);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_UP  , GDK_KEY_Up   , 0);

  // Zoom shortcuts
  dt_action_register(DT_ACTION(self), N_("zoom close-up"), zoom_key_accel,
                     GDK_KEY_1, GDK_MOD1_MASK);

  // zoom in/out
  dt_action_register(DT_ACTION(self), N_("zoom in"), zoom_in_callback,
                     GDK_KEY_plus, GDK_CONTROL_MASK);
  dt_action_register(DT_ACTION(self), N_("zoom out"), zoom_out_callback,
                     GDK_KEY_minus, GDK_CONTROL_MASK);

  // Shortcut to skip images
  dt_action_register(DT_ACTION(self), N_("image forward"), skip_f_key_accel_callback,
                     GDK_KEY_space, 0);
  dt_action_register(DT_ACTION(self), N_("image back"), skip_b_key_accel_callback,
                     GDK_KEY_BackSpace, 0);

  // cycle overlay colors
  dt_action_register(DT_ACTION(self), N_("cycle overlay colors"),
                     _overlay_cycle_callback, GDK_KEY_o, GDK_CONTROL_MASK);

  // toggle visibility of drawn masks for current gui module
  dt_action_register(DT_ACTION(self), N_("show drawn masks"),
                     _toggle_mask_visibility_callback, 0, 0);

  // brush size +/-
  dt_action_register(DT_ACTION(self), N_("increase brush size"),
                     _brush_size_up_callback, 0, 0);
  dt_action_register(DT_ACTION(self), N_("decrease brush size"),
                     _brush_size_down_callback, 0, 0);

  // brush hardness +/-
  dt_action_register(DT_ACTION(self), N_("increase brush hardness"),
                     _brush_hardness_up_callback, GDK_KEY_braceright, 0);
  dt_action_register(DT_ACTION(self), N_("decrease brush hardness"),
                     _brush_hardness_down_callback, GDK_KEY_braceleft, 0);

  // brush opacity +/-
  dt_action_register(DT_ACTION(self), N_("increase brush opacity"),
                     _brush_opacity_up_callback, GDK_KEY_greater, 0);
  dt_action_register(DT_ACTION(self), N_("decrease brush opacity"),
                     _brush_opacity_down_callback, GDK_KEY_less, 0);

  // undo/redo
  dt_action_register(DT_ACTION(self), N_("undo"), _darkroom_undo_callback,
                     GDK_KEY_z, GDK_CONTROL_MASK);
  dt_action_register(DT_ACTION(self), N_("redo"), _darkroom_redo_callback,
                     GDK_KEY_y, GDK_CONTROL_MASK);

  // change the precision for adjusting sliders with keyboard shortcuts
  dt_action_register(DT_ACTION(self), N_("change keyboard shortcut slider precision"),
                     _change_slider_accel_precision, 0, 0);

  dt_action_register(DT_ACTION(self), N_("synchronize selection"),
                     _darkroom_do_synchronize_selection_callback,
                     GDK_KEY_x, GDK_CONTROL_MASK);
}

void enter(dt_view_t *self)
{
  // prevent accels_window to refresh
  darktable.view_manager->accels_window.prevent_refresh = TRUE;

  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);

  /* connect to ui pipe finished signal for redraw */
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                           _darkroom_ui_pipe_finish_signal_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED,
                           _darkroom_ui_preview2_pipe_finish_signal_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_TROUBLE_MESSAGE,
                           _display_module_trouble_message_callback);

  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode", dt_get_wtime());
  dt_develop_t *dev = self->data;
  
  // Reset shutdown flags on all pipes - they may still be set from previous session
  if(dev->full.pipe)
    dt_atomic_set_int(&dev->full.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);
  if(dev->preview_pipe)
    dt_atomic_set_int(&dev->preview_pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);
  if(dev->preview2.pipe)
    dt_atomic_set_int(&dev->preview2.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);
  
  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }
  dt_masks_change_form_gui(NULL);
  dev->form_gui->pipe_hash = DT_INVALID_HASH;
  dev->form_gui->formid = NO_MASKID;
  dev->gui_leaving = FALSE;
  dev->gui_module = NULL;

  // change active image
  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(dev->image_storage.id, TRUE);
  dt_ui_thumbtable(darktable.gui->ui)->mouse_inside = FALSE; // consider mouse outside filmstrip by default

  dt_dev_zoom_move(&dev->full, DT_ZOOM_FIT, 0.0f, 0, -1.0f, -1.0f, TRUE);

  // take a copy of the image struct for convenience.

  dt_dev_load_image(darktable.develop, dev->image_storage.id);


  /*
   * add IOP modules to plugin list
   */
  GtkWidget *box = GTK_WIDGET(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER));
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_widget_get_ancestor(box, GTK_TYPE_SCROLLED_WINDOW));
  if(sw) gtk_scrolled_window_set_propagate_natural_width(sw, FALSE);

  char option[1024];

  for(const GList *modules = g_list_last(dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = modules->data;

    /* initialize gui if iop have one defined */
    if(!dt_iop_is_hidden(module))
    {
      dt_iop_gui_init(module);

      /* add module to right panel */
      dt_iop_gui_set_expander(module);

      if(module->multi_priority == 0)
      {
        snprintf(option, sizeof(option), "plugins/darkroom/%s/expanded", module->op);
        module->expanded = dt_conf_get_bool(option);
        dt_iop_gui_update_expanded(module);
      }

      dt_iop_reload_defaults(module);
    }
  }

  /* signal that darktable.develop is initialized and ready to be used */
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_INITIALIZE);
  /* signal that there is a new image to be developed */
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED);

  // synch gui and flag pipe as dirty
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  dt_dev_pop_history_items(dev, dev->history_end);

  /* ensure that filmstrip shows current image */
  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui),
                                 dev->image_storage.id, TRUE);

  // get last active plugin:
  const char *active_plugin = dt_conf_get_string_const("plugins/darkroom/active");
  if(active_plugin)
  {
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = modules->data;
      if(dt_iop_module_is(module->so, active_plugin))
        dt_iop_request_focus(module);
    }
  }

  // image should be there now.
  dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, -1.f, TRUE, 0.0f, 0.0f, TRUE);

  /* connect signal for filmstrip image activate */
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                           _view_darkroom_filmstrip_activate_callback);
  dt_collection_hint_message(darktable.collection);

  dt_ui_scrollbars_show(darktable.gui->ui, dt_conf_get_bool("darkroom/ui/scrollbars"));

  if(dt_conf_get_bool("second_window/last_visible"))
  {
    _darkroom_display_second_window(dev);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_wnd_button), TRUE);
  }

  // just make sure at this stage we have only history info into the undo, all automatic
  // tagging should be ignored.
  dt_undo_clear(darktable.undo, DT_UNDO_TAGS);

  // update accels_window
  darktable.view_manager->accels_window.prevent_refresh = FALSE;

  //connect iop accelerators
  dt_iop_connect_accels_all();

  // switch on groups as they were last time:
  dt_dev_modulegroups_set(dev, dt_conf_get_int("plugins/darkroom/groups"));

  // connect to preference change for module header button hiding
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_PREFERENCES_CHANGE, _preference_changed_button_hide);
  dt_iop_color_picker_init();

  dt_image_check_camera_missing_sample(&dev->image_storage);

#ifdef USE_LUA

  _fire_darkroom_image_loaded_event(TRUE, dev->image_storage.id);

#endif

}

void leave(dt_view_t *self)
{
  dt_iop_color_picker_cleanup();
  if(darktable.lib->proxy.colorpicker.picker_proxy)
    dt_iop_color_picker_reset(darktable.lib->proxy.colorpicker.picker_proxy->module, FALSE);

  DT_CONTROL_SIGNAL_DISCONNECT_ALL(self, "darkroom");

  // store groups for next time:
  dt_conf_set_int("plugins/darkroom/groups", dt_dev_modulegroups_get(darktable.develop));

  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");

  dt_develop_t *dev = self->data;

  // Close second window when leaving darkroom (save state first)
  if(dev->second_wnd)
  {
    GtkWidget *wnd = dev->second_wnd;
    
    if(gtk_widget_is_visible(wnd))
    {
      dt_conf_set_bool("second_window/last_visible", TRUE);
      _darkroom_ui_second_window_write_config(wnd);
    }
    
    _darkroom_ui_second_window_cleanup(dev);
    gtk_widget_hide(wnd);
    gtk_widget_destroy(wnd);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_wnd_button), FALSE);
  }

  // reset color assessment mode
  if(dev->full.color_assessment)
  {
    dev->full.width = dev->full.orig_width;
    dev->full.height = dev->full.orig_height;
    dev->preview2.width = dev->preview2.orig_width;
    dev->preview2.height = dev->preview2.orig_height;
    dev->full.border_size =
      DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  }

  // commit image ops to db
  dt_dev_write_history(dev);

  const dt_imgid_t imgid = dev->image_storage.id;

  dt_overlay_add_from_history(imgid);

  // update aspect ratio
  if(dev->preview_pipe->backbuf && dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID)
  {
    const float aspect_ratio =
      (float)dev->preview_pipe->backbuf_width / (float)dev->preview_pipe->backbuf_height;
    dt_image_set_aspect_ratio_to(dev->preview_pipe->image.id, aspect_ratio, FALSE);
  }
  else
  {
    dt_image_set_aspect_ratio(imgid, FALSE);
  }

  // be sure light table will regenerate the thumbnail:
  if(!dt_history_hash_is_mipmap_synced(imgid))
  {
    dt_mipmap_cache_remove(imgid);
    dt_image_update_final_size(imgid);
    dt_image_synch_xmp(imgid);
    dt_history_hash_set_mipmap(imgid);
#ifdef USE_LUA
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
        0, NULL, NULL,
        LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-history-changed",
        LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(imgid),
        LUA_ASYNC_DONE);
#endif
    // update the lighttable metadata_view with any changes
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_METADATA_CHANGED);
  }
  else
    dt_image_synch_xmp(imgid);


  // clear gui.

  dt_pthread_mutex_lock(&dev->preview_pipe->mutex);
  dt_pthread_mutex_lock(&dev->preview2.pipe->mutex);
  dt_pthread_mutex_lock(&dev->full.pipe->mutex);

  dev->gui_leaving = TRUE;

  dt_pthread_mutex_lock(&dev->history_mutex);

  dt_dev_pixelpipe_cleanup_nodes(dev->full.pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview2.pipe);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);

  while(dev->history)
  {
    dt_dev_history_item_t *hist = dev->history->data;
    // printf("removing history item %d - %s, data %f %f\n", hist->module->instance, hist->module->op, *(float
    // *)hist->params, *((float *)hist->params+1));
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }

  while(dev->iop)
  {
    dt_iop_module_t *module = dev->iop->data;
    if(!dt_iop_is_hidden(module)) dt_iop_gui_cleanup_module(module);

    // force refresh if module has mask visualized
    if(module->request_mask_display || module->suppress_mask) dt_iop_refresh_center(module);

    dt_action_cleanup_instance_iop(module);
    dt_iop_cleanup_module(module);
    free(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }

  GtkWidget *box =
    GTK_WIDGET(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER));
  GtkScrolledWindow *sw =
    GTK_SCROLLED_WINDOW(gtk_widget_get_ancestor(box, GTK_TYPE_SCROLLED_WINDOW));
  if(sw) gtk_scrolled_window_set_propagate_natural_width(sw, TRUE);

  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_pthread_mutex_unlock(&dev->full.pipe->mutex);
  dt_pthread_mutex_unlock(&dev->preview2.pipe->mutex);
  dt_pthread_mutex_unlock(&dev->preview_pipe->mutex);

  // cleanup visible masks
  if(dev->form_gui)
  {
    dev->gui_module = NULL; // modules have already been free()
    dt_masks_clear_form_gui(dev);
    free(dev->form_gui);
    dev->form_gui = NULL;
    dt_masks_change_form_gui(NULL);
  }
  // clear masks
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);
  dev->allforms = NULL;

  gtk_widget_hide(dev->overexposed.floating_window);
  gtk_widget_hide(dev->rawoverexposed.floating_window);
  gtk_widget_hide(dev->profile.floating_window);
  gtk_widget_hide(dev->color_assessment.floating_window);

  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);

  // darkroom development could have changed a collection, so update that before being back in lighttable
  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_prepend(NULL, GINT_TO_POINTER(darktable.develop->image_storage.id)));

  darktable.develop->image_storage.id = NO_IMGID;

  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show
  // metadata of opened image.
  dt_develop_t *dev = self->data;
  dt_control_set_mouse_over_id(dev->image_storage.id);

  dev->darkroom_mouse_in_center_area = FALSE;
  // masks
  int handled = dt_masks_events_mouse_leave(dev->gui_module);
  if(handled) return;
  // module
  if(dev->gui_module && dev->gui_module->mouse_leave)
    handled = dev->gui_module->mouse_leave(dev->gui_module);

  // reset any changes the selected plugin might have made.
  dt_control_change_cursor("default");
}

void mouse_enter(dt_view_t *self)
{
  dt_develop_t *dev = self->data;
  // masks
  dev->darkroom_mouse_in_center_area = TRUE;
  dt_masks_events_mouse_enter(dev->gui_module);
}

void mouse_moved(dt_view_t *self,
                 const double x,
                 const double y,
                 const double pressure,
                 const int which)
{
  dt_develop_t *dev = self->data;

  // if we are not hovering over a thumbnail in the filmstrip -> show
  // metadata of opened image.
  dt_imgid_t mouse_over_id = dt_control_get_mouse_over_id();
  if(!dt_is_valid_imgid(mouse_over_id))
  {
    mouse_over_id = dev->image_storage.id;
    dt_control_set_mouse_over_id(mouse_over_id);
  }

  dt_control_t *ctl = darktable.control;
  int handled = 0;

  float zoom_x = FLT_MAX, zoom_y, zoom_scale;

  if(!darktable.develop->darkroom_skip_mouse_events
     && dt_iop_color_picker_is_visible(dev)
     && ctl->button_down && ctl->button_down_which == 1)
  {
    // module requested a color box
    dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;
    // Make sure a minimal width/height
    const float delta_x = 1.0f / (float) dev->full.pipe->processed_width;
    const float delta_y = 1.0f / (float) dev->full.pipe->processed_height;

    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    dt_boundingbox_t pbox = { zoom_x, zoom_y };

    if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      dt_pickerpoint_t corner;
      dt_color_picker_transform_box(dev, 1, sample->point, corner, TRUE);

      pbox[0] = MAX(0.0, MIN(corner[0], zoom_x) - delta_x);
      pbox[1] = MAX(0.0, MIN(corner[1], zoom_y) - delta_y);
      pbox[2] = MIN(1.0, MAX(corner[0], zoom_x) + delta_x);
      pbox[3] = MIN(1.0, MAX(corner[1], zoom_y) + delta_y);
      dt_color_picker_backtransform_box(dev, 2, pbox, sample->box);
    }
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      dt_color_picker_backtransform_box(dev, 1, pbox, sample->point);
    }
    dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
    dt_control_queue_redraw_center();
    handled = TRUE;
  }

  // masks
  if(dev->form_visible
     && !handled
     && !darktable.develop->darkroom_skip_mouse_events
     && !dt_iop_color_picker_is_visible(dev))
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dt_masks_events_mouse_moved(dev->gui_module, zoom_x, zoom_y,
                                          pressure, which, zoom_scale);
  }

  // module
  if(dev->gui_module && dev->gui_module->mouse_moved
     && !handled
     && !darktable.develop->darkroom_skip_mouse_events
     && !dt_iop_color_picker_is_visible(dev)
     && dt_dev_modulegroups_test_activated(darktable.develop))
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dev->gui_module->mouse_moved(dev->gui_module, zoom_x, zoom_y,
                                           pressure, which, zoom_scale);
  }

  if(ctl->button_down && ctl->button_down_which == GDK_BUTTON_PRIMARY)
  {
    if(!handled)
      dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, -1.f, 0,
                       x - ctl->button_x, y - ctl->button_y, TRUE);
    else
    {
      const int32_t bs = dev->full.border_size;
      const float dx = MIN(0, x - bs) + MAX(0, x - dev->full.width  - bs);
      const float dy = MIN(0, y - bs) + MAX(0, y - dev->full.height - bs);
      if(fabsf(dx) + fabsf(dy) > 0.5f)
        dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, 1.f, 0, dx, dy, TRUE);
    }
    ctl->button_x = x;
    ctl->button_y = y;
  }
  else if(darktable.control->button_down
          && !handled
          && darktable.control->button_down_which == GDK_BUTTON_SECONDARY
          && dev->proxy.rotate)
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    dev->proxy.rotate->mouse_moved(dev->proxy.rotate, zoom_x, zoom_y,
                                   pressure, which, zoom_scale);
  }
}


int button_released(dt_view_t *self,
                    const double x,
                    const double y,
                    const int which,
                    const uint32_t state)
{
  dt_develop_t *dev = darktable.develop;

  if(darktable.develop->darkroom_skip_mouse_events && which == GDK_BUTTON_PRIMARY)
  {
    dt_control_change_cursor("default");
    return 1;
  }

  int handled = 0;
  if(dt_iop_color_picker_is_visible(dev) && which == GDK_BUTTON_PRIMARY)
  {
    // only sample box picker at end, for speed
    if(darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      dt_control_queue_redraw_center();
      dt_control_change_cursor("default");
    }
    return 1;
  }

  float zoom_x = FLT_MAX, zoom_y, zoom_scale;
  // rotate
  if(which == GDK_BUTTON_SECONDARY && dev->proxy.rotate)
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dev->proxy.rotate->button_released(dev->proxy.rotate, zoom_x, zoom_y,
                                                 which, state, zoom_scale);
    if(handled) return handled;
  }
  // masks
  if(dev->form_visible)
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dt_masks_events_button_released(dev->gui_module, zoom_x, zoom_y,
                                              which, state, zoom_scale);
    if(handled) return handled;
  }
  // module
  if(dev->gui_module && dev->gui_module->button_released
     && dt_dev_modulegroups_test_activated(darktable.develop))
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dev->gui_module->button_released(dev->gui_module, zoom_x, zoom_y,
                                               which, state, zoom_scale);
    if(handled) return handled;
  }
  if(which == GDK_BUTTON_PRIMARY) dt_control_change_cursor("default");

  return 1;
}


int button_pressed(dt_view_t *self,
                   double x,
                   double y,
                   double pressure,
                   const int which,
                   const int type,
                   const uint32_t state)
{
  dt_develop_t *dev = self->data;
  dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;

  float zoom_x = FLT_MAX, zoom_y, zoom_scale;

  if(darktable.develop->darkroom_skip_mouse_events)
  {
    if(which == GDK_BUTTON_PRIMARY)
    {
      if(type == GDK_2BUTTON_PRESS) return 0;
      dt_control_change_cursor("pointer");
      return 1;
    }
    else if(which == GDK_BUTTON_SECONDARY && dev->proxy.rotate)
    {
      _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
      return dev->proxy.rotate->button_pressed(dev->proxy.rotate, zoom_x, zoom_y, pressure,
                                               which, type, state, zoom_scale);
    }
  }

  int handled = 0;
  if(dt_iop_color_picker_is_visible(dev))
  {
    const int procw = dev->preview_pipe->backbuf_width;
    const int proch = dev->preview_pipe->backbuf_height;

    // For a Ctrl+Click we do change the color picker from/to area <-> point
    if(which == GDK_BUTTON_PRIMARY
       && dt_modifier_is(state, GDK_CONTROL_MASK))
    {
      if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      {
        // dt_lib_colorpicker_reset_box_area(sample->box);
        dt_lib_colorpicker_set_box_area(darktable.lib, sample->box);
      }
      else if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        dt_lib_colorpicker_set_point(darktable.lib, sample->point);
      }

      dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      dt_control_queue_redraw_center();

      return 1;
    }

    if(which == GDK_BUTTON_PRIMARY)
    {
      _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
      sample->point[0] = zoom_x;
      sample->point[1] = zoom_y;

      if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        dt_boundingbox_t sbox;
        dt_color_picker_transform_box(dev, 2, sample->box, sbox, TRUE);

        const float handle_px = 6.0f;
        const float hx = handle_px / (procw * zoom_scale);
        const float hy = handle_px / (proch * zoom_scale);

        const float dx0 = fabsf(zoom_x - sbox[0]);
        const float dx1 = fabsf(zoom_x - sbox[2]);
        const float dy0 = fabsf(zoom_y - sbox[1]);
        const float dy1 = fabsf(zoom_y - sbox[3]);

        if(MIN(dx0, dx1) < hx && MIN(dy0, dy1) < hy)
        {
          sample->point[0] = sbox[dx0 < dx1 ? 2 : 0];
          sample->point[1] = sbox[dy0 < dy1 ? 3 : 1];
        }
        else
        {
          const float dx = 0.02f;
          const float dy = dx * (float)dev->full.pipe->processed_width / (float)dev->full.pipe->processed_height;
          const dt_boundingbox_t fbox = { zoom_x - dx,
                                          zoom_y - dy,
                                          zoom_x + dx,
                                          zoom_y + dy };
          dt_color_picker_backtransform_box(dev, 2, fbox, sample->box);
        }
        dt_control_change_cursor("move");
      }

      dt_color_picker_backtransform_box(dev, 1, sample->point, sample->point);
      dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      dt_control_queue_redraw_center();
      return 1;
    }

    if(which == GDK_BUTTON_SECONDARY)
    {
      // apply a live sample's area to the active picker?
      // FIXME: this is a naive implementation, nicer would be to cycle through overlapping samples then reset
      dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;
      if(darktable.lib->proxy.colorpicker.display_samples)
      {
        _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
        for(GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
            samples;
            samples = g_slist_next(samples))
        {
          dt_colorpicker_sample_t *live_sample = samples->data;
          dt_boundingbox_t sbox;
          if(live_sample->size == DT_LIB_COLORPICKER_SIZE_BOX
             && (picker->flags & DT_COLOR_PICKER_AREA))
          {
            dt_color_picker_transform_box(dev, 2, live_sample->box, sbox, TRUE);
            if(zoom_x < sbox[0] || zoom_x > sbox[2] ||
               zoom_y < sbox[1] || zoom_y > sbox[3])
              continue;
            dt_lib_colorpicker_set_box_area(darktable.lib, live_sample->box);
          }
          else if(live_sample->size == DT_LIB_COLORPICKER_SIZE_POINT
                  && (picker->flags & DT_COLOR_PICKER_POINT))
          {
            // magic values derived from _darkroom_pickers_draw
            float slop_px = MAX(26.0f, roundf(3.0f * zoom_scale));
            const float slop_x = slop_px / (procw * zoom_scale);
            const float slop_y = slop_px / (proch * zoom_scale);
            dt_color_picker_transform_box(dev, 1, live_sample->point, sbox, TRUE);
            if(!feqf(zoom_x, sbox[0], slop_x) || !feqf(zoom_y, sbox[1], slop_y))
              continue;
            dt_lib_colorpicker_set_point(darktable.lib, live_sample->point);
          }
          else
            continue;
          dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
          dt_control_queue_redraw_center();
          return 1;
        }
      }
      if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        dt_pickerbox_t box;
        dt_lib_colorpicker_reset_box_area(box);
        dt_lib_colorpicker_set_box_area(darktable.lib, box);
        dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
        dt_control_queue_redraw_center();
      }

      return 1;
    }
  }

  // masks
  if(dev->form_visible)
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dt_masks_events_button_pressed(dev->gui_module, zoom_x, zoom_y,
                                             pressure, which, type, state);
    if(handled) return handled;
  }
  // module
  if(dev->gui_module && dev->gui_module->button_pressed
     && dt_dev_modulegroups_test_activated(darktable.develop))
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dev->gui_module->button_pressed(dev->gui_module, zoom_x, zoom_y,
                                              pressure, which, type, state, zoom_scale);
    if(handled) return handled;
  }

  if(which == GDK_BUTTON_PRIMARY && type == GDK_2BUTTON_PRESS) return 0;
  if(which == GDK_BUTTON_PRIMARY)
  {
    dt_control_change_cursor("pointer");
    return 1;
  }

  if(which == GDK_BUTTON_MIDDLE  && type == GDK_BUTTON_PRESS) // Middle mouse button
    dt_dev_zoom_move(&dev->full, DT_ZOOM_1, 0.0f, -2, x, y,
                     !dt_modifier_is(state, GDK_CONTROL_MASK));
  if(which == GDK_BUTTON_SECONDARY && dev->proxy.rotate)
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    return dev->proxy.rotate->button_pressed(dev->proxy.rotate, zoom_x, zoom_y,
                                             pressure, which, type, state, zoom_scale);
  }
  return 0;
}

void scrollbar_changed(dt_view_t *self,
                       const double x,
                       const double y)
{
  dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_POSITION, 0.0f, 0, x, y, TRUE);
}

void scrolled(dt_view_t *self,
              const double x,
              const double y,
              const int up,
              const int state)
{
  dt_develop_t *dev = self->data;

  float zoom_x = FLT_MAX, zoom_y, zoom_scale;
  int handled = 0;

  // masks
  if(dev->form_visible
     && !darktable.develop->darkroom_skip_mouse_events)
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dt_masks_events_mouse_scrolled(dev->gui_module, zoom_x, zoom_y, up, state);
    if(handled) return;
  }

  // module
  if(dev->gui_module && dev->gui_module->scrolled
     && !darktable.develop->darkroom_skip_mouse_events
     && !dt_iop_color_picker_is_visible(dev)
     && dt_dev_modulegroups_test_activated(darktable.develop))
  {
    _get_zoom_pos(&dev->full, x, y, &zoom_x, &zoom_y, &zoom_scale);
    handled = dev->gui_module->scrolled(dev->gui_module, zoom_x, zoom_y, up, state);
    if(handled) return;
  }

  // free zoom
  const gboolean constrained = !dt_modifier_is(state, GDK_CONTROL_MASK);
  dt_dev_zoom_move(&dev->full, DT_ZOOM_SCROLL, 0.0f, up, x, y, constrained);
}

gboolean gesture_pan(dt_view_t *self,
                     const double x,
                     const double y,
                     const double dx,
                     const double dy,
                     const int state)
{
  dt_develop_t *dev = self->data;
  (void)x;
  (void)y;
  (void)state;
  if(!dev) return FALSE;

  // Mask editing (brush etc.) uses scroll for tool parameters.
  if(dev->form_visible
     && !darktable.develop->darkroom_skip_mouse_events)
    return FALSE;

  // Let active modules consume scroll for their own interactions (e.g. brush size).
  if(dev->gui_module && dev->gui_module->scrolled
     && !darktable.develop->darkroom_skip_mouse_events
     && !dt_iop_color_picker_is_visible(dev)
     && dt_dev_modulegroups_test_activated(darktable.develop))
    return FALSE;

  if(dx == 0.0 && dy == 0.0) return FALSE;

  dt_dev_zoom_move(&dev->full, DT_ZOOM_MOVE, 1.0f, 0, dx, dy, TRUE);
  return TRUE;
}

gboolean gesture_pinch(dt_view_t *self,
                       const double x,
                       const double y,
                       const int phase,
                       const double scale,
                       const int state)
{
  dt_develop_t *dev = self->data;
  if(!dev) return FALSE;
  const gboolean constrained = !dt_modifier_is(state, GDK_CONTROL_MASK);
  const double pinch_step_ratio = 1.1;

  static double pinch_last_scale = 0.0;

  if(phase == GDK_TOUCHPAD_GESTURE_PHASE_BEGIN)
  {
    pinch_last_scale = scale > 0.0 ? scale : 1.0;
    return TRUE;
  }
  else if(phase == GDK_TOUCHPAD_GESTURE_PHASE_END
          || phase == GDK_TOUCHPAD_GESTURE_PHASE_CANCEL)
  {
    pinch_last_scale = 0.0;
    return TRUE;
  }

  if(phase != GDK_TOUCHPAD_GESTURE_PHASE_UPDATE) return FALSE;
  if(pinch_last_scale <= 0.0 || scale <= 0.0) return FALSE;

  const double ratio = scale / pinch_last_scale;
  int zoom_step = -1;
  if(ratio > pinch_step_ratio)
    zoom_step = 1;
  else if(ratio < 1.0 / pinch_step_ratio)
    zoom_step = 0;

  if(zoom_step >= 0)
  {
    dt_dev_zoom_move(&dev->full, DT_ZOOM_SCROLL, 0.0f, zoom_step, x, y, constrained);
    pinch_last_scale = scale;
  }

  return TRUE;
}

static void _change_slider_accel_precision(dt_action_t *action)
{
  const int curr_precision = dt_conf_get_int("accel/slider_precision");
  const int new_precision = curr_precision + 1 == 3 ? 0 : curr_precision + 1;
  dt_conf_set_int("accel/slider_precision", new_precision);

  if(new_precision == DT_IOP_PRECISION_FINE)
    dt_toast_log(_("keyboard shortcut slider precision: fine"));
  else if(new_precision == DT_IOP_PRECISION_NORMAL)
    dt_toast_log(_("keyboard shortcut slider precision: normal"));
  else
    dt_toast_log(_("keyboard shortcut slider precision: coarse"));
}

void configure(dt_view_t *self, int wd, int ht)
{
  dt_develop_t *dev = self->data;
  dev->full.orig_width = wd;
  dev->full.orig_height = ht;
  dt_dev_configure(&dev->full);
}

GSList *mouse_actions(const dt_view_t *self)
{
  GSList *lm = NULL;
  GSList *lm2 = NULL;
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DOUBLE_LEFT,
                                     0, _("switch to lighttable"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL,
                                     0, _("zoom in the image"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL,
                                     GDK_CONTROL_MASK, _("unbounded zoom in the image"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_MIDDLE,
                                     0, _("zoom to 100% 200% and back"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT_DRAG,
                                     0, _("pan a zoomed image"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_SHIFT_MASK,
                                     dt_conf_get_bool("darkroom/ui/single_module")
                                     ? _("[modules] expand module without closing others")
                                     : _("[modules] expand module and close others"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_CONTROL_MASK,
                                     _("[modules] rename module"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DRAG_DROP,
                                     GDK_SHIFT_MASK | GDK_CONTROL_MASK,
                                     _("[modules] change module position in pipe"));

  const dt_develop_t *dev = self->data;
  if(dev->form_visible)
  {
    // masks
    lm2 = dt_masks_mouse_actions(dev->form_visible);
  }
  else if(dev->gui_module && dev->gui_module->mouse_actions)
  {
    // modules with on canvas actions
    lm2 = dev->gui_module->mouse_actions(dev->gui_module);
  }

  return g_slist_concat(lm, lm2);
}

//-----------------------------------------------------------
// second darkroom window
//-----------------------------------------------------------

/* helper macro that applies the DPI transformation to fixed pixel values. input should be defaulting to 96
 * DPI */
#define DT_PIXEL_APPLY_DPI_2ND_WND(dev, value) ((value) * dev->preview2.dpi_factor)

static void _dt_second_window_change_cursor(dt_develop_t *dev,
                                            const gchar *curs)
{
  GtkWidget *widget = dev->second_wnd;
  GdkCursor *cursor = gdk_cursor_new_from_name(gdk_display_get_default(), curs);
  gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
  g_object_unref(cursor);
}

static void _second_window_leave(dt_develop_t *dev)
{
  // reset any changes the selected plugin might have made.
  _dt_second_window_change_cursor(dev, "default");
}

static void _second_window_configure_ppd_dpi(dt_develop_t *dev)
{
  GtkWidget *widget = dev->second_wnd;

  dev->preview2.ppd = dt_get_system_gui_ppd(widget);
  dev->preview2.dpi = dt_get_screen_resolution(widget);

#ifdef GDK_WINDOWING_QUARTZ
  dev->preview2.dpi_factor
      = dev->preview2.dpi / 72; // macOS has a fixed DPI of 72
#else
  dev->preview2.dpi_factor
      = dev->preview2.dpi / 96; // according to man xrandr and the docs of gdk_screen_set_resolution 96 is the default
#endif
}

static gboolean _second_window_draw_callback(GtkWidget *widget,
                                             cairo_t *cri,
                                             dt_develop_t *dev)
{
  // Set background
  dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_DARKROOM_BG);
  cairo_paint(cri);

  // Early exit if we're in an inconsistent state
  if(!dev->preview2.widget || dev->gui_leaving)
    return TRUE;

  // Determine which develop and viewport to use
  // Take a local copy of the pointer to avoid race conditions
  dt_develop_t *pinned_dev = dev->preview2_pinned ? dev->preview2_pinned_dev : NULL;
  dt_develop_t *render_dev = pinned_dev ? pinned_dev : dev;
  dt_dev_viewport_t *port = &render_dev->preview2;
  
  // Check if pinned dev is being cleaned up
  if(pinned_dev && pinned_dev->gui_leaving)
  {
    render_dev = dev;
    port = &dev->preview2;
    pinned_dev = NULL;
  }
  
  // For pinned images, sync viewport dimensions from main dev
  if(pinned_dev)
  {
    port->width = dev->preview2.width;
    port->height = dev->preview2.height;
    port->orig_width = dev->preview2.orig_width;
    port->orig_height = dev->preview2.orig_height;
    port->ppd = dev->preview2.ppd;
    port->dpi = dev->preview2.dpi;
    port->dpi_factor = dev->preview2.dpi_factor;
  }

  if(port->pipe && port->pipe->backbuf)  // do we have a preview image?
  {
    // draw the preview image using the appropriate viewport
    _view_paint_surface(cri, dev->preview2.orig_width, dev->preview2.orig_height,
                       port, DT_WINDOW_SECOND);
  }
  else if(pinned_dev)
  {
    // Pinned image is still rendering.
    // Only use the main dev's backbuf as a fallback when the pinned image is
    // the same as the one in the main view — this avoids flickering when
    // pinning the currently-edited image.
    // For a different image, keep the black background rather than flashing
    // the wrong image while the new pixelpipe processes.
    if(pinned_dev->image_storage.id == dev->image_storage.id
       && dev->preview2.pipe && dev->preview2.pipe->backbuf)
    {
      _view_paint_surface(cri, dev->preview2.orig_width, dev->preview2.orig_height,
                         &dev->preview2, DT_WINDOW_SECOND);
    }
  }

  // Request processing if needed
  if(pinned_dev && !pinned_dev->gui_leaving)
  {
    // Process pinned image pipeline - process if no backbuf, pipe dirty, or zoom/pan changed
    if(!port->pipe->backbuf
       || port->pipe->status == DT_DEV_PIXELPIPE_DIRTY
       || port->pipe->status == DT_DEV_PIXELPIPE_INVALID
       || port->pipe->changed != DT_DEV_PIPE_UNCHANGED)
    {
      dt_dev_process_preview2(pinned_dev);
    }
  }
  else if(!pinned_dev)
  {
    // Process main dev's preview2 pipeline
    if(_preview2_request(dev)) dt_dev_process_preview2(dev);
  }

  return TRUE;
}

static gboolean _second_window_scrolled_callback(GtkWidget *widget,
                                                 GdkEventScroll *event,
                                                 dt_develop_t *dev)
{
  if(dev->gui_leaving) return TRUE;
  
  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    // Use pinned viewport if pinned, otherwise main dev's preview2
    dt_develop_t *pinned_dev = dev->preview2_pinned ? dev->preview2_pinned_dev : NULL;
    if(pinned_dev && pinned_dev->gui_leaving) pinned_dev = NULL;
    
    dt_dev_viewport_t *port = pinned_dev ? &pinned_dev->preview2 : &dev->preview2;

    const gboolean constrained = !dt_modifier_is(event->state, GDK_CONTROL_MASK);
    dt_dev_zoom_move(port, DT_ZOOM_SCROLL, 0.0f, delta_y < 0,
                     event->x, event->y, constrained);
  }

  return TRUE;
}

static gboolean _second_window_button_pressed_callback(GtkWidget *w,
                                                       GdkEventButton *event,
                                                       dt_develop_t *dev)
{
  if(dev->gui_leaving) return FALSE;
  
  // Use pinned viewport if pinned, otherwise main dev's preview2
  dt_develop_t *pinned_dev = dev->preview2_pinned ? dev->preview2_pinned_dev : NULL;
  if(pinned_dev && pinned_dev->gui_leaving) pinned_dev = NULL;
  
  dt_dev_viewport_t *port = pinned_dev ? &pinned_dev->preview2 : &dev->preview2;

  // Handle double-click to reset zoom and center
  if(event->type == GDK_2BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY)
  {
    dt_dev_zoom_move(port, DT_ZOOM_FIT, 0.0f, 0,
                     event->x, event->y, TRUE);
    return TRUE;
  }
  if(event->button == GDK_BUTTON_PRIMARY)
  {
    // store coordinates in logical pixels (as delivered by event)
    darktable.control->button_x = event->x;
    darktable.control->button_y = event->y;
    _dt_second_window_change_cursor(dev, "grabbing");
    return TRUE;
  }
  if(event->button == GDK_BUTTON_MIDDLE)
  {
    dt_dev_zoom_move(port, DT_ZOOM_1, 0.0f, -2,
                     event->x, event->y, !dt_modifier_is(event->state, GDK_CONTROL_MASK));
    return TRUE;
  }
  return FALSE;
}

static gboolean _second_window_button_released_callback(GtkWidget *w,
                                                        GdkEventButton *event,
                                                        dt_develop_t *dev)
{
  if(event->button == GDK_BUTTON_PRIMARY) _dt_second_window_change_cursor(dev, "default");

  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean _second_window_mouse_moved_callback(GtkWidget *w,
                                                    GdkEventMotion *event,
                                                    dt_develop_t *dev)
{
  if(dev->gui_leaving) return FALSE;
  
  if(event->state & GDK_BUTTON1_MASK)
  {
    dt_control_t *ctl = darktable.control;
    
    // Use pinned viewport if pinned, otherwise main dev's preview2
    dt_develop_t *pinned_dev = dev->preview2_pinned ? dev->preview2_pinned_dev : NULL;
    if(pinned_dev && pinned_dev->gui_leaving) pinned_dev = NULL;
    
    dt_dev_viewport_t *port = pinned_dev ? &pinned_dev->preview2 : &dev->preview2;

    dt_dev_zoom_move(port, DT_ZOOM_MOVE, -1.f, 0,
                     event->x - ctl->button_x, event->y - ctl->button_y, TRUE);
    ctl->button_x = event->x;
    ctl->button_y = event->y;
    return TRUE;
  }
  return FALSE;
}

static gboolean _second_window_leave_callback(GtkWidget *widget,
                                              GdkEventCrossing *event,
                                              dt_develop_t *dev)
{
  _second_window_leave(dev);
  return TRUE;
}

static gboolean _second_window_configure_callback(GtkWidget *da,
                                                  GdkEventConfigure *event,
                                                  dt_develop_t *dev)
{
  if(dev->gui_leaving) return TRUE;
  
  gboolean size_changed = (dev->preview2.orig_width != event->width || 
                          dev->preview2.orig_height != event->height);
  
  if(size_changed)
  {
    dev->preview2.width = event->width;
    dev->preview2.height = event->height;
    dev->preview2.orig_width = event->width;
    dev->preview2.orig_height = event->height;

    // pipe needs to be reconstructed
    dev->preview2.pipe->status = DT_DEV_PIXELPIPE_DIRTY;
    dev->preview2.pipe->changed |= DT_DEV_PIPE_REMOVE;
    dev->preview2.pipe->cache_obsolete = TRUE;
    
    // If we have a pinned image, update its viewport dimensions too
    dt_develop_t *pinned_dev = dev->preview2_pinned ? dev->preview2_pinned_dev : NULL;
    if(pinned_dev && !pinned_dev->gui_leaving)
    {
      dt_dev_viewport_t *pinned_port = &pinned_dev->preview2;
      pinned_port->width = event->width;
      pinned_port->height = event->height;
      pinned_port->orig_width = event->width;
      pinned_port->orig_height = event->height;
      pinned_port->pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      pinned_port->pipe->changed |= DT_DEV_PIPE_REMOVE;
      pinned_port->pipe->cache_obsolete = TRUE;
    }
  }

  dt_colorspaces_set_display_profile(DT_COLORSPACE_DISPLAY2);

#ifndef GDK_WINDOWING_QUARTZ
  _second_window_configure_ppd_dpi(dev);
#endif

  dt_dev_configure(&dev->preview2);
  
  // Also configure pinned viewport if present
  dt_develop_t *pinned_dev = dev->preview2_pinned ? dev->preview2_pinned_dev : NULL;
  if(pinned_dev && !pinned_dev->gui_leaving)
  {
    dt_dev_viewport_t *pinned_port = &pinned_dev->preview2;
    pinned_port->ppd = dev->preview2.ppd;
    pinned_port->dpi = dev->preview2.dpi;
    pinned_port->dpi_factor = dev->preview2.dpi_factor;
    dt_dev_configure(pinned_port);
  }

  return TRUE;
}

static gboolean _second_window_buttons_enter_notify_callback(GtkWidget *widget,
                                                              GdkEventCrossing *event,
                                                              GtkWidget *button_box)
{
  // Make buttons visible and interactive.  Using opacity instead of hide/show
  // keeps the GdkWindow (and its NSView tracking areas on macOS) always alive,
  // which is required for GTK's tooltip mechanism to work correctly.
  gtk_widget_set_opacity(button_box, 1.0);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(gtk_widget_get_parent(button_box)),
                                       button_box, FALSE);
  return FALSE;
}

static gboolean _second_window_buttons_leave_notify_callback(GtkWidget *widget,
                                                              GdkEventCrossing *event,
                                                              GtkWidget *button_box)
{
  // GDK_NOTIFY_INFERIOR means the pointer moved into a child window (still
  // within the second window); keep the buttons visible in that case.
  if(event->detail != GDK_NOTIFY_INFERIOR)
  {
    gtk_widget_set_opacity(button_box, 0.0);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(gtk_widget_get_parent(button_box)),
                                         button_box, TRUE);
  }
  return FALSE;
}

// Callback for the pin button in the overlay
static void _preview2_pin_button_clicked(GtkToggleButton *button,
                                         dt_develop_t *dev)
{
  dt_dev_toggle_preview2_pinned(dev);
}


static void _darkroom_ui_second_window_init(GtkWidget *overlay,
                                            dt_develop_t *dev)
{
  // Get the window that contains this overlay
  GtkWidget *window = gtk_widget_get_toplevel(overlay);
  
  const int width = MAX(10, dt_conf_get_int("second_window/window_w"));
  const int height = MAX(10, dt_conf_get_int("second_window/window_h"));
  const gint x = MAX(0, dt_conf_get_int("second_window/window_x"));
  const gint y = MAX(0, dt_conf_get_int("second_window/window_y"));
  
  // Group buttons in a vertical box for easy future expansion.
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  // Create the pin button
  GtkWidget *pin_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_pin, 0, NULL);
  gtk_widget_set_name(pin_button, "dt_window2_pin_button");
  gtk_widget_set_size_request(pin_button, 24, 24);
  gtk_widget_set_tooltip_text(pin_button, _("pin current image"));
  g_signal_connect(G_OBJECT(pin_button), "toggled",
                   G_CALLBACK(_preview2_pin_button_clicked), dev);
  gtk_box_pack_start(GTK_BOX(button_box), pin_button, FALSE, FALSE, 0);

  // Associate the pin button with the toggle COMMAND action registered in
  // gui_init().  Passing action_def=NULL leaves the action type and callback
  // unchanged, but sets the action quark on the widget so right-click
  // shortcut assignment and shortcut tooltips work on the pin button itself.
  dt_action_define(DT_ACTION(darktable.view_manager->current_view), NULL,
                   N_("toggle pinned state in second window"), pin_button, NULL);

  // Wrap the box in a GtkEventBox so that the overlay can toggle pass-through on
  // a windowed widget, which enables tooltip rendering.
  GtkWidget *event_box = gtk_event_box_new();
  gtk_widget_set_halign(event_box, GTK_ALIGN_END);
  gtk_widget_set_valign(event_box, GTK_ALIGN_START);
  gtk_widget_set_margin_top(event_box, 10);
  gtk_widget_set_margin_end(event_box, 10);
  gtk_container_add(GTK_CONTAINER(event_box), button_box);

  // Add the event box as a single overlay widget.  Start transparent and
  // non-interactive; the enter/leave callbacks will toggle opacity and
  // pass-through.  Keeping the widget always mapped (never hidden) preserves
  // NSView tracking areas on macOS, which is required for GTK's tooltip
  // mechanism to work.
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), event_box);
  gtk_widget_show_all(event_box);
  gtk_widget_set_opacity(event_box, 0.0);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), event_box, TRUE);

  // Needed to display/hide the widgets.
  // Must be done before the window is realized.
  gtk_widget_add_events(window, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

  // Show / hide controls on enter/leave events.
  g_signal_connect(G_OBJECT(window), "enter-notify-event",
                   G_CALLBACK(_second_window_buttons_enter_notify_callback), event_box);
  g_signal_connect(G_OBJECT(window), "leave-notify-event",
                   G_CALLBACK(_second_window_buttons_leave_notify_callback), event_box);

  dev->preview2.pin_button = pin_button;
  
  dev->preview2.border_size = 0;
  gtk_window_set_default_size(GTK_WINDOW(window), width, height);
  gtk_window_move(GTK_WINDOW(window), x, y);
  gtk_window_resize(GTK_WINDOW(window), width, height);
  
  // Handle window state (fullscreen/maximized)
  const int fullscreen = dt_conf_get_bool("second_window/fullscreen");
  if (fullscreen)
    gtk_window_fullscreen(GTK_WINDOW(window));
  else
  {
    gtk_window_unfullscreen(GTK_WINDOW(window));
    const int maximized = dt_conf_get_bool("second_window/maximized");
    if (maximized)
      gtk_window_maximize(GTK_WINDOW(window));
    else
      gtk_window_unmaximize(GTK_WINDOW(window));
  }
}

static void _darkroom_ui_second_window_write_config(GtkWidget *widget)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gint x, y;
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  dt_conf_set_int("second_window/window_x", x);
  dt_conf_set_int("second_window/window_y", y);
  dt_conf_set_int("second_window/window_w", allocation.width);
  dt_conf_set_int("second_window/window_h", allocation.height);
  dt_conf_set_bool("second_window/maximized",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_MAXIMIZED));
  dt_conf_set_bool("second_window/fullscreen",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_FULLSCREEN));
}

// Helper to clean up second window state - called before destroying window
static void _darkroom_ui_second_window_cleanup(dt_develop_t *dev)
{
  // Signal main preview2 pipe to stop and wait for any pending jobs
  if(dev->preview2.pipe)
  {
    dt_atomic_set_int(&dev->preview2.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NODES);
    dt_pthread_mutex_lock(&dev->preview2.pipe->mutex);
    dt_pthread_mutex_unlock(&dev->preview2.pipe->mutex);
    dt_pthread_mutex_lock(&dev->preview2.pipe->busy_mutex);
    dt_pthread_mutex_unlock(&dev->preview2.pipe->busy_mutex);
  }

  // Clean up pinned develop
  if(dev->preview2_pinned && dev->preview2_pinned_dev)
  {
    dt_develop_t *pinned_dev = dev->preview2_pinned_dev;
    
    pinned_dev->gui_leaving = TRUE;
    pinned_dev->preview2.widget = NULL;
    
    if(pinned_dev->preview2.pipe)
      dt_atomic_set_int(&pinned_dev->preview2.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NODES);
    if(pinned_dev->preview_pipe)
      dt_atomic_set_int(&pinned_dev->preview_pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NODES);
    if(pinned_dev->full.pipe)
      dt_atomic_set_int(&pinned_dev->full.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NODES);
    
    if(pinned_dev->preview2.pipe)
    {
      dt_pthread_mutex_lock(&pinned_dev->preview2.pipe->mutex);
      dt_pthread_mutex_unlock(&pinned_dev->preview2.pipe->mutex);
      dt_pthread_mutex_lock(&pinned_dev->preview2.pipe->busy_mutex);
      dt_pthread_mutex_unlock(&pinned_dev->preview2.pipe->busy_mutex);
    }
    
    dt_dev_cleanup(pinned_dev);
    free(pinned_dev);
    dev->preview2_pinned_dev = NULL;
    dev->preview2_pinned = FALSE;
  }

  dev->second_wnd = NULL;
  dev->preview2.widget = NULL;
  dev->preview2.pin_button = NULL;
}

static gboolean _second_window_delete_callback(GtkWidget *widget,
                                               GdkEvent *event,
                                               dt_develop_t *dev)
{
  // Called when user closes window via window manager (X button)
  _darkroom_ui_second_window_write_config(widget);
  dt_conf_set_bool("second_window/last_visible", FALSE);

  // There's a bug in GTK+3 where fullscreen GTK window on macOS may cause EXC_BAD_ACCESS.
  gtk_window_unfullscreen(GTK_WINDOW(widget));

  _darkroom_ui_second_window_cleanup(dev);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_wnd_button), FALSE);

  return FALSE;
}

static void _second_window_dnd_received(GtkWidget *widget,
                                        GdkDragContext *context,
                                        const gint x,
                                        const gint y,
                                        GtkSelectionData *selection_data,
                                        const guint target_type,
                                        const guint time,
                                        gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  gboolean success = FALSE;

  if(selection_data != NULL && target_type == DND_TARGET_IMGID)
  {
    const int imgs_nb = gtk_selection_data_get_length(selection_data) / sizeof(dt_imgid_t);
    if(imgs_nb)
    {
      const dt_imgid_t *imgs = (const dt_imgid_t *)gtk_selection_data_get_data(selection_data);
      if(dt_is_valid_imgid(imgs[0]))
      {
        dt_dev_pin_image(dev, imgs[0]);
        success = TRUE;
      }
    }
  }

  gtk_drag_finish(context, success, FALSE, time);
}

static void _darkroom_display_second_window(dt_develop_t *dev)
{
  // Wait for any pending jobs and reset shutdown flag
  if(dev->preview2.pipe)
  {
    dt_pthread_mutex_lock(&dev->preview2.pipe->mutex);
    dt_pthread_mutex_unlock(&dev->preview2.pipe->mutex);
    dt_pthread_mutex_lock(&dev->preview2.pipe->busy_mutex);
    dt_pthread_mutex_unlock(&dev->preview2.pipe->busy_mutex);
    dt_atomic_set_int(&dev->preview2.pipe->shutdown, DT_DEV_PIXELPIPE_STOP_NO);
  }
    
  if(dev->second_wnd == NULL)
  {
    dev->preview2.width = -1;
    dev->preview2.height = -1;

    dev->second_wnd = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(dev->second_wnd, "second_window");

    _second_window_configure_ppd_dpi(dev);

    gtk_window_set_icon_name(GTK_WINDOW(dev->second_wnd), "darktable");
    gtk_window_set_title(GTK_WINDOW(dev->second_wnd), _("darktable - darkroom preview"));

#ifndef GDK_WINDOWING_QUARTZ
    // On macOS, transient_for is implemented via [NSWindow addChildWindow:ordered:],
    // which constrains the child to the parent's screen and prevents it from being
    // moved to a different monitor.  Use keep_above instead (see below, after show_all,
    // where the NSWindow is already realized).
    gtk_window_set_transient_for(GTK_WINDOW(dev->second_wnd),
                                 GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
#endif

    // Create the overlay for the window
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_add_events(overlay, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    gtk_container_add(GTK_CONTAINER(dev->second_wnd), overlay);
    
    // Create the drawing area and add it to the overlay
    dev->preview2.widget = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(overlay), dev->preview2.widget);
    gtk_widget_set_size_request(dev->preview2.widget, DT_PIXEL_APPLY_DPI_2ND_WND(dev, 50), DT_PIXEL_APPLY_DPI_2ND_WND(dev, 200));
    gtk_widget_set_hexpand(dev->preview2.widget, TRUE);
    gtk_widget_set_vexpand(dev->preview2.widget, TRUE);
    gtk_widget_set_app_paintable(dev->preview2.widget, TRUE);

    gtk_widget_set_events(dev->preview2.widget,
                          GDK_POINTER_MOTION_MASK
                          | GDK_BUTTON_PRESS_MASK
                          | GDK_BUTTON_RELEASE_MASK
                          | GDK_ENTER_NOTIFY_MASK
                          | GDK_LEAVE_NOTIFY_MASK
                          | darktable.gui->scroll_mask);

    /* connect callbacks */
    g_signal_connect(G_OBJECT(dev->preview2.widget), "draw",
                     G_CALLBACK(_second_window_draw_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "scroll-event",
                     G_CALLBACK(_second_window_scrolled_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "button-press-event",
                     G_CALLBACK(_second_window_button_pressed_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "button-release-event",
                     G_CALLBACK(_second_window_button_released_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "motion-notify-event",
                     G_CALLBACK(_second_window_mouse_moved_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "leave-notify-event",
                     G_CALLBACK(_second_window_leave_callback), dev);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "configure-event",
                     G_CALLBACK(_second_window_configure_callback), dev);

    /* dropping a filmstrip thumbnail pins it in the 2nd window */
    gtk_drag_dest_set(dev->preview2.widget, GTK_DEST_DEFAULT_ALL,
                      target_list_internal, n_targets_internal,
                      GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(G_OBJECT(dev->preview2.widget), "drag-data-received",
                     G_CALLBACK(_second_window_dnd_received), dev);

    g_signal_connect(G_OBJECT(dev->second_wnd), "delete-event",
                     G_CALLBACK(_second_window_delete_callback), dev);
    g_signal_connect(G_OBJECT(dev->second_wnd), "event",
                     G_CALLBACK(dt_shortcut_dispatcher), NULL);

    _darkroom_ui_second_window_init(overlay, dev);
  }

  // Show all widgets in the window
  gtk_widget_show_all(dev->second_wnd);

#ifdef GDK_WINDOWING_QUARTZ
  // keep_above must be set after the window is realized (i.e. after show_all),
  // because the Quartz backend applies the NSWindow level change only to an
  // already-existing NSWindow object.
  gtk_window_set_keep_above(GTK_WINDOW(dev->second_wnd), TRUE);
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
