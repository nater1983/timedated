/*
  Copyright 2012 Alexandre Rostovtsev

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

  Extracted from src/utils.h. Modified in 2019 by Pierre Labastie.
  See git log
*/

#ifndef _SHELL_PARSER_H_
#define _SHELL_PARSER_H_

#include <glib.h>
#include <gio/gio.h>

/**
 * SECTION: shellparser
 * @short_description: A variable=value shell parser
 * @title: Shell Parser
 * @include: shellparser.h
 *
 * The shellparser API solves the following problem: given a file containing
 * various records, including variable assignments in shell syntax, be able to:
 *
 * - read the file
 *
 * - change a variable value
 *
 * - add a new variable and its value
 *
 * - write the changes to the file, keeping the other records intact
 */

/**
 * ShellParser:
 * @file: the file that is parsed
 * @filename: its filename
 * @entry_list: a list of <structname>struct ShellEntry</structname>
 *
 * ShellParser holds the content of the file parsed to a list of
 * <structname>ShellEntry</structname>. The various set/clear functions
 * act on this structure, which is otherwise private.
 */

typedef struct _ShellParser ShellParser;

struct _ShellParser
{
  GFile *file;
  gchar *filename;
  GList *entry_list;
};

/* Always return TRUE */
gboolean
_g_match_info_clear (GMatchInfo **match_info);

gchar *
strstr0 (const gchar *haystack, const gchar *needle);

ShellParser *
shell_parser_new (GFile *file,
                  GError **error);

ShellParser *
shell_parser_new_from_string (GFile *file,
                              gchar *filebuf,
                              GError **error);

void
shell_parser_free (ShellParser *parser);

gboolean
shell_parser_is_empty (ShellParser *parser);

gboolean
shell_parser_set_variable (ShellParser *parser,
                           const gchar *variable,
                           const gchar *value,
                           gboolean     add_if_unset);

void
shell_parser_clear_variable (ShellParser *parser,
                             const gchar *variable);

gboolean
shell_parser_save (ShellParser *parser,
                   GError **error);

gboolean
shell_parser_set_and_save (GFile *file,
                           GError **error,
                           const gchar *first_var_name,
                           const gchar *first_alt_var_name,
                           const gchar *first_value,
                           ...);

gchar **
shell_parser_source_var_list (GFile *file,
                              const gchar * const *var_names,
                              GError **error);

/*void
shell_parser_init (void);*/

void
shell_parser_destroy (void);

#endif
