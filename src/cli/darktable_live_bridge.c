/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <glib.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DT_LIVE_BRIDGE_VERSION 1
#define DT_LIVE_BRIDGE_SERVICE "org.darktable.service"
#define DT_LIVE_BRIDGE_PATH "/darktable"
#define DT_LIVE_BRIDGE_INTERFACE "org.darktable.service.Remote"
#define DT_LIVE_BRIDGE_CACHE_KEY "__darktable_live_bridge_v1_controls"
#define DT_LIVE_BRIDGE_EXPOSURE_MIN -3.0
#define DT_LIVE_BRIDGE_EXPOSURE_MAX 4.0
#define DT_LIVE_BRIDGE_BLEND_OPACITY_MIN 0.0
#define DT_LIVE_BRIDGE_BLEND_OPACITY_MAX 100.0

typedef struct dt_live_bridge_blend_request_t
{
  gboolean have_opacity;
  double opacity;
  gboolean have_blend_mode;
  const gchar *blend_mode;
  gboolean have_reverse_order;
  gboolean reverse_order;
} dt_live_bridge_blend_request_t;

typedef struct dt_live_bridge_mask_request_t
{
  const gchar *action;
  const gchar *source_instance_key;
} dt_live_bridge_mask_request_t;

typedef struct dt_live_bridge_blend_mode_name_t
{
  const gchar *name;
} dt_live_bridge_blend_mode_name_t;

static const dt_live_bridge_blend_mode_name_t dt_live_bridge_blend_modes[] = {
  { "normal" },
  { "average" },
  { "difference" },
  { "bounded" },
  { "lighten" },
  { "darken" },
  { "screen" },
  { "multiply" },
  { "divide" },
  { "add" },
  { "subtract" },
  { "geometric-mean" },
  { "harmonic-mean" },
  { "overlay" },
  { "softlight" },
  { "hardlight" },
  { "vividlight" },
  { "linearlight" },
  { "pinlight" },
  { "lightness" },
  { "chromaticity" },
  { "lab-lightness" },
  { "lab-a" },
  { "lab-b" },
  { "lab-color" },
  { "rgb-r" },
  { "rgb-g" },
  { "rgb-b" },
  { "hsv-value" },
  { "hsv-color" },
  { "hue" },
  { "color" },
  { "coloradjust" },
  { "difference-legacy" },
  { "subtract-inverse" },
  { "divide-inverse" },
  { "lab-l" },
  { NULL },
};

static void usage(FILE *stream, const char *progname)
{
  fprintf(stream,
          "Usage:\n"
          "  %s get-session\n"
          "  %s get-snapshot\n"
          "  %s list-controls\n"
          "  %s get-control <control-id>\n"
          "  %s set-control <control-id> <value-json>\n"
          "  %s set-exposure <EV>\n"
           "  %s apply-module-instance-action <instance-key> <action>\n"
           "  %s apply-module-instance-action <instance-key> <move-before|move-after> <anchor-instance-key>\n"
           "  %s apply-module-instance-blend <instance-key> <blend-json>\n"
           "  %s apply-module-instance-mask <instance-key> <mask-json>\n"
           "  %s --help\n",
           progname, progname, progname, progname, progname, progname, progname, progname, progname,
           progname, progname);
}

static gboolean print_json_only(const gchar *json, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if(!json_parser_load_from_data(parser, json, -1, error)) return FALSE;

  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *root = json_parser_get_root(parser);
  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, FALSE);

  g_autofree gchar *normalized = json_generator_to_data(generator, NULL);
  fputs(normalized, stdout);
  fputc('\n', stdout);
  return TRUE;
}

static gboolean call_lua(const gchar *lua_source, gchar **json_result, GError **error)
{
  g_autoptr(GDBusConnection) connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
  if(connection == NULL) return FALSE;

  g_autoptr(GVariant) result = g_dbus_connection_call_sync(connection, DT_LIVE_BRIDGE_SERVICE,
                                                           DT_LIVE_BRIDGE_PATH,
                                                           DT_LIVE_BRIDGE_INTERFACE, "Lua",
                                                           g_variant_new("(s)", lua_source),
                                                           G_VARIANT_TYPE("(s)"),
                                                           G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                                           error);
  if(result == NULL) return FALSE;

  g_variant_get(result, "(s)", json_result);
  return TRUE;
}

static gchar *lua_string_literal(const gchar *value)
{
  GString *literal = g_string_new("'");

  for(const guchar *cursor = (const guchar *)value; cursor != NULL && *cursor != '\0'; cursor++)
  {
    switch(*cursor)
    {
      case '\\':
        g_string_append(literal, "\\\\");
        break;
      case '\'':
        g_string_append(literal, "\\\'");
        break;
      case '\n':
        g_string_append(literal, "\\n");
        break;
      case '\r':
        g_string_append(literal, "\\r");
        break;
      case '\t':
        g_string_append(literal, "\\t");
        break;
      default:
        if(*cursor < 0x20)
          g_string_append_printf(literal, "\\%03u", (unsigned int)*cursor);
        else
          g_string_append_c(literal, (gchar)*cursor);
        break;
    }
  }

  g_string_append_c(literal, '\'');
  return g_string_free(literal, FALSE);
}

static gboolean parse_json_number_literal(const gchar *json_text, double *value_out, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if(!json_parser_load_from_data(parser, json_text, -1, error)) return FALSE;

  JsonNode *root = json_parser_get_root(parser);
  if(root == NULL || !JSON_NODE_HOLDS_VALUE(root))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "control value must be a JSON number literal");
    return FALSE;
  }

  const GType value_type = json_node_get_value_type(root);
  if(value_type != G_TYPE_DOUBLE && value_type != G_TYPE_INT64 && value_type != G_TYPE_INT
     && value_type != G_TYPE_UINT64 && value_type != G_TYPE_UINT && value_type != G_TYPE_LONG
     && value_type != G_TYPE_ULONG)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "control value must be a JSON number literal");
    return FALSE;
  }

  const double value = json_node_get_double(root);
  if(!isfinite(value))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "control value must be finite");
    return FALSE;
  }

  *value_out = value;
  return TRUE;
}

static gboolean validate_json_literal(const gchar *json_text, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  return json_parser_load_from_data(parser, json_text, -1, error);
}

static gboolean json_node_holds_number(JsonNode *node)
{
  if(node == NULL || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;

  const GType value_type = json_node_get_value_type(node);
  return value_type == G_TYPE_DOUBLE || value_type == G_TYPE_INT64 || value_type == G_TYPE_INT
         || value_type == G_TYPE_UINT64 || value_type == G_TYPE_UINT || value_type == G_TYPE_LONG
         || value_type == G_TYPE_ULONG;
}

static gboolean blend_mode_name_supported(const gchar *name)
{
  if(name == NULL || name[0] == '\0') return FALSE;

  for(const dt_live_bridge_blend_mode_name_t *mode = dt_live_bridge_blend_modes; mode->name != NULL; mode++)
  {
    if(g_strcmp0(mode->name, name) == 0) return TRUE;
  }

  return FALSE;
}

static gboolean parse_blend_json(const gchar *json_text,
                                 dt_live_bridge_blend_request_t *request_out,
                                 GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if(!json_parser_load_from_data(parser, json_text, -1, error)) return FALSE;

  JsonNode *root = json_parser_get_root(parser);
  if(root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "blend value must be a JSON object");
    return FALSE;
  }

  JsonObject *object = json_node_get_object(root);
  if(object == NULL)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "blend value must be a JSON object");
    return FALSE;
  }

  dt_live_bridge_blend_request_t request = { 0 };
  GList *members = json_object_get_members(object);
  for(const GList *iter = members; iter != NULL; iter = g_list_next(iter))
  {
    const gchar *key = iter->data;
    JsonNode *member = json_object_get_member(object, key);

    if(g_strcmp0(key, "opacity") == 0)
    {
      if(!json_node_holds_number(member))
      {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "blend opacity must be a JSON number");
        g_list_free(members);
        return FALSE;
      }

      const double opacity = json_node_get_double(member);
      if(!isfinite(opacity))
      {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "blend opacity must be finite");
        g_list_free(members);
        return FALSE;
      }

      if(opacity < DT_LIVE_BRIDGE_BLEND_OPACITY_MIN || opacity > DT_LIVE_BRIDGE_BLEND_OPACITY_MAX)
      {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "blend opacity must be between %.0f and %.0f",
                    DT_LIVE_BRIDGE_BLEND_OPACITY_MIN, DT_LIVE_BRIDGE_BLEND_OPACITY_MAX);
        g_list_free(members);
        return FALSE;
      }

      request.have_opacity = TRUE;
      request.opacity = opacity;
    }
    else if(g_strcmp0(key, "blendMode") == 0)
    {
      if(member == NULL || !JSON_NODE_HOLDS_VALUE(member)
         || json_node_get_value_type(member) != G_TYPE_STRING)
      {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "blend blendMode must be a JSON string");
        g_list_free(members);
        return FALSE;
      }

      const gchar *blend_mode = json_node_get_string(member);
      if(!blend_mode_name_supported(blend_mode))
      {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "unknown blend mode: %s",
                    blend_mode != NULL ? blend_mode : "");
        g_list_free(members);
        return FALSE;
      }

      request.have_blend_mode = TRUE;
      request.blend_mode = blend_mode;
    }
    else if(g_strcmp0(key, "reverseOrder") == 0)
    {
      if(member == NULL || !JSON_NODE_HOLDS_VALUE(member)
         || json_node_get_value_type(member) != G_TYPE_BOOLEAN)
      {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "blend reverseOrder must be a JSON boolean");
        g_list_free(members);
        return FALSE;
      }

      request.have_reverse_order = TRUE;
      request.reverse_order = json_node_get_boolean(member);
    }
    else
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "unknown blend field: %s",
                  key != NULL ? key : "");
      g_list_free(members);
      return FALSE;
    }
  }
  g_list_free(members);

  if(!request.have_opacity && !request.have_blend_mode && !request.have_reverse_order)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "blend value must include at least one of opacity, blendMode, or reverseOrder");
    return FALSE;
  }

  if(request_out != NULL) *request_out = request;
  return TRUE;
}

static gboolean parse_mask_json(const gchar *json_text,
                                dt_live_bridge_mask_request_t *request_out,
                                GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if(!json_parser_load_from_data(parser, json_text, -1, error)) return FALSE;

  JsonNode *root = json_parser_get_root(parser);
  if(root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "mask value must be a JSON object");
    return FALSE;
  }

  JsonObject *object = json_node_get_object(root);
  if(object == NULL)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "mask value must be a JSON object");
    return FALSE;
  }

  dt_live_bridge_mask_request_t request = { 0 };
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
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "mask action must be a JSON string");
        g_list_free(members);
        return FALSE;
      }

      const gchar *action = json_node_get_string(member);
      if(g_strcmp0(action, "clear-mask") != 0 && g_strcmp0(action, "reuse-same-shapes") != 0)
      {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "unknown mask action: %s",
                    action != NULL ? action : "");
        g_list_free(members);
        return FALSE;
      }

      request.action = action;
    }
    else if(g_strcmp0(key, "sourceInstanceKey") == 0)
    {
      if(member == NULL || !JSON_NODE_HOLDS_VALUE(member)
         || json_node_get_value_type(member) != G_TYPE_STRING)
      {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "mask sourceInstanceKey must be a JSON string");
        g_list_free(members);
        return FALSE;
      }

      request.source_instance_key = json_node_get_string(member);
    }
    else
    {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "unknown mask field: %s",
                  key != NULL ? key : "");
      g_list_free(members);
      return FALSE;
    }
  }
  g_list_free(members);

  if(request.action == NULL)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "mask value must include action");
    return FALSE;
  }

  if(g_strcmp0(request.action, "reuse-same-shapes") == 0)
  {
    if(request.source_instance_key == NULL || request.source_instance_key[0] == '\0')
    {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "mask reuse-same-shapes requires sourceInstanceKey");
      return FALSE;
    }
  }
  else if(request.source_instance_key != NULL)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "mask sourceInstanceKey is only supported for reuse-same-shapes");
    return FALSE;
  }

  if(request_out != NULL) *request_out = request;
  return TRUE;
}

static gboolean validate_exposure_value(const double value, const gchar *label, GError **error)
{
  if(value < DT_LIVE_BRIDGE_EXPOSURE_MIN || value > DT_LIVE_BRIDGE_EXPOSURE_MAX)
  {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s must be between %.0f and %.0f",
                label, DT_LIVE_BRIDGE_EXPOSURE_MIN, DT_LIVE_BRIDGE_EXPOSURE_MAX);
    return FALSE;
  }

  return TRUE;
}

static gchar *build_lua_command(const gchar *command, const gchar *control_id,
                                 const gchar *instance_key, const gchar *module_action,
                                 const gchar *anchor_instance_key,
                                 gboolean have_numeric_value, double numeric_value,
                                 const gchar *blend_json,
                                 const gchar *mask_json)
{
  const char *lua_template =
    "local bridge = rawget(_G, '" DT_LIVE_BRIDGE_CACHE_KEY "')\n"
    "if not bridge then\n"
    "  local darktable = require 'darktable'\n"
    "  bridge = {\n"
    "    bridgeVersion = 1,\n"
    "    exposureAction = nil,\n"
    "    view = '',\n"
    "    renderSequence = 0,\n"
    "    historyChangeSequence = 0,\n"
    "    imageLoadSequence = 0\n"
    "  }\n"
    "\n"
    "  local function json_escape(value)\n"
    "    local escaped = {}\n"
    "    for index = 1, #value do\n"
    "      local byte = string.byte(value, index)\n"
    "      if byte == 92 then escaped[#escaped + 1] = string.char(92, 92)\n"
    "      elseif byte == 34 then escaped[#escaped + 1] = string.char(92, 34)\n"
    "      elseif byte == 8 then escaped[#escaped + 1] = string.char(92, 98)\n"
    "      elseif byte == 12 then escaped[#escaped + 1] = string.char(92, 102)\n"
    "      elseif byte == 10 then escaped[#escaped + 1] = string.char(92, 110)\n"
    "      elseif byte == 13 then escaped[#escaped + 1] = string.char(92, 114)\n"
    "      elseif byte == 9 then escaped[#escaped + 1] = string.char(92, 116)\n"
    "      elseif byte <= 31 then escaped[#escaped + 1] = string.char(92, 117) .. string.format('%%04x', byte)\n"
    "      else escaped[#escaped + 1] = string.char(byte) end\n"
    "    end\n"
    "    return table.concat(escaped)\n"
    "  end\n"
    "\n"
    "  local function json_number(value)\n"
    "    if type(value) ~= 'number' or value ~= value or value == math.huge or value == -math.huge then\n"
    "      error('invalid numeric value')\n"
    "    end\n"
    "    local encoded = string.format('%%.17g', value)\n"
    "    if encoded == '-0' then encoded = '0' end\n"
    "    return encoded\n"
    "  end\n"
    "\n"
    "  local function json_encode(value)\n"
    "    local value_type = type(value)\n"
    "    if value_type == 'nil' then\n"
    "      return 'null'\n"
    "    elseif value_type == 'string' then\n"
    "      return '\"' .. json_escape(value) .. '\"'\n"
    "    elseif value_type == 'number' then\n"
    "      return json_number(value)\n"
    "    elseif value_type == 'boolean' then\n"
    "      return value and 'true' or 'false'\n"
    "    elseif value_type == 'table' then\n"
    "      local max_index = 0\n"
    "      local count = 0\n"
    "      local is_array = true\n"
    "      for key, _ in pairs(value) do\n"
    "        count = count + 1\n"
    "        if type(key) ~= 'number' or key < 1 or key %% 1 ~= 0 then\n"
    "          is_array = false\n"
    "          break\n"
    "        end\n"
    "        if key > max_index then max_index = key end\n"
    "      end\n"
    "      if is_array and max_index == count then\n"
    "        local items = {}\n"
    "        for index = 1, max_index do\n"
    "          items[index] = json_encode(value[index])\n"
    "        end\n"
    "        return '[' .. table.concat(items, ',') .. ']'\n"
    "      end\n"
    "\n"
    "      local keys = {}\n"
    "      for key, _ in pairs(value) do\n"
    "        if type(key) ~= 'string' then error('json object key must be a string') end\n"
    "        keys[#keys + 1] = key\n"
    "      end\n"
    "      table.sort(keys)\n"
    "      local items = {}\n"
    "      for _, key in ipairs(keys) do\n"
    "        items[#items + 1] = '\"' .. json_escape(key) .. '\":' .. json_encode(value[key])\n"
    "      end\n"
    "      return '{' .. table.concat(items, ',') .. '}'\n"
    "    end\n"
    "    error('unsupported json value type: ' .. value_type)\n"
    "  end\n"
    "\n"
    "  local function update_view()\n"
    "    local current_view = darktable.gui.current_view()\n"
    "    bridge.view = current_view and tostring(current_view) or ''\n"
    "  end\n"
    "\n"
    "  local function current_image()\n"
    "    update_view()\n"
    "    if bridge.view ~= 'darkroom' then\n"
    "      return nil, 'unsupported-view'\n"
    "    end\n"
    "\n"
    "    local darkroom = darktable.gui.views.darkroom\n"
    "    local image = darkroom and darkroom.display_image and darkroom.display_image() or nil\n"
    "    if not image then\n"
    "      return nil, 'no-active-image'\n"
    "    end\n"
    "\n"
    "    return image, nil\n"
    "  end\n"
    "\n"
    "  local function session_object()\n"
    "    update_view()\n"
    "    return {\n"
    "      view = bridge.view,\n"
    "      renderSequence = bridge.renderSequence,\n"
    "      historyChangeSequence = bridge.historyChangeSequence,\n"
    "      imageLoadSequence = bridge.imageLoadSequence\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function active_image_object(image)\n"
    "    return {\n"
    "      imageId = image.id,\n"
    "      directoryPath = image.path,\n"
    "      fileName = image.filename,\n"
    "      sourceAssetPath = image.path .. '/' .. image.filename\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function unavailable(reason, requested_control_id)\n"
    "    local payload = {\n"
    "      bridgeVersion = bridge.bridgeVersion,\n"
    "      reason = reason,\n"
    "      session = session_object(),\n"
    "      status = 'unavailable'\n"
    "    }\n"
    "    if requested_control_id ~= nil then payload.requestedControlId = requested_control_id end\n"
    "    return payload\n"
    "  end\n"
    "\n"
    "  local function numbers_equal(left, right)\n"
    "    return type(left) == 'number' and type(right) == 'number'\n"
    "      and left == left and right == right and math.abs(left - right) <= 1e-6\n"
    "  end\n"
    "\n"
    "  local exposure_soft_min = -3.0\n"
    "  local exposure_soft_max = 4.0\n"
    "\n"
    "  local function exposure_position_to_ev(position)\n"
    "    return exposure_soft_min + ((exposure_soft_max - exposure_soft_min) * position)\n"
    "  end\n"
    "\n"
    "  local function exposure_ev_to_position(value)\n"
    "    return (value - exposure_soft_min) / (exposure_soft_max - exposure_soft_min)\n"
    "  end\n"
    "\n"
    "  local function read_exposure_action(action)\n"
    "    local ok, value = pcall(darktable.gui.action, action, '', '', 0)\n"
    "    if ok and type(value) == 'number' and value == value then return value end\n"
    "    error('exposure action unavailable')\n"
    "  end\n"
    "\n"
    "  local function write_exposure_action(action, value)\n"
    "    local ok, current = pcall(darktable.gui.action, action, '', 'set', value)\n"
    "    if ok and type(current) == 'number' and current == current then return current end\n"
    "    error('exposure action unavailable after update')\n"
    "  end\n"
    "\n"
    "  local function wait_for_exposure_settle(image_id, action, expected, render_before, history_before)\n"
    "    local current = read_exposure_action(action)\n"
    "    local previous_read = current\n"
    "    local stable_reads = 0\n"
    "    local observed_render_sequence = bridge.renderSequence\n"
    "\n"
    "    for _ = 1, 40 do\n"
    "      darktable.control.sleep(100)\n"
    "      local image, reason = current_image()\n"
    "      if not image then return current, observed_render_sequence, reason end\n"
    "      if image.id ~= image_id then return current, observed_render_sequence, 'image-changed' end\n"
    "\n"
    "      current = read_exposure_action(action)\n"
    "      observed_render_sequence = bridge.renderSequence\n"
    "\n"
    "      if numbers_equal(current, previous_read) then\n"
    "        stable_reads = stable_reads + 1\n"
    "      else\n"
    "        stable_reads = 0\n"
    "      end\n"
    "\n"
    "      previous_read = current\n"
    "\n"
    "      if (bridge.renderSequence > render_before or bridge.historyChangeSequence > history_before)\n"
    "         and (numbers_equal(current, expected) or stable_reads >= 2) then\n"
    "        break\n"
    "      end\n"
    "    end\n"
    "\n"
    "    return current, observed_render_sequence, nil\n"
    "  end\n"
    "\n"
    "  local function available_payload(image)\n"
    "    local current = exposure_position_to_ev(read_exposure_action(bridge.exposure_action()))\n"
    "    return {\n"
    "      activeImage = active_image_object(image),\n"
    "      bridgeVersion = bridge.bridgeVersion,\n"
    "      exposure = { current = current },\n"
    "      session = session_object(),\n"
    "      status = 'ok'\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function get_session()\n"
    "    local image, reason = current_image()\n"
    "    if not image then return unavailable(reason) end\n"
    "    return available_payload(image)\n"
    "  end\n"
    "\n"
    "  local control_registry\n"
    "\n"
    "  local function splice_snapshot_controls(snapshot_json, controls)\n"
    "    if type(snapshot_json) ~= 'string' or snapshot_json == '' then error('snapshot unavailable') end\n"
    "    if string.sub(snapshot_json, -1) ~= '}' then error('snapshot payload malformed') end\n"
    "    return string.sub(snapshot_json, 1, -2) .. ',\"controls\":' .. json_encode(controls) .. '}'\n"
    "  end\n"
    "\n"
    "  local function snapshot_controls()\n"
    "    local controls = {}\n"
    "    for _, control_id in ipairs({ 'exposure.exposure' }) do\n"
    "      local adapter = control_registry()[control_id]\n"
    "      controls[#controls + 1] = adapter.snapshot and adapter.snapshot() or adapter.list()\n"
    "    end\n"
    "    return controls\n"
    "  end\n"
    "\n"
    "  local function get_snapshot_json()\n"
    "    local image, reason = current_image()\n"
    "    if not image then return json_encode(unavailable(reason)) end\n"
    "\n"
    "    local darkroom = darktable.gui.views.darkroom\n"
    "    local snapshot_json = darkroom and darkroom.live_snapshot and darkroom.live_snapshot() or nil\n"
    "    snapshot_json = splice_snapshot_controls(snapshot_json, snapshot_controls())\n"
    "\n"
    "    return '{'\n"
    "      .. '\"bridgeVersion\":' .. json_encode(bridge.bridgeVersion)\n"
    "      .. ',\"status\":\"ok\"'\n"
    "      .. ',\"session\":' .. json_encode(session_object())\n"
    "      .. ',\"activeImage\":' .. json_encode(active_image_object(image))\n"
    "      .. ',\"snapshot\":' .. snapshot_json\n"
    "      .. '}'\n"
    "  end\n"
    "\n"
    "  local function exposure_action()\n"
    "    if bridge.exposureAction then return bridge.exposureAction end\n"
    "    local candidates = { 'iop/exposure/exposure', 'exposure/exposure' }\n"
    "    for _, candidate in ipairs(candidates) do\n"
    "      local ok, value = pcall(darktable.gui.action, candidate, '', '', 0)\n"
    "      if ok and type(value) == 'number' and value == value then\n"
    "        bridge.exposureAction = candidate\n"
    "        return candidate\n"
    "      end\n"
    "    end\n"
    "    error('exposure action unavailable')\n"
    "  end\n"
    "\n"
    "  local function set_exposure(requested)\n"
    "    local image, reason = current_image()\n"
    "    if not image then return unavailable(reason) end\n"
    "\n"
    "    local action = bridge.exposure_action()\n"
    "    local previous_position = read_exposure_action(action)\n"
    "    local current_position = previous_position\n"
    "    local requested_position = exposure_ev_to_position(requested)\n"
    "\n"
    "    local sequence_before = bridge.renderSequence\n"
    "    local history_before = bridge.historyChangeSequence\n"
    "    if not numbers_equal(exposure_position_to_ev(previous_position), requested) then\n"
    "      write_exposure_action(action, requested)\n"
    "    end\n"
    "\n"
    "    local requested_render_sequence\n"
    "    current_position, requested_render_sequence = wait_for_exposure_settle(image.id, action, requested_position, sequence_before, history_before)\n"
    "    if requested_render_sequence == nil then requested_render_sequence = bridge.renderSequence end\n"
    "\n"
    "    local payload = available_payload(image)\n"
    "    payload.exposure = {\n"
    "      previous = exposure_position_to_ev(previous_position),\n"
    "      requested = requested,\n"
    "      current = exposure_position_to_ev(current_position),\n"
    "      requestedRenderSequence = requested_render_sequence\n"
    "    }\n"
    "    return payload\n"
    "  end\n"
    "\n"
    "  local function exposure_control_metadata()\n"
    "    return {\n"
    "      control = 'exposure',\n"
    "      id = 'exposure.exposure',\n"
    "      module = 'exposure',\n"
    "      operations = { 'get', 'set' },\n"
    "      requires = { activeImage = true, view = 'darkroom' },\n"
    "      valueType = { maximum = 4, minimum = -3, type = 'number' }\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function exposure_control_object()\n"
    "    local control = exposure_control_metadata()\n"
    "    control.value = exposure_position_to_ev(read_exposure_action(bridge.exposure_action()))\n"
    "    return control\n"
    "  end\n"
    "\n"
    "  local function set_exposure_control(requested)\n"
    "    local image, reason = current_image()\n"
    "    if not image then return unavailable(reason) end\n"
    "\n"
    "    local action = bridge.exposure_action()\n"
    "    local previous_position = read_exposure_action(action)\n"
    "    local current_position = previous_position\n"
    "    local requested_position = exposure_ev_to_position(requested)\n"
    "\n"
    "    local sequence_before = bridge.renderSequence\n"
    "    local history_before = bridge.historyChangeSequence\n"
    "    if not numbers_equal(exposure_position_to_ev(previous_position), requested) then\n"
    "      write_exposure_action(action, requested)\n"
    "    end\n"
    "\n"
    "    local requested_render_sequence\n"
    "    current_position, requested_render_sequence = wait_for_exposure_settle(image.id, action, requested_position, sequence_before, history_before)\n"
    "    if requested_render_sequence == nil then requested_render_sequence = bridge.renderSequence end\n"
    "\n"
    "    local control = exposure_control_metadata()\n"
    "    control.value = exposure_position_to_ev(current_position)\n"
    "\n"
    "    return {\n"
    "      activeImage = active_image_object(image),\n"
    "      bridgeVersion = bridge.bridgeVersion,\n"
    "      change = {\n"
    "        current = exposure_position_to_ev(current_position),\n"
    "        previous = exposure_position_to_ev(previous_position),\n"
    "        requested = requested,\n"
    "        requestedRenderSequence = requested_render_sequence\n"
    "      },\n"
    "      control = control,\n"
    "      session = session_object(),\n"
    "      status = 'ok'\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function exposure_control_adapter()\n"
    "    return {\n"
    "      get = function()\n"
    "        local image, reason = current_image()\n"
    "        if not image then return unavailable(reason) end\n"
    "        return {\n"
    "          activeImage = active_image_object(image),\n"
    "          bridgeVersion = bridge.bridgeVersion,\n"
    "          control = exposure_control_object(),\n"
    "          session = session_object(),\n"
    "          status = 'ok'\n"
    "        }\n"
    "      end,\n"
    "      list = function()\n"
    "        return exposure_control_metadata()\n"
    "      end,\n"
    "      snapshot = function()\n"
    "        return exposure_control_object()\n"
    "      end,\n"
    "      set = function(value)\n"
    "        return set_exposure_control(value)\n"
    "      end\n"
    "    }\n"
    "  end\n"
    "\n"
    "  control_registry = function()\n"
    "    return { ['exposure.exposure'] = exposure_control_adapter() }\n"
    "  end\n"
    "\n"
    "  local function lookup_control(control_id)\n"
    "    return control_registry()[control_id]\n"
    "  end\n"
    "\n"
    "  local function list_controls()\n"
    "    local controls = {}\n"
    "    for _, control_id in ipairs({ 'exposure.exposure' }) do\n"
    "      controls[#controls + 1] = control_registry()[control_id].list()\n"
    "    end\n"
    "    return {\n"
    "      bridgeVersion = bridge.bridgeVersion,\n"
    "      controls = controls,\n"
    "      session = session_object(),\n"
    "      status = 'ok'\n"
    "    }\n"
    "  end\n"
    "\n"
    "  local function get_control(control_id)\n"
    "    local adapter = lookup_control(control_id)\n"
    "    if not adapter then return unavailable('unsupported-control', control_id) end\n"
    "    local response = adapter.get()\n"
    "    if response and response.status == 'unavailable' and response.requestedControlId == nil then\n"
    "      response.requestedControlId = control_id\n"
    "    end\n"
    "    return response\n"
    "  end\n"
    "\n"
     "  local function set_control(control_id, value)\n"
     "    local adapter = lookup_control(control_id)\n"
     "    if not adapter then return unavailable('unsupported-control', control_id) end\n"
     "    local response = adapter.set(value)\n"
    "    if response and response.status == 'unavailable' and response.requestedControlId == nil then\n"
    "      response.requestedControlId = control_id\n"
     "    end\n"
     "    return response\n"
     "  end\n"
     "\n"
      "  local function apply_module_instance_action(instance_key, action, anchor_instance_key)\n"
      "    local darkroom = darktable.gui.views.darkroom\n"
      "    local response_json = darkroom and darkroom.live_apply_module_instance_action\n"
      "      and darkroom.live_apply_module_instance_action(instance_key, action, anchor_instance_key) or nil\n"
      "    if type(response_json) ~= 'string' or response_json == '' then\n"
      "      return json_encode({\n"
      "        bridgeVersion = bridge.bridgeVersion,\n"
      "        moduleAction = { action = action, anchorInstanceKey = anchor_instance_key, targetInstanceKey = instance_key },\n"
      "        reason = 'unsupported-module-state',\n"
      "        session = session_object(),\n"
     "        status = 'unavailable'\n"
     "      })\n"
     "    end\n"
     "\n"
     "    if string.sub(response_json, 1, 1) ~= '{' or string.sub(response_json, -1) ~= '}' then\n"
     "      error('module action payload malformed')\n"
     "    end\n"
     "\n"
      "    return '{'\n"
      "      .. '\"bridgeVersion\":' .. json_encode(bridge.bridgeVersion)\n"
      "      .. ',\"session\":' .. json_encode(session_object())\n"
      "      .. ',' .. string.sub(response_json, 2)\n"
      "  end\n"
      "\n"
       "  local function apply_module_instance_blend(instance_key, blend_json)\n"
      "    local darkroom = darktable.gui.views.darkroom\n"
      "    local response_json = darkroom and darkroom.live_apply_module_instance_blend\n"
      "      and darkroom.live_apply_module_instance_blend(instance_key, blend_json) or nil\n"
      "    if type(response_json) ~= 'string' or response_json == '' then\n"
      "      return json_encode({\n"
      "        bridgeVersion = bridge.bridgeVersion,\n"
      "        moduleBlend = { targetInstanceKey = instance_key },\n"
      "        reason = 'unsupported-module-blend',\n"
      "        session = session_object(),\n"
      "        status = 'unavailable'\n"
      "      })\n"
      "    end\n"
      "\n"
      "    if string.sub(response_json, 1, 1) ~= '{' or string.sub(response_json, -1) ~= '}' then\n"
      "      error('module blend payload malformed')\n"
      "    end\n"
      "\n"
       "    return '{'\n"
       "      .. '\"bridgeVersion\":' .. json_encode(bridge.bridgeVersion)\n"
       "      .. ',\"session\":' .. json_encode(session_object())\n"
       "      .. ',' .. string.sub(response_json, 2)\n"
       "  end\n"
       "\n"
       "  local function apply_module_instance_mask(instance_key, mask_json)\n"
       "    local darkroom = darktable.gui.views.darkroom\n"
       "    local response_json = darkroom and darkroom.live_apply_module_instance_mask\n"
       "      and darkroom.live_apply_module_instance_mask(instance_key, mask_json) or nil\n"
       "    if type(response_json) ~= 'string' or response_json == '' then\n"
       "      return json_encode({\n"
       "        bridgeVersion = bridge.bridgeVersion,\n"
       "        moduleMask = { targetInstanceKey = instance_key },\n"
       "        reason = 'unsupported-module-mask',\n"
       "        session = session_object(),\n"
       "        status = 'unavailable'\n"
       "      })\n"
       "    end\n"
       "\n"
       "    if string.sub(response_json, 1, 1) ~= '{' or string.sub(response_json, -1) ~= '}' then\n"
       "      error('module mask payload malformed')\n"
       "    end\n"
       "\n"
       "    return '{'\n"
       "      .. '\"bridgeVersion\":' .. json_encode(bridge.bridgeVersion)\n"
       "      .. ',\"session\":' .. json_encode(session_object())\n"
       "      .. ',' .. string.sub(response_json, 2)\n"
       "  end\n"
       "\n"
       "  local function on_view_changed(_, _, new_view)\n"
     "    bridge.view = new_view and tostring(new_view) or ''\n"
     "  end\n"
    "\n"
    "  local function on_image_loaded()\n"
    "    bridge.imageLoadSequence = bridge.imageLoadSequence + 1\n"
    "    update_view()\n"
    "  end\n"
    "\n"
    "  local function on_history_changed()\n"
    "    bridge.historyChangeSequence = bridge.historyChangeSequence + 1\n"
    "  end\n"
    "\n"
    "  local function on_pixelpipe_complete()\n"
    "    bridge.renderSequence = bridge.renderSequence + 1\n"
    "  end\n"
    "\n"
    "  local function register_bridge_event(event_name, callback)\n"
    "    local ok, err = pcall(darktable.register_event, 'darktable-live-bridge', event_name, callback)\n"
    "    if ok then return end\n"
    "    ok, err = pcall(darktable.register_event, event_name, callback)\n"
    "    if ok then return end\n"
    "    error(err)\n"
    "  end\n"
    "\n"
    "  register_bridge_event('view-changed', on_view_changed)\n"
    "  register_bridge_event('darkroom-image-loaded', on_image_loaded)\n"
    "  register_bridge_event('darkroom-image-history-changed', on_history_changed)\n"
    "  register_bridge_event('pixelpipe-processing-complete', on_pixelpipe_complete)\n"
    "\n"
    "  bridge.update_view = update_view\n"
    "  bridge.current_image = current_image\n"
    "  bridge.exposure_action = exposure_action\n"
    "  bridge.session_object = session_object\n"
    "  bridge.get_session = get_session\n"
    "  bridge.get_snapshot_json = get_snapshot_json\n"
      "  bridge.list_controls = list_controls\n"
      "  bridge.get_control = get_control\n"
       "  bridge.set_control = set_control\n"
       "  bridge.set_exposure = set_exposure\n"
       "  bridge.apply_module_instance_action = apply_module_instance_action\n"
       "  bridge.apply_module_instance_blend = apply_module_instance_blend\n"
       "  bridge.apply_module_instance_mask = apply_module_instance_mask\n"
       "  bridge.json_encode = json_encode\n"
     "  bridge.update_view()\n"
     "  rawset(_G, '" DT_LIVE_BRIDGE_CACHE_KEY "', bridge)\n"
     "end\n"
     "\n"
     "local command = %s\n"
      "local control_id = %s\n"
      "local instance_key = %s\n"
      "local module_action = %s\n"
       "local anchor_instance_key = %s\n"
       "local numeric_value = %s\n"
      "local blend_json = %s\n"
      "local mask_json = %s\n"
      "local response\n"
     "if command == 'get-session' then\n"
     "  response = bridge.get_session()\n"
    "elseif command == 'get-snapshot' then\n"
    "  return bridge.get_snapshot_json()\n"
    "elseif command == 'list-controls' then\n"
    "  response = bridge.list_controls()\n"
    "elseif command == 'get-control' then\n"
    "  response = bridge.get_control(control_id)\n"
    "elseif command == 'set-control' then\n"
     "  response = bridge.set_control(control_id, numeric_value)\n"
      "elseif command == 'set-exposure' then\n"
      "  response = bridge.set_exposure(numeric_value)\n"
       "elseif command == 'apply-module-instance-action' then\n"
       "  return bridge.apply_module_instance_action(instance_key, module_action, anchor_instance_key)\n"
       "elseif command == 'apply-module-instance-blend' then\n"
       "  return bridge.apply_module_instance_blend(instance_key, blend_json)\n"
       "elseif command == 'apply-module-instance-mask' then\n"
       "  return bridge.apply_module_instance_mask(instance_key, mask_json)\n"
       "else\n"
     "  error('unknown command: ' .. tostring(command))\n"
    "end\n"
    "return bridge.json_encode(response)\n";

  g_autofree gchar *command_literal = lua_string_literal(command);
  g_autofree gchar *control_literal = control_id != NULL ? lua_string_literal(control_id) : g_strdup("nil");
  g_autofree gchar *instance_literal = instance_key != NULL ? lua_string_literal(instance_key) : g_strdup("nil");
  g_autofree gchar *module_action_literal =
    module_action != NULL ? lua_string_literal(module_action) : g_strdup("nil");
  g_autofree gchar *anchor_instance_literal =
    anchor_instance_key != NULL ? lua_string_literal(anchor_instance_key) : g_strdup("nil");
  g_autofree gchar *blend_json_literal = blend_json != NULL ? lua_string_literal(blend_json) : g_strdup("nil");
  g_autofree gchar *mask_json_literal = mask_json != NULL ? lua_string_literal(mask_json) : g_strdup("nil");
  const char *numeric_literal = "nil";
  gchar numeric_buffer[G_ASCII_DTOSTR_BUF_SIZE] = { 0 };

  if(have_numeric_value)
  {
    g_ascii_dtostr(numeric_buffer, sizeof(numeric_buffer), numeric_value);
    numeric_literal = numeric_buffer;
  }

  return g_strdup_printf(lua_template, command_literal, control_literal, instance_literal,
                          module_action_literal, anchor_instance_literal, numeric_literal,
                          blend_json_literal, mask_json_literal);
}

int main(int argc, char **argv)
{
  const char *progname = argc > 0 ? argv[0] : "darktable-live-bridge";

  if(argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
  {
    usage(stdout, progname);
    return 0;
  }

  const gchar *command = NULL;
  const gchar *control_id = NULL;
  const gchar *instance_key = NULL;
  const gchar *module_action = NULL;
  const gchar *anchor_instance_key = NULL;
  const gchar *blend_json = NULL;
  const gchar *mask_json = NULL;
  gboolean have_numeric_value = FALSE;
  double numeric_value = 0.0;

  if(argc == 2 && !strcmp(argv[1], "get-session"))
  {
    command = "get-session";
  }
  else if(argc == 2 && !strcmp(argv[1], "get-snapshot"))
  {
    command = "get-snapshot";
  }
  else if(argc == 2 && !strcmp(argv[1], "list-controls"))
  {
    command = "list-controls";
  }
  else if(argc == 3 && !strcmp(argv[1], "get-control"))
  {
    command = "get-control";
    control_id = argv[2];
  }
  else if(argc == 4 && !strcmp(argv[1], "set-control"))
  {
    g_autoptr(GError) parse_error = NULL;
    if(!validate_json_literal(argv[3], &parse_error))
    {
      fprintf(stderr, "%s\n", parse_error != NULL ? parse_error->message : "invalid control value json");
      return 1;
    }

    control_id = argv[2];
    if(g_strcmp0(control_id, "exposure.exposure") == 0)
    {
      g_clear_error(&parse_error);
      if(!parse_json_number_literal(argv[3], &numeric_value, &parse_error))
      {
        fprintf(stderr, "%s\n", parse_error != NULL ? parse_error->message : "invalid control value json");
        return 1;
      }

      g_clear_error(&parse_error);
      if(!validate_exposure_value(numeric_value, "exposure control value", &parse_error))
      {
        fprintf(stderr, "%s\n", parse_error != NULL ? parse_error->message : "invalid control value json");
        return 1;
      }

      have_numeric_value = TRUE;
    }

    command = "set-control";
  }
  else if(argc == 3 && !strcmp(argv[1], "set-exposure"))
  {
    char *endptr = NULL;
    errno = 0;
    numeric_value = g_ascii_strtod(argv[2], &endptr);
    if(errno != 0 || endptr == argv[2] || (endptr && *endptr != '\0') || !isfinite(numeric_value))
    {
      fprintf(stderr, "invalid exposure value\n");
      return 1;
    }

    g_autoptr(GError) parse_error = NULL;
    if(!validate_exposure_value(numeric_value, "exposure value", &parse_error))
    {
      fprintf(stderr, "%s\n", parse_error != NULL ? parse_error->message : "invalid exposure value");
      return 1;
    }

    command = "set-exposure";
    have_numeric_value = TRUE;
  }
  else if((argc == 4 || argc == 5) && !strcmp(argv[1], "apply-module-instance-action"))
  {
    command = "apply-module-instance-action";
    instance_key = argv[2];
    module_action = argv[3];

    if(argc == 5)
    {
      if(strcmp(module_action, "move-before") && strcmp(module_action, "move-after"))
      {
        usage(stderr, progname);
        return 1;
      }
      anchor_instance_key = argv[4];
    }
    else if(!strcmp(module_action, "move-before") || !strcmp(module_action, "move-after"))
    {
      usage(stderr, progname);
      return 1;
    }
  }
  else if(argc == 4 && !strcmp(argv[1], "apply-module-instance-blend"))
  {
    dt_live_bridge_blend_request_t blend_request = { 0 };
    g_autoptr(GError) parse_error = NULL;
    if(!parse_blend_json(argv[3], &blend_request, &parse_error))
    {
      fprintf(stderr, "%s\n", parse_error != NULL ? parse_error->message : "invalid blend json");
      return 1;
    }

    command = "apply-module-instance-blend";
    instance_key = argv[2];
    blend_json = argv[3];
  }
  else if(argc == 4 && !strcmp(argv[1], "apply-module-instance-mask"))
  {
    dt_live_bridge_mask_request_t mask_request = { 0 };
    g_autoptr(GError) parse_error = NULL;
    if(!parse_mask_json(argv[3], &mask_request, &parse_error))
    {
      fprintf(stderr, "%s\n", parse_error != NULL ? parse_error->message : "invalid mask json");
      return 1;
    }

    command = "apply-module-instance-mask";
    instance_key = argv[2];
    mask_json = argv[3];
  }
  else
  {
    usage(stderr, progname);
    return 1;
  }

  g_autofree gchar *lua_source =
    build_lua_command(command, control_id, instance_key, module_action, anchor_instance_key,
                      have_numeric_value, numeric_value, blend_json, mask_json);
  if(lua_source == NULL)
  {
    fprintf(stderr, "failed to build Lua command\n");
    return 1;
  }

  g_autofree gchar *json_result = NULL;
  g_autoptr(GError) error = NULL;
  if(!call_lua(lua_source, &json_result, &error))
  {
    fprintf(stderr, "%s\n", error != NULL ? error->message : "DBus call failed");
    return 1;
  }

  if(json_result == NULL || json_result[0] == '\0')
  {
    fprintf(stderr, "darktable returned an empty response\n");
    return 1;
  }

  if(!print_json_only(json_result, &error))
  {
    fprintf(stderr, "%s\n", error != NULL ? error->message : "invalid JSON response");
    return 1;
  }

  return 0;
}
