/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * broker_log_util.c -
 */

#ident "$Id$"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <sys/time.h>

#include "cas_common.h"
#include "broker_log_util.h"
#include "cas_cci.h"

static bool is_bind_with_size (char *buf, int *tot_val_size, int *info_size);

void
ut_tolower (char *str)
{
  char *p;

  if (str == NULL)
    return;

  for (p = str; *p; p++)
    {
      if (*p >= 'A' && *p <= 'Z')
	*p = *p - 'A' + 'a';
    }
}

#if defined(BROKER_LOG_RUNNER)
static bool
is_bind_with_size (char *buf, int *tot_val_size, int *info_size)
{
  char *p;
  int type;
  int size;

  if (tot_val_size)
    {
      *tot_val_size = 0;
    }
  if (info_size)
    {
      *info_size = 0;
    }

  if (strncmp (buf, "B ", 1) != 0)
    {
      return false;
    }

  type = atoi (buf + 2);
  if ((type != CCI_U_TYPE_VARCHAR) && (type != CCI_U_TYPE_VARBIT))
    {
      return false;
    }

  p = strchr (buf + 2, ' ');
  if (p == NULL)
    {
      goto error_on_val_size;
    }

  size = atoi (p);
  p = strchr (p + 1, ' ');
  if (p == NULL)
    {
      goto error_on_val_size;
    }

  if (info_size)
    {
      *info_size = (char *) (p + 1) - (char *) buf;
    }
  if (tot_val_size)
    {
      *tot_val_size = size;
    }
  return true;

error_on_val_size:
  if (tot_val_size)
    {
      *tot_val_size = -1;
    }
  return false;
}
#else /* BROKER_LOG_RUNNER */
static bool
is_bind_with_size (char *buf, int *tot_val_size, int *info_size)
{
  const char *msg;
  char *p, *q;
  char size[256];
  char *value_p;
  char *size_begin;
  char *size_end;
  char *info_end;
  int len;

  size[0] = '\0';		/* init */

  if (info_size)
    {
      *info_size = 0;
    }
  if (tot_val_size)
    {
      *tot_val_size = 0;
    }

  msg = get_msg_start_ptr (buf);
  if (strncmp (msg, "bind ", 5) != 0)
    {
      return false;
    }

  p = strchr (msg, ':');
  if (p == NULL)
    {
      return false;
    }
  p += 2;

  if ((strncmp (p, "CHAR", 4) != 0) && (strncmp (p, "VARCHAR", 7) != 0)
      && (strncmp (p, "NCHAR", 5) != 0) && (strncmp (p, "VARNCHAR", 8) != 0)
      && (strncmp (p, "BINARY", 3) != 0)
      && (strncmp (p, "VARBINARY", 6) != 0))
    {
      return false;
    }

  q = strchr (p, ' ');
  if (q == NULL)
    {
      /* log error case or NULL bind type */
      return false;
    }

  *q = '\0';
  value_p = q + 1;

  size_begin = strstr (value_p, "(");
  if (size_begin == NULL)
    {
      goto error_on_val_size;
    }
  size_begin += 1;
  size_end = strstr (value_p, ")");
  if (size_end == NULL)
    {
      goto error_on_val_size;
    }

  info_end = size_end + 1;

  if (info_size)
    {
      *info_size = (char *) info_end - (char *) buf;
    }
  if (tot_val_size)
    {
      len = size_end - size_begin;
      if (len >= (int) sizeof (size))
	{
	  goto error_on_val_size;
	}
      if (len > 0)
	{
	  memcpy (size, size_begin, len);
	  size[len] = '\0';
	}
      *tot_val_size = atoi (size);
      if (*tot_val_size < 0)
	{
	  goto error_on_val_size;
	}
    }

  return true;

error_on_val_size:
  if (info_size)
    {
      *info_size = -1;
    }
  if (tot_val_size)
    {
      *tot_val_size = -1;
    }
  return false;
}
#endif /* BROKER_LOG_RUNNER */

int
ut_get_line (FILE * fp, T_STRING * t_str, char **out_str, int *lineno)
{
  char buf[1024];
  int out_str_len;
  bool is_first, bind_with_size = false;
  int tot_val_size = 0, info_size = 0;
  long position;

  t_string_clear (t_str);

  is_first = true;
  while (1)
    {
      memset (buf, 0, sizeof (buf));
      position = ftell (fp);
      if (fgets (buf, sizeof (buf), fp) == NULL)
	break;
      /* if it is (debug) line, skip it */
      if (strncmp (buf + 19, "(debug)", 7) == 0)
	{
	  continue;
	}
      if (is_first)
	{
	  bind_with_size = is_bind_with_size (buf, &tot_val_size, &info_size);
	  if (tot_val_size < 0 || info_size < 0
	      || (tot_val_size + info_size + 1) < 0)
	    {
	      fprintf (stderr, "log error\n");
	      return -1;
	    }
	  is_first = false;
	}

      if (bind_with_size)
	{
	  size_t rlen;
	  char *value = NULL;

	  value = (char *) RYE_MALLOC (info_size + tot_val_size + 1);
	  if (value == NULL)
	    {
	      fprintf (stderr, "memory allocation error.\n");
	      return -1;
	    }
	  fseek (fp, position, SEEK_SET);
	  rlen =
	    fread ((void *) value, sizeof (char), info_size + tot_val_size,
		   fp);
	  if (t_bind_string_add
	      (t_str, value, info_size + tot_val_size, tot_val_size) < 0)
	    {
	      fprintf (stderr, "memory allocation error.\n");
	      RYE_FREE_MEM (value);
	      return -1;
	    }
	  RYE_FREE_MEM (value);
	  break;
	}
      else
	{
	  if (t_string_add (t_str, buf, strlen (buf)) < 0)
	    {
	      fprintf (stderr, "memory allocation error.\n");
	      return -1;
	    }
	  if (buf[sizeof (buf) - 2] == '\0' || buf[sizeof (buf) - 2] == '\n')
	    break;
	}
    }

  out_str_len = t_string_len (t_str);
  if (out_str)
    *out_str = t_string_str (t_str);
  if (lineno)
    *lineno = *lineno + 1;
  return out_str_len;
}

bool
is_cas_log (const char *str)
{
  if (strlen (str) < CAS_LOG_MSG_INDEX)
    {
      return false;
    }

  if (str[4] == '-' && str[7] == '-' && str[10] == ' ' && str[13] == ':' &&
      str[16] == ':' && str[23] == ' ')
    {
      return true;
    }

  return false;
}

const char *
get_msg_start_ptr (char *linebuf)
{
  const char *tmp_ptr;

  tmp_ptr = linebuf + CAS_LOG_MSG_INDEX;

  tmp_ptr = strchr (tmp_ptr, ' ');
  if (tmp_ptr == NULL)
    {
      return "";
    }
  else
    {
      return tmp_ptr + 1;
    }
}

#define  DATE_VALUE_COUNT 7
int
str_to_log_date_format (char *str, char *date_format_str)
{
  char *startp;
  char *endp;
  int i;
  int result = 0;
  int val;
  int date_val[DATE_VALUE_COUNT];

  for (i = 0; i < DATE_VALUE_COUNT; i++)
    date_val[i] = 0;

  for (i = 0, startp = str; i < DATE_VALUE_COUNT; i++)
    {
      result = str_to_int32 (&val, &endp, startp, 10);
      if (result != 0)
	{
	  goto error;
	}
      if (val < 0)
	{
	  val = 0;
	}
      date_val[i] = val;
      if (*endp == '\0')
	{
	  break;
	}
      startp = endp + 1;
      if (*startp == '\0')
	{
	  break;
	}
    }

  sprintf (date_format_str,
	   "%d-%02d-%02d %02d:%02d:%02d.%03d",
	   date_val[0], date_val[1], date_val[2], date_val[3], date_val[4],
	   date_val[5], date_val[6]);
  return 0;

error:
  return -1;
}

const char *
ut_get_execute_type (const char *msg_p, int *prepare_flag, int *execute_flag)
{
  if (strncmp (msg_p, "execute ", 8) == 0)
    {
      *prepare_flag = 0;
      *execute_flag = 0;
      return (msg_p += 8);
    }
  else
    {
      return NULL;
    }
}

int
ut_check_log_valid_time (const char *log_date, const char *from_date,
			 const char *to_date)
{
  if (from_date[0])
    {
      if (strncmp (log_date, from_date, DATE_STR_LEN) < 0)
	return -1;
    }
  if (to_date[0])
    {
      if (strncmp (to_date, log_date, DATE_STR_LEN) < 0)
	return -1;
    }

  return 0;
}

double
ut_diff_time (struct timeval *begin, struct timeval *end)
{
  double sec, usec;

  sec = (end->tv_sec - begin->tv_sec);
  usec = (double) (end->tv_usec - begin->tv_usec) / 1000000;
  return (sec + usec);
}
