/**
 * collectd - src/utils_cmd_putval.c
 * Copyright (C) 2007-2009  Florian octo Forster
 * Copyright (C) 2016       Sebastian tokkee Harl
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 *   Sebastian tokkee Harl <sh at tokkee.org>
 **/

#include "collectd.h"

#include "common.h"
#include "plugin.h"

#include "utils_cmds.h"
#include "utils_cmd_putval.h"
#include "utils_parse_option.h"
#include "utils_cmd_putval.h"

/*
 * private helper functions
 */

static int set_option (value_list_t *vl, const char *key, const char *value)
{
	if ((vl == NULL) || (key == NULL) || (value == NULL))
		return (-1);

	if (strcasecmp ("interval", key) == 0)
	{
		double tmp;
		char *endptr;

		endptr = NULL;
		errno = 0;
		tmp = strtod (value, &endptr);

		if ((errno == 0) && (endptr != NULL)
				&& (endptr != value) && (tmp > 0.0))
			vl->interval = DOUBLE_TO_CDTIME_T (tmp);
	}
	else
		return (1);

	return (0);
} /* int set_option */

/*
 * public API
 */

cmd_status_t cmd_parse_putval (char *buffer,
		cmd_putval_t *ret_putval, cmd_error_handler_t *err)
{
	char *identifier;
	char *hostname;
	char *plugin;
	char *plugin_instance;
	char *type;
	char *type_instance;
	int   status;

	char *identifier_copy;

	const data_set_t *ds;
	value_list_t vl = VALUE_LIST_INIT;
	vl.values = NULL;

	identifier = NULL;
	status = parse_string (&buffer, &identifier);
	if (status != 0)
	{
		cmd_error (CMD_PARSE_ERROR, err, "Cannot parse identifier.");
		return (CMD_PARSE_ERROR);
	}
	assert (identifier != NULL);

	/* parse_identifier() modifies its first argument,
	 * returning pointers into it */
	identifier_copy = sstrdup (identifier);

	status = parse_identifier (identifier_copy, &hostname,
			&plugin, &plugin_instance,
			&type, &type_instance);
	if (status != 0)
	{
		DEBUG ("cmd_handle_putval: Cannot parse identifier `%s'.",
				identifier);
		cmd_error (CMD_PARSE_ERROR, err, "Cannot parse identifier `%s'.",
				identifier);
		sfree (identifier_copy);
		return (CMD_PARSE_ERROR);
	}

	if ((strlen (hostname) >= sizeof (vl.host))
			|| (strlen (plugin) >= sizeof (vl.plugin))
			|| ((plugin_instance != NULL)
				&& (strlen (plugin_instance) >= sizeof (vl.plugin_instance)))
			|| ((type_instance != NULL)
				&& (strlen (type_instance) >= sizeof (vl.type_instance))))
	{
		cmd_error (CMD_PARSE_ERROR, err, "Identifier too long.");
		sfree (identifier_copy);
		return (CMD_PARSE_ERROR);
	}

	sstrncpy (vl.host, hostname, sizeof (vl.host));
	sstrncpy (vl.plugin, plugin, sizeof (vl.plugin));
	sstrncpy (vl.type, type, sizeof (vl.type));
	if (plugin_instance != NULL)
		sstrncpy (vl.plugin_instance, plugin_instance, sizeof (vl.plugin_instance));
	if (type_instance != NULL)
		sstrncpy (vl.type_instance, type_instance, sizeof (vl.type_instance));

	ds = plugin_get_ds (type);
	if (ds == NULL)
	{
		cmd_error (CMD_PARSE_ERROR, err, "1 Type `%s' isn't defined.", type);
		sfree (identifier_copy);
		return (CMD_PARSE_ERROR);
	}

	/* Free identifier_copy */
	hostname = NULL;
	plugin = NULL; plugin_instance = NULL;
	type = NULL;   type_instance = NULL;
	sfree (identifier_copy);

	vl.values_len = ds->ds_num;
	vl.values = malloc (vl.values_len * sizeof (*vl.values));
	if (vl.values == NULL)
	{
		cmd_error (CMD_ERROR, err, "malloc failed.");
		return (CMD_ERROR);
	}

	ret_putval->identifier = strdup (identifier);
	if (ret_putval->identifier == NULL)
	{
		cmd_error (CMD_ERROR, err, "malloc failed.");
		cmd_destroy_putval (ret_putval);
		return (CMD_ERROR);
	}

	/* All the remaining fields are part of the optionlist. */
	while (*buffer != 0)
	{
		value_list_t *tmp;

		char *string = NULL;
		char *value  = NULL;

		status = parse_option (&buffer, &string, &value);
		if (status < 0)
		{
			/* parse_option failed, buffer has been modified.
			 * => we need to abort */
			cmd_error (CMD_PARSE_ERROR, err, "Misformatted option.");
			cmd_destroy_putval (ret_putval);
			return (CMD_PARSE_ERROR);
		}
		else if (status == 0)
		{
			assert (string != NULL);
			assert (value != NULL);
			set_option (&vl, string, value);
			continue;
		}
		/* else: parse_option but buffer has not been modified. This is
		 * the default if no `=' is found.. */

		status = parse_string (&buffer, &string);
		if (status != 0)
		{
			cmd_error (CMD_PARSE_ERROR, err, "Misformatted value.");
			cmd_destroy_putval (ret_putval);
			return (CMD_PARSE_ERROR);
		}
		assert (string != NULL);

		status = parse_values (string, &vl, ds);
		if (status != 0)
		{
			cmd_error (CMD_PARSE_ERROR, err, "Parsing the values string failed.");
			cmd_destroy_putval (ret_putval);
			return (CMD_PARSE_ERROR);
		}

		tmp = (value_list_t *) realloc (ret_putval->vl,
				(ret_putval->vl_num + 1) * sizeof(*ret_putval->vl));
		if (tmp == NULL)
		{
			cmd_error (CMD_ERROR, err, "realloc failed.");
			cmd_destroy_putval (ret_putval);
			return (CMD_ERROR);
		}

		ret_putval->vl = tmp;
		ret_putval->vl_num++;
		memcpy (&ret_putval->vl[ret_putval->vl_num - 1], &vl, sizeof (vl));
	} /* while (*buffer != 0) */
	/* Done parsing the options. */

	return (CMD_OK);
} /* cmd_status_t cmd_parse_putval */

void cmd_destroy_putval (cmd_putval_t *putval)
{
	size_t i;

	if (putval == NULL)
		return;

	sfree (putval->identifier);

	for (i = 0; i < putval->vl_num; ++i)
	{
		sfree (putval->vl[i].values);
		meta_data_destroy (putval->vl[i].meta);
		putval->vl[i].meta = NULL;
	}
	sfree (putval->vl);
	putval->vl = NULL;
	putval->vl_num = 0;
} /* void cmd_destroy_putval */

cmd_status_t cmd_handle_putval (FILE *fh, char *buffer)
{
	cmd_error_handler_t err = { cmd_error_fh, fh };
	cmd_t cmd;
	size_t i;

	int status;

	DEBUG ("utils_cmd_putval: cmd_handle_putval (fh = %p, buffer = %s);",
			(void *) fh, buffer);

	if ((status = cmd_parse (buffer, &cmd, &err)) != CMD_OK)
		return (status);
	if (cmd.type != CMD_PUTVAL)
	{
		cmd_error (CMD_UNKNOWN_COMMAND, &err, "Unexpected command: `%s'.",
				CMD_TO_STRING (cmd.type));
		cmd_destroy (&cmd);
		return (CMD_UNKNOWN_COMMAND);
	}

	for (i = 0; i < cmd.cmd.putval.vl_num; ++i)
		plugin_dispatch_values (&cmd.cmd.putval.vl[i]);

	if (fh != stdout)
		cmd_error (CMD_OK, &err, "Success: %i %s been dispatched.",
				(int)cmd.cmd.putval.vl_num,
				(cmd.cmd.putval.vl_num == 1) ? "value has" : "values have");

	cmd_destroy (&cmd);
	return (CMD_OK);
} /* int cmd_handle_putval */

int cmd_create_putval (char *ret, size_t ret_len, /* {{{ */
	const data_set_t *ds, const value_list_t *vl)
{
	char buffer_ident[6 * DATA_MAX_NAME_LEN];
	char buffer_values[1024];
	int status;

	status = FORMAT_VL (buffer_ident, sizeof (buffer_ident), vl);
	if (status != 0)
		return (status);
	escape_string (buffer_ident, sizeof (buffer_ident));

	status = format_values (buffer_values, sizeof (buffer_values),
			ds, vl, /* store rates = */ 0);
	if (status != 0)
		return (status);
	escape_string (buffer_values, sizeof (buffer_values));

	ssnprintf (ret, ret_len,
			"PUTVAL %s interval=%.3f %s",
			buffer_ident,
			(vl->interval > 0)
			? CDTIME_T_TO_DOUBLE (vl->interval)
			: CDTIME_T_TO_DOUBLE (plugin_get_interval ()),
			buffer_values);

	return (0);
} /* }}} int cmd_create_putval */
