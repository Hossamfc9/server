/*
   Copyright (c) 2002, 2013, Oracle and/or its affiliates
   Copyright (c) 2009, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include <mysys_priv.h>
#include <my_default.h>
#include <m_string.h>
#include <ctype.h>
#include <stdlib.h>
#include <mysys_err.h>
#include <my_getopt.h>
#include <errno.h>

my_bool is_file_marker(const char* arg);
typedef void (*init_func_p)(const struct my_option *option, void *variable,
                            longlong value);

static void default_reporter(enum loglevel level, const char *format, ...);
my_error_reporter my_getopt_error_reporter= &default_reporter;

static int findopt(char *, uint, const struct my_option **, const char **);
static my_bool getopt_compare_strings(const char *, const char *, uint);
static longlong getopt_ll(char *arg, const struct my_option *optp, int *err);
static ulonglong getopt_ull(char *, const struct my_option *, int *);
static double getopt_double(char *arg, const struct my_option *optp, int *err);
static void init_variables(const struct my_option *, init_func_p);
static void fini_one_value(const struct my_option *, void *, longlong);
static int setval(const struct my_option *, void *, char *, my_bool, const char *);
static char *check_struct_option(char *cur_arg, char *key_name);

/*
  The following three variables belong to same group and the number and
  order of their arguments must correspond to each other.
*/
static const char *special_opt_prefix[]=
{"skip", "disable", "enable", "maximum", "loose", "autoset", 0};
static const uint special_opt_prefix_lengths[]=
{ 4,      7,         6,        7,         5,       7,        0};
enum enum_special_opt
{ OPT_SKIP, OPT_DISABLE, OPT_ENABLE, OPT_MAXIMUM, OPT_LOOSE, OPT_AUTOSET};

char *disabled_my_option= (char*) "0";
char *enabled_my_option= (char*) "1";
char *autoset_my_option= (char*) "auto";

/*
   This is a flag that can be set in client programs. 0 means that
   my_getopt will not print error messages, but the client should do
   it by itself
*/

my_bool my_getopt_print_errors= 1;

/*
   This is a flag that can be set in client programs. 1 means that
   my_getopt will skip over options it does not know how to handle.
*/

my_bool my_getopt_skip_unknown= 0;


/*
   This is a flag that can be set in client programs. 1 means that
   my_getopt will recognize command line options by their unambiguous
   prefixes. 0 means an option must be always specified in full.
*/
my_bool my_getopt_prefix_matching= 1;

/*
   This is a flag that can be set in client programs. 1 means that
   handle_options()  will not initialize options to default values.
*/
my_bool my_handle_options_init_variables = 1;

my_getopt_value my_getopt_get_addr= 0;

ATTRIBUTE_FORMAT(printf, 2, 3)
static void default_reporter(enum loglevel level, const char *format, ...)
{
  va_list args;
  DBUG_ENTER("default_reporter");

  va_start(args, format);
  if (level == WARNING_LEVEL)
    fprintf(stderr, "%s", "Warning: ");
  else if (level == INFORMATION_LEVEL)
    fprintf(stderr, "%s", "Info: ");
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
  fflush(stderr);
  DBUG_VOID_RETURN;
}

union ull_dbl
{
  ulonglong ull;
  double dbl;
};


static inline int cmp_opt_name(const char *a, const char *b)
{
  return my_strcasecmp_latin1(a, b);
}


/**
  Returns an ulonglong value containing a raw
  representation of the given double value.
*/
ulonglong getopt_double2ulonglong(double v)
{
  union ull_dbl u;
  u.dbl= v;
  compile_time_assert(sizeof(ulonglong) >= sizeof(double));
  return u.ull;
}

/**
  Returns the double value which corresponds to
  the given raw representation.
*/
double getopt_ulonglong2double(ulonglong v)
{
  union ull_dbl u;
  u.ull= v;
  return u.dbl;
}

#ifdef _WIN32
/**

 On Windows, if program is running in UTF8 mode, but some arguments are not UTF8.

 This will mostly likely be a sign of old "ANSI" my.ini, and it is likely that
 something will go wrong, e.g file access error.
*/
static void validate_value(const char *key, const char *value,
                           const char *filename)
{
  MY_STRCOPY_STATUS status;
  const struct charset_info_st *cs= &my_charset_utf8mb4_bin;
  size_t len;
  if (GetACP() != CP_UTF8)
    return;
  if (!(len= strlen(value)))
    return;
  cs->cset->well_formed_char_length(cs, value, value + len, len, &status);
  if (!status.m_well_formed_error_pos)
    return;
  if (filename && *filename)
  {
    my_getopt_error_reporter(WARNING_LEVEL,
      "%s: invalid (non-UTF8) characters found for option '%s'"
      " in file '%s'",
      my_progname, key, filename);
  }
  else
  {
    my_getopt_error_reporter(
    WARNING_LEVEL, "%s: invalid (non-UTF8) characters for option %s",
      my_progname, key);
  }
}
#else
#define validate_value(key, value, filename) (void)filename
#endif

#define SET_HO_ERROR_AND_CONTINUE(e) { ho_error= (e); (*argc)--; continue; }

void warn_deprecated(const struct my_option *optp)
{
  char buf1[NAME_CHAR_LEN + 3];
  strxmov(buf1, "--", optp->name, NullS);
  convert_underscore_to_dash(buf1, strlen(buf1));
  if (IS_DEPRECATED_NO_REPLACEMENT(optp->deprecation_substitute))
    my_getopt_error_reporter(WARNING_LEVEL, "%s is deprecated and will be "
      "removed in a future release", buf1);
  else
  {
    char buf2[NAME_CHAR_LEN + 3];
    strxmov(buf2, "--", optp->deprecation_substitute, NullS);
    convert_underscore_to_dash(buf2, strlen(buf2));
    my_getopt_error_reporter(WARNING_LEVEL, "%s is deprecated and will be "
      "removed in a future release. Please use %s instead.", buf1, buf2);
  }
}

/**
  Handle command line options.
  Sort options.
  Put options first, until special end of options (--),
  or until the end of argv. Parse options, check that the given option
  matches with one of the options in struct 'my_option'.
  Check that option was given an argument if it requires one
  Call the 'get_one_option()' function once for each option.

  Note that handle_options() can be invoked multiple times to
  parse a command line in several steps.
  In this case, use the global flag @c my_getopt_skip_unknown to indicate
  that options unknown in the current step should be preserved in the
  command line for later parsing in subsequent steps.

  For 'long' options (--a_long_option), @c my_getopt_skip_unknown is
  fully supported. Command line parameters such as:
  - "--a_long_option"
  - "--a_long_option=value"
  - "--a_long_option value"
  will be preserved as is when the option is not known.

  For 'short' options (-S), support for @c my_getopt_skip_unknown
  comes with some limitation, because several short options
  can also be specified together in the same command line argument,
  as in "-XYZ".

  The first use case supported is: all short options are declared.
  handle_options() will be able to interpret "-XYZ" as one of:
  - an unknown X option
  - "-X -Y -Z", three short options with no arguments
  - "-X -YZ", where Y is a short option with argument Z
  - "-XYZ", where X is a short option with argument YZ
  based on the full short options specifications.

  The second use case supported is: no short option is declared.
  handle_options() will reject "-XYZ" as unknown, to be parsed later.

  The use case that is explicitly not supported is to provide
  only a partial list of short options to handle_options().
  This function can not be expected to extract some option Y
  in the middle of the string "-XYZ" in these conditions,
  without knowing if X will be declared an option later.

  Note that this limitation only impacts parsing of several
  short options from the same command line argument,
  as in "mysqld -anW5".
  When each short option is properly separated out in the command line
  argument, for example in "mysqld -a -n -w5", the code would actually
  work even with partial options specs given at each stage.

  @param [in, out] argc      command line options (count)
  @param [in, out] argv      command line options (values)
  @param [in] longopts       descriptor of all valid options
  @param [in] get_one_option callback function to process each option
  @return error in case of ambiguous or unknown options,
          0 on success.
*/
int handle_options(int *argc, char ***argv, const struct my_option *longopts,
                   my_get_one_option get_one_option)
{
  uint UNINIT_VAR(opt_found), argvpos= 0, length;
  my_bool end_of_options= 0, must_be_var, set_maximum_value,
          option_is_loose, option_is_autoset;
  char **pos, **pos_end, *optend, *opt_str, key_name[FN_REFLEN];
  char *filename= (char*)"";
  const char *UNINIT_VAR(prev_found);
  const struct my_option *optp;
  void *value;
  int ho_error= 0, error, i;
  my_bool is_cmdline_arg= 1;
  DBUG_ENTER("handle_options");

  /* handle_options() assumes arg0 (program name) always exists */
  DBUG_ASSERT(*argc >= 1);
  DBUG_ASSERT(*argv);
  (*argc)--; /* Skip the program name */
  (*argv)++; /*      --- || ----      */
  if (my_handle_options_init_variables)
    init_variables(longopts, my_getopt_init_one_value);

  is_cmdline_arg= !is_file_marker(**argv);

  for (pos= *argv, pos_end=pos+ *argc; pos < pos_end ; pos++)
  {
    char **first= pos;
    char *cur_arg= *pos;
    opt_found= 0;
    if (!is_cmdline_arg)
    {
      if (is_file_marker(cur_arg))
      {
        pos++;
        filename= *pos;
        is_cmdline_arg= *filename == 0; /* empty file name = command line */
        if (my_getopt_skip_unknown)
        {
          (*argv)[argvpos++]= cur_arg;
          (*argv)[argvpos++]= filename;
        }
        else
          (*argc)-= 2;
        continue;
      }
    }
    if (cur_arg[0] == '-' && cur_arg[1] && !end_of_options) /* must be opt */
    {
      char *argument=    0;
      must_be_var=       0;
      set_maximum_value= 0;
      option_is_loose=   0;
      option_is_autoset= 0;

      cur_arg++;		/* skip '-' */
      if (*cur_arg == '-')      /* check for long option, */
      {
        if (!*++cur_arg)	/* skip the double dash */
        {
          /* '--' means end of options, look no further */
          end_of_options= 1;
          (*argc)--;
          continue;
        }
	opt_str= check_struct_option(cur_arg, key_name);
	optend= strcend(opt_str, '=');
	length= (uint) (optend - opt_str);
	if (*optend == '=')
	  optend++;
	else
	  optend= 0;

	/*
	  Find first the right option. Return error in case of an ambiguous,
	  or unknown option
	*/
	optp= longopts;
	if (!(opt_found= findopt(opt_str, length, &optp, &prev_found)))
	{
	  /*
	    Didn't find any matching option. Let's see if someone called
	    option with a special option prefix
	  */
	  if (!must_be_var)
	  {
	    if (optend)
	      must_be_var= 1; /* option is followed by an argument */
	    for (i= 0; special_opt_prefix[i]; i++)
	    {
	      if (!getopt_compare_strings(special_opt_prefix[i], opt_str,
					  special_opt_prefix_lengths[i]) &&
		  (opt_str[special_opt_prefix_lengths[i]] == '-' ||
		   opt_str[special_opt_prefix_lengths[i]] == '_'))
	      {
		/*
		  We were called with a special prefix, we can reuse opt_found
		*/
		opt_str+= special_opt_prefix_lengths[i] + 1;
                length-= special_opt_prefix_lengths[i] + 1;
		if (i == OPT_LOOSE)
		  option_is_loose= 1;
		else if (i == OPT_AUTOSET)
		  option_is_autoset= 1;
		if ((opt_found= findopt(opt_str, length, &optp, &prev_found)))
		{
		  if (opt_found > 1)
		  {
		    if (my_getopt_print_errors)
                      my_getopt_error_reporter(ERROR_LEVEL,
                                               "%s: ambiguous option '--%s-%s' (--%s-%s)",
                                               my_progname, special_opt_prefix[i],
                                               opt_str, special_opt_prefix[i],
                                               prev_found);
		    SET_HO_ERROR_AND_CONTINUE(EXIT_AMBIGUOUS_OPTION)
		  }
		  switch (i) {
		  case OPT_SKIP:
		  case OPT_DISABLE: /* fall through */
		    /*
		      double negation is actually enable again,
		      for example: --skip-option=0 -> option = TRUE
		    */
		    optend= (optend && *optend == '0' && !(*(optend + 1))) ?
		      enabled_my_option : disabled_my_option;
		    break;
		  case OPT_ENABLE:
		    optend= (optend && *optend == '0' && !(*(optend + 1))) ?
                      disabled_my_option : enabled_my_option;
		    break;
		  case OPT_MAXIMUM:
		    set_maximum_value= 1;
		    must_be_var= 1;
		    break;
		  }
		  break; /* break from the inner loop, main loop continues */
		}
                i= -1; /* restart the loop */
	      }
	    }
	  }
	  if (!opt_found)
	  {
            if (my_getopt_skip_unknown)
            {
              /* Preserve all the components of this unknown option. */
              do {
                (*argv)[argvpos++]= *first++;
              } while (first <= pos);
              continue;
            }
	    if (must_be_var)
	    {
	      if (my_getopt_print_errors)
                my_getopt_error_reporter(option_is_loose ? 
                                           WARNING_LEVEL : ERROR_LEVEL,
                                         "%s: unknown variable '%s'",
                                         my_progname, cur_arg);
	      if (!option_is_loose)
		SET_HO_ERROR_AND_CONTINUE(EXIT_UNKNOWN_VARIABLE)
	    }
	    else
	    {
	      if (my_getopt_print_errors)
                my_getopt_error_reporter(option_is_loose ? 
                                           WARNING_LEVEL : ERROR_LEVEL,
                                         "%s: unknown option '--%s'", 
                                         my_progname, cur_arg);
	      if (!option_is_loose)
		SET_HO_ERROR_AND_CONTINUE(EXIT_UNKNOWN_OPTION)
	    }
	    if (option_is_loose)
	    {
	      (*argc)--;
	      continue;
	    }
	  }
	}
	if (opt_found > 1)
	{
	  if (must_be_var)
	  {
	    if (my_getopt_print_errors)
              my_getopt_error_reporter(ERROR_LEVEL,
                                       "%s: variable prefix '%s' is not unique",
                                       my_progname, opt_str);
	    SET_HO_ERROR_AND_CONTINUE(EXIT_VAR_PREFIX_NOT_UNIQUE)
	  }
	  else
	  {
	    if (my_getopt_print_errors)
              my_getopt_error_reporter(ERROR_LEVEL,
                                       "%s: ambiguous option '--%s' (%s, %s)",
                                       my_progname, opt_str, prev_found, 
                                       optp->name);
	    SET_HO_ERROR_AND_CONTINUE(EXIT_AMBIGUOUS_OPTION)
	  }
	}
	if ((optp->var_type & GET_TYPE_MASK) == GET_DISABLED)
	{
	  if (my_getopt_print_errors)
	    fprintf(stderr,
		    "%s: %s: Option '%s' used, but is disabled\n", my_progname,
		    option_is_loose ? "WARNING" : "ERROR", opt_str);
	  if (option_is_loose)
	  {
	    (*argc)--;
	    continue;
	  }
	  SET_HO_ERROR_AND_CONTINUE(EXIT_OPTION_DISABLED)
	}
        error= 0;
	value= optp->var_type & GET_ASK_ADDR
          ? (*my_getopt_get_addr)(key_name, (uint)strlen(key_name), optp, &error)
          : optp->value;
        if (error)
          SET_HO_ERROR_AND_CONTINUE(error)

	if (optp->arg_type == NO_ARG)
	{
	  /*
	    Due to historical reasons GET_BOOL var_types still accepts arguments
	    despite the NO_ARG arg_type attribute. This can seems a bit unintuitive
	    and care should be taken when refactoring this code.
	  */
	  if (optend && (optp->var_type & GET_TYPE_MASK) != GET_BOOL)
	  {
	    if (my_getopt_print_errors)
              my_getopt_error_reporter(ERROR_LEVEL,
                                       "%s: option '--%s' cannot take an argument",
                                       my_progname, optp->name);
	    SET_HO_ERROR_AND_CONTINUE(EXIT_NO_ARGUMENT_ALLOWED)
	  }
	  if ((optp->var_type & GET_TYPE_MASK) == GET_BOOL)
	  {
	    /*
	      Set bool to 1 if no argument or if the user has used
	      --enable-'option-name'.
	      *optend was set to '0' if one used --disable-option
	    */
	    (*argc)--;
	    if (!optend || *optend == '1' ||
		!cmp_opt_name(optend, "true") ||
		!cmp_opt_name(optend, "on"))
	      *((my_bool*) value)= (my_bool) 1;
	    else if (*optend == '0' ||
		     !cmp_opt_name(optend, "false") ||
                     !cmp_opt_name(optend, "off"))
	      *((my_bool*) value)= (my_bool) 0;
	    else
	    {
	      my_getopt_error_reporter(WARNING_LEVEL,
				       "%s: ignoring option '--%s' "
                                       "due to invalid value '%s'",
				       my_progname, optp->name, optend);
	      continue;
	    }
            if (get_one_option(optp, *((my_bool*) value) ?
                               enabled_my_option : disabled_my_option,
                               filename))
              SET_HO_ERROR_AND_CONTINUE(EXIT_ARGUMENT_INVALID)
            if (optp->deprecation_substitute)
              warn_deprecated(optp);
	    continue;
	  }
	  argument= optend;
	}
	else if (option_is_autoset)
	{
	  if (optend)
	  {
	    my_getopt_error_reporter(ERROR_LEVEL,
                                     "%s: automatically set "
                                     "option '--%s' cannot take an argument",
                                     my_progname, optp->name);

	    SET_HO_ERROR_AND_CONTINUE(EXIT_NO_ARGUMENT_ALLOWED)
	  }
	  if (!(optp->var_type & GET_AUTO))
	  {
	    my_getopt_error_reporter(option_is_loose ?
				     WARNING_LEVEL : ERROR_LEVEL,
                                     "%s: automatic setup request is "
				     "unsupported by option '--%s'",
                                     my_progname, optp->name);
	    if (!option_is_loose)
	      SET_HO_ERROR_AND_CONTINUE(EXIT_ARGUMENT_INVALID)
	    continue;
	  }
	  else
            argument= autoset_my_option;
	}
	else if (optp->arg_type == REQUIRED_ARG && !optend)
	{
          /*
            Check if there are more arguments after this one,
            Note: options loaded from config file that requires value
            should always be in the form '--option=value'.
          */
	  if (!is_cmdline_arg || !*++pos)
	  {
	    if (my_getopt_print_errors)
              my_getopt_error_reporter(ERROR_LEVEL,
                                       "%s: option '--%s' requires an argument",
                                       my_progname, optp->name);
	    SET_HO_ERROR_AND_CONTINUE(EXIT_ARGUMENT_REQUIRED)
	  }
	  argument= *pos;
	  (*argc)--;
	}
	else
	  argument= optend;
      }
      else  /* must be short option */
      {
	for (optend= cur_arg; *optend; optend++)
	{
	  opt_found= 0;
	  for (optp= longopts; optp->name; optp++)
	  {
	    if (optp->id && optp->id == (int) (uchar) *optend)
	    {
	      /* Option recognized. Find next what to do with it */
	      opt_found= 1;
	      if ((optp->var_type & GET_TYPE_MASK) == GET_DISABLED)
	      {
		if (my_getopt_print_errors)
		  fprintf(stderr,
			  "%s: ERROR: Option '-%c' used, but is disabled\n",
			  my_progname, optp->id);
		SET_HO_ERROR_AND_CONTINUE(EXIT_OPTION_DISABLED)
	      }
	      if ((optp->var_type & GET_TYPE_MASK) == GET_BOOL &&
		  optp->arg_type == NO_ARG)
	      {
		*((my_bool*) optp->value)= (my_bool) 1;
                if (get_one_option(optp, argument, filename))
                  SET_HO_ERROR_AND_CONTINUE(EXIT_UNSPECIFIED_ERROR)
                if (optp->deprecation_substitute)
                  warn_deprecated(optp);
		continue;
	      }
	      else if (optp->arg_type == REQUIRED_ARG ||
		       optp->arg_type == OPT_ARG)
	      {
		if (*(optend + 1))
		{
		  /* The rest of the option is option argument */
		  argument= optend + 1;
		  /* This is in effect a jump out of the outer loop */
		  optend= (char*) " ";
		}
		else
		{
                  if (optp->arg_type == OPT_ARG)
                  {
                    if (optp->var_type == GET_BOOL)
                      *((my_bool*) optp->value)= (my_bool) 1;
                    if (get_one_option(optp, argument, filename))
                      SET_HO_ERROR_AND_CONTINUE(EXIT_UNSPECIFIED_ERROR)
                    if (optp->deprecation_substitute)
                      warn_deprecated(optp);
                    continue;
                  }
		  /* Check if there are more arguments after this one */
		  if (!pos[1])
		  {
                    if (my_getopt_print_errors)
                      my_getopt_error_reporter(ERROR_LEVEL,
                                               "%s: option '-%c' requires an argument",
                                               my_progname, optp->id);
                    SET_HO_ERROR_AND_CONTINUE(EXIT_ARGUMENT_REQUIRED)
		  }
		  argument= *++pos;
		  (*argc)--;
		  /* the other loop will break, because *optend + 1 == 0 */
		}
	      }
	      if ((error= setval(optp, optp->value, argument,
                                 set_maximum_value,filename)))
		SET_HO_ERROR_AND_CONTINUE(error)
              if (get_one_option(optp, argument, filename))
                SET_HO_ERROR_AND_CONTINUE(EXIT_UNSPECIFIED_ERROR)
              if (optp->deprecation_substitute)
                warn_deprecated(optp);
	      break;
	    }
	  }
	  if (!opt_found)
	  {
            if (my_getopt_skip_unknown)
            {
              /*
                We are currently parsing a single argv[] argument
                of the form "-XYZ".
                One or the argument found (say Y) is not an option.
                Hack the string "-XYZ" to make a "-YZ" substring in it,
                and push that to the output as an unrecognized parameter.
              */
              DBUG_ASSERT(optend > *pos);
              DBUG_ASSERT(optend >= cur_arg);
              DBUG_ASSERT(optend <= *pos + strlen(*pos));
              DBUG_ASSERT(*optend);
              optend--;
              optend[0]= '-'; /* replace 'X' or '-' by '-' */
              (*argv)[argvpos++]= optend;
              /*
                Do not continue to parse at the current "-XYZ" argument,
                skip to the next argv[] argument instead.
              */
              optend= (char*) " ";
            }
            else
            {
              if (my_getopt_print_errors)
                my_getopt_error_reporter(ERROR_LEVEL,
                                         "%s: unknown option '-%c'",
                                         my_progname, *optend);
              SET_HO_ERROR_AND_CONTINUE(EXIT_UNKNOWN_OPTION)
            }
	  }
	}
        if (opt_found)
          (*argc)--; /* option handled (short), decrease argument count */
	continue;
      }
      if ((!option_is_autoset) &&
        ((error= setval(optp, value, argument, set_maximum_value,filename))) &&
          !option_is_loose)
	SET_HO_ERROR_AND_CONTINUE(error)
      if (get_one_option(optp, argument, filename))
        SET_HO_ERROR_AND_CONTINUE(EXIT_UNSPECIFIED_ERROR)
      if (optp->deprecation_substitute)
        warn_deprecated(optp);

      (*argc)--; /* option handled (long), decrease argument count */
    }
    else /* non-option found */
      (*argv)[argvpos++]= cur_arg;
  }
  if (ho_error)
    DBUG_RETURN(ho_error);
  /*
    Destroy the first, already handled option, so that programs that look
    for arguments in 'argv', without checking 'argc', know when to stop.
    Items in argv, before the destroyed one, are all non-option -arguments
    to the program, yet to be (possibly) handled.
  */
  (*argv)[argvpos]= 0;
  DBUG_RETURN(0);
}


/*
  function: check_struct_option

  Arguments: Current argument under processing from argv and a variable
  where to store the possible key name.

  Return value: In case option is a struct option, returns a pointer to
  the current argument at the position where the struct option (key_name)
  ends, the next character after the dot. In case argument is not a struct
  option, returns a pointer to the argument.

  key_name will hold the name of the key, or 0 if not found.
*/

static char *check_struct_option(char *cur_arg, char *key_name)
{
  char *ptr, *end;
  DBUG_ENTER("check_struct_option");

  ptr= strcend(cur_arg + 1, '.'); /* Skip the first character */
  end= strcend(cur_arg, '=');

  /* 
     If the first dot is after an equal sign, then it is part
     of a variable value and the option is not a struct option.
     Also, if the last character in the string before the ending
     NULL, or the character right before equal sign is the first
     dot found, the option is not a struct option.
  */
  if (end - ptr > 1)
  {
    uint len= (uint) (ptr - cur_arg);
    set_if_smaller(len, FN_REFLEN-1);
    strmake(key_name, cur_arg, len);
    DBUG_RETURN(++ptr);
  }
  else
  {
    key_name[0]= 0;
    DBUG_RETURN(cur_arg);
  }
}

/**
   Parse a boolean command line argument

   "ON", "TRUE" and "1" will return true,
   other values will return false.

   @param[in] argument The value argument
   @return boolean value
*/
static my_bool get_bool_argument(const struct my_option *opts,
                                 const char *argument)
{
  DBUG_ENTER("get_bool_argument");

  if (!cmp_opt_name(argument, "true") ||
      !cmp_opt_name(argument, "on") ||
      !cmp_opt_name(argument, "1"))
    DBUG_RETURN(1);
  else if (!cmp_opt_name(argument, "false") ||
      !cmp_opt_name(argument, "off") ||
      !cmp_opt_name(argument, "0"))
    DBUG_RETURN(0);
  my_getopt_error_reporter(WARNING_LEVEL,
      "option '%s': boolean value '%s' wasn't recognized. Set to OFF.",
      opts->name, argument);
  DBUG_RETURN(0);
}

/*
  function: setval

  Arguments: opts, argument
  Will set the option value to given value
*/

static int setval(const struct my_option *opts, void *value, char *argument,
                  my_bool set_maximum_value, const char *option_file)
{
  int err= 0, res= 0;
  DBUG_ENTER("setval");

  if (!argument)
    argument= enabled_my_option;

  if (value)
  {
    if (set_maximum_value && !(value= opts->u_max_value))
    {
      my_getopt_error_reporter(ERROR_LEVEL,
                               "%s: Maximum value of '%s' cannot be set",
                               my_progname, opts->name);
      DBUG_RETURN(EXIT_NO_PTR_TO_VARIABLE);
    }

    switch ((opts->var_type & GET_TYPE_MASK)) {
    case GET_BOOL: /* If argument differs from 0, enable option, else disable */
      *((my_bool*) value)= get_bool_argument(opts, argument);
      break;
    case GET_INT:
      *((int*) value)= (int) getopt_ll(argument, opts, &err);
      break;
    case GET_UINT:
      *((uint*) value)= (uint) getopt_ull(argument, opts, &err);
      break;
    case GET_LONG:
      *((long*) value)= (long) getopt_ll(argument, opts, &err);
      break;
    case GET_ULONG:
      *((long*) value)= (long) getopt_ull(argument, opts, &err);
      break;
    case GET_LL:
      *((longlong*) value)= getopt_ll(argument, opts, &err);
      break;
    case GET_ULL:
      *((ulonglong*) value)= getopt_ull(argument, opts, &err);
      break;
    case GET_DOUBLE:
      *((double*) value)= getopt_double(argument, opts, &err);
      break;
    case GET_STR:
      /* If no argument or --enable-string-option, set string to "" */
      *((char**) value)= argument == enabled_my_option ? (char*) "" : argument;
      break;
    case GET_STR_ALLOC:
      my_free(*((char**) value));
      if (!(*((char**) value)= my_strdup(key_memory_defaults,
                                         argument == enabled_my_option ? "" :
                                         argument, MYF(MY_WME))))
      {
        res= EXIT_OUT_OF_MEMORY;
        goto ret;
      };
      break;
    case GET_ENUM:
      {
        int type= find_type(argument, opts->typelib, FIND_TYPE_BASIC);
        if (type == 0)
        {
          /*
            Accept an integer representation of the enumerated item.
          */
          char *endptr;
          ulong arg= strtoul(argument, &endptr, 10);
          if (*endptr || arg >= opts->typelib->count)
          {
            res= EXIT_ARGUMENT_INVALID;
            goto ret;
          }
          *(ulong*)value= arg;
        }
        else if (type < 0)
        {
          res= EXIT_AMBIGUOUS_OPTION;
          goto ret;
        }
        else
          *(ulong*)value= type - 1;
      }
      break;
    case GET_SET:
      *((ulonglong*)value)= find_typeset(argument, opts->typelib, &err);
      if (err)
      {
        /* Check if option 'all' is used (to set all bits) */
        if (!cmp_opt_name(argument, "all"))
          *(ulonglong*) value= ((1ULL << opts->typelib->count) - 1);
        else
        {
          /* Accept an integer representation of the set */
          char *endptr;
          ulonglong arg= (ulonglong) strtol(argument, &endptr, 10);
          if (*endptr || (arg >> 1) >= (1ULL << (opts->typelib->count-1)))
          {
            res= EXIT_ARGUMENT_INVALID;
            goto ret;
          };
          *(ulonglong*)value= arg;
        }
        err= 0;
      }
      break;
    case GET_FLAGSET:
      {
        char *error;
        uint error_len;

        *((ulonglong*)value)=
              find_set_from_flags(opts->typelib, opts->typelib->count, 
                                  *(ulonglong *)value, opts->def_value,
                                  argument, (uint)strlen(argument),
                                  &error, &error_len);
        if (error)
        {
          res= EXIT_ARGUMENT_INVALID;
          goto ret;
        };
      }
      break;
    case GET_BIT:
    {
      uint tmp;
      ulonglong bit= (opts->block_size >= 0 ?
                      opts->block_size :
                      -opts->block_size);
      /*
        This sets a bit stored in a longlong.
        The bit to set is stored in block_size. If block_size is positive
        then setting the bit means value is true. If block_size is negative,
        then setting the bit means value is false.
      */
      tmp= get_bool_argument(opts, argument);
      if (opts->block_size < 0)
        tmp= !tmp;
      if (tmp)
        (*(ulonglong*)value)|= bit;
      else
        (*(ulonglong*)value)&= ~bit;
      break;
    }
    case GET_NO_ARG: /* get_one_option has taken care of the value already */
    default:         /* dummy default to avoid compiler warnings */
      break;
    }
    if (err)
    {
      res= err;
      goto ret;
    };
  }

  validate_value(opts->name, argument, option_file);

  DBUG_RETURN(0);

ret:
  my_getopt_error_reporter(ERROR_LEVEL,
                           "%s: Error while setting value '%s' to '%s'",
                           my_progname, argument, opts->name);
  DBUG_RETURN(res);
}


/* 
  Find option

  SYNOPSIS
    findopt()
    optpat	Prefix of option to find (with - or _)
    length	Length of optpat
    opt_res	Options
    ffname	Place for pointer to first found name

  IMPLEMENTATION
    Go through all options in the my_option struct. Return number
    of options found that match the pattern and in the argument
    list the option found, if any. In case of ambiguous option, store
    the name in ffname argument

    RETURN
    0    No matching options
    #   Number of matching options
        ffname points to first matching option
*/

static int findopt(char *optpat, uint length,
		   const struct my_option **opt_res,
		   const char **ffname)
{
  uint count;
  const struct my_option *opt= *opt_res;
  DBUG_ENTER("findopt");

  for (count= 0; opt->name; opt++)
  {
    if (!getopt_compare_strings(opt->name, optpat, length)) /* match found */
    {
      (*opt_res)= opt;
      if (!opt->name[length])		/* Exact match */
	DBUG_RETURN(1);

      if (!my_getopt_prefix_matching)
        continue;

      if (!count)
      {
        /* We only need to know one prev */
	count= 1;
	*ffname= opt->name;
      }
      else if (strcmp(*ffname, opt->name))
      {
	/*
	  The above test is to not count same option twice
	  (see mysql.cc, option "help")
	*/
	count++;
      }
    }
  }

  if (count == 1)
    my_getopt_error_reporter(INFORMATION_LEVEL,
                             "Using unique option prefix '%.*s' is error-prone "
                             "and can break in the future. "
                             "Please use the full name '%s' instead.",
                             length, optpat, *ffname);

  DBUG_RETURN(count);
}


/* 
  function: compare_strings

  Works like strncmp, other than 1.) considers '-' and '_' the same.
  2.) Returns -1 if strings differ, 0 if they are equal
*/

my_bool getopt_compare_strings(register const char *s, register const char *t,
			       uint length)
{
  char const *end= s + length;
  DBUG_ENTER("getopt_compare_strings");

  for (;s != end ; s++, t++)
  {
    if ((*s != '-' ? tolower(*s) : '_') != (*t != '-' ? tolower(*t) : '_'))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}

/*
  function: eval_num_suffix

  Transforms suffix like k/m/g/t/p/e to their real value.
*/

static inline ulonglong eval_num_suffix(char *suffix, int *error)
{
  switch (*suffix) {
  case '\0':
    return 1ULL;
  case 'k':
  case 'K':
    return 1ULL << 10;
  case 'm':
  case 'M':
    return 1ULL << 20;
  case 'g':
  case 'G':
    return 1ULL << 30;
  case 't':
  case 'T':
    return 1ULL << 40;
  case 'p':
  case 'P':
    return 1ULL << 50;
  case 'e':
  case 'E':
    return 1ULL << 60;
  default:
    *error= EXIT_UNKNOWN_SUFFIX;
    return 0ULL;
  }
}

/*
  function: eval_num_suffix_ll

  Transforms a number with a suffix to real number. Suffix can
  be k|K for kilo, m|M for mega, etc.
*/

static longlong eval_num_suffix_ll(char *argument,
                                   int *error, char *option_name)
{
  char *endchar;
  longlong num;
  DBUG_ENTER("eval_num_suffix_ll");

  
  *error= 0;
  errno= 0;
  num= strtoll(argument, &endchar, 10);
  if (errno == ERANGE)
  {
    my_getopt_error_reporter(ERROR_LEVEL,
                             "Integer value out of range for int64:"
                             " '%s' for %s",
                             argument, option_name);
    *error= EXIT_ARGUMENT_INVALID;
    DBUG_RETURN(0);
  }
  num*= eval_num_suffix(endchar, error);
  if (*error)
    my_getopt_error_reporter(ERROR_LEVEL,
                             "Unknown suffix '%c' used for variable '%s' (value '%s'). "
                             "Legal suffix characters are: K, M, G, T, P, E",
                             *endchar, option_name, argument);
  DBUG_RETURN(num);
}

/*
  function: eval_num_suffix_ull

  Transforms a number with a suffix to positive Integer. Suffix can
  be k|K for kilo, m|M for mega, etc.
*/

static ulonglong eval_num_suffix_ull(char *argument,
                                     int *error, char *option_name)
{
  char *endchar;
  ulonglong num;
  DBUG_ENTER("eval_num_suffix_ull");

  if (*argument == '-')
  {
    my_getopt_error_reporter(ERROR_LEVEL,
                             "Incorrect unsigned value: '%s' for %s",
                             argument, option_name);
    *error= EXIT_ARGUMENT_INVALID;
    DBUG_RETURN(0);
  }
  *error= 0;
  errno= 0;
  num= strtoull(argument, &endchar, 10);
  if (errno == ERANGE)
  {
    my_getopt_error_reporter(ERROR_LEVEL,
                             "Integer value out of range for uint64:"
                             " '%s' for %s",
                             argument, option_name);
    *error= EXIT_ARGUMENT_INVALID;
    DBUG_RETURN(0);
  }
  num*= eval_num_suffix(endchar, error);
  if (*error)
    my_getopt_error_reporter(ERROR_LEVEL,
                             "Unknown suffix '%c' used for variable '%s'"
                             " (value '%s')."
                             " Legal suffix characters are: K, M, G, T, P, E",
                             *endchar, option_name, argument);
  DBUG_RETURN(num);
}


/*
  function: getopt_ll

  Evaluates and returns the value that user gave as an argument
  to a variable. Recognizes (case insensitive) K as KILO, M as MEGA
  and G as GIGA bytes. Some values must be in certain blocks, as
  defined in the given my_option struct, this function will check
  that those values are honored.
  In case of an error, set error value in *err.
*/

static longlong getopt_ll(char *arg, const struct my_option *optp, int *err)
{
  longlong num=eval_num_suffix_ll(arg, err, (char*) optp->name);
  if (*err)
    return(0);
  return getopt_ll_limit_value(num, optp, NULL);
}

/*
  function: getopt_ll_limit_value

  Applies min/max/block_size to a numeric value of an option.
  Returns "fixed" value.
*/

longlong getopt_ll_limit_value(longlong num, const struct my_option *optp,
                               my_bool *fix)
{
  longlong old= num;
  my_bool adjusted= FALSE;
  char buf1[255], buf2[255];
  ulonglong block_size= (optp->block_size ? (ulonglong) optp->block_size : 1L);
  DBUG_ENTER("getopt_ll_limit_value");

  if (num > 0 && ((ulonglong) num > (ulonglong) optp->max_value) &&
      optp->max_value) /* if max value is not set -> no upper limit */
  {
    num= (ulonglong) optp->max_value;
    adjusted= TRUE;
  }

  switch ((optp->var_type & GET_TYPE_MASK)) {
  case GET_INT:
    if (num > (longlong) INT_MAX)
    {
      num= ((longlong) INT_MAX);
      adjusted= TRUE;
    }
    break;
  case GET_LONG:
#if SIZEOF_LONG < SIZEOF_LONG_LONG
    if (num > (longlong) LONG_MAX)
    {
      num= ((longlong) LONG_MAX);
      adjusted= TRUE;
    }
#endif
    break;
  default:
    DBUG_ASSERT((optp->var_type & GET_TYPE_MASK) == GET_LL);
    break;
  }

  num= (num / block_size);
  num= (longlong) (num * block_size);

  if (num < optp->min_value)
  {
    num= optp->min_value;
    if (old < optp->min_value)
      adjusted= TRUE;
  }

  if (fix)
    *fix= old != num;
  else if (adjusted)
    my_getopt_error_reporter(WARNING_LEVEL,
                             "option '%s': signed value %s adjusted to %s",
                             optp->name, llstr(old, buf1), llstr(num, buf2));
  DBUG_RETURN(num);
}

/*
  function: getopt_ull

  This is the same as getopt_ll, but is meant for unsigned long long
  values.
*/

static ulonglong getopt_ull(char *arg, const struct my_option *optp, int *err)
{
  ulonglong num= eval_num_suffix_ull(arg, err, (char*) optp->name);
  if (*err)
    return(0);
  return getopt_ull_limit_value(num, optp, NULL);
}


ulonglong getopt_ull_limit_value(ulonglong num, const struct my_option *optp,
                                 my_bool *fix)
{
  my_bool adjusted= FALSE;
  ulonglong old= num;
  char buf1[255], buf2[255];
  DBUG_ENTER("getopt_ull_limit_value");

  if ((ulonglong) num > (ulonglong) optp->max_value &&
      optp->max_value) /* if max value is not set -> no upper limit */
  {
    num= (ulonglong) optp->max_value;
    adjusted= TRUE;
  }

  switch ((optp->var_type & GET_TYPE_MASK)) {
  case GET_UINT:
    if (num > (ulonglong) UINT_MAX)
    {
      num= ((ulonglong) UINT_MAX);
      adjusted= TRUE;
    }
    break;
  case GET_ULONG:
#if SIZEOF_LONG < SIZEOF_LONG_LONG
    if (num > (ulonglong) ULONG_MAX)
    {
      num= ((ulonglong) ULONG_MAX);
      adjusted= TRUE;
    }
#endif
    break;
  default:
    DBUG_ASSERT((optp->var_type & GET_TYPE_MASK) == GET_ULL);
    break;
  }

  if (optp->block_size > 1)
  {
    num/= (ulonglong) optp->block_size;
    num*= (ulonglong) optp->block_size;
  }

  if (num < (ulonglong) optp->min_value)
  {
    num= (ulonglong) optp->min_value;
    if (old < (ulonglong) optp->min_value)
      adjusted= TRUE;
  }

  if (fix)
    *fix= old != num;
  else if (adjusted)
    my_getopt_error_reporter(WARNING_LEVEL,
                             "option '%s': unsigned value %s adjusted to %s",
                             optp->name, ullstr(old, buf1), ullstr(num, buf2));

  DBUG_RETURN(num);
}

double getopt_double_limit_value(double num, const struct my_option *optp,
                                 my_bool *fix)
{
  my_bool adjusted= FALSE;
  double old= num;
  double min, max;
  DBUG_ENTER("getopt_double_limit_value");

  max= getopt_ulonglong2double(optp->max_value);
  min= getopt_ulonglong2double(optp->min_value);
  if (max && num > max)
  {
    num= max;
    adjusted= TRUE;
  }
  if (num < min)
  {
    num= min;
    adjusted= TRUE;
  }
  if (fix)
    *fix= adjusted;
  else if (adjusted)
    my_getopt_error_reporter(WARNING_LEVEL,
                             "option '%s': value %g adjusted to %g",
                             optp->name, old, num);
  DBUG_RETURN(num);
}

/*
  Get double value withing ranges

  Evaluates and returns the value that user gave as an argument to a variable.

  RETURN
    decimal value of arg

    In case of an error, prints an error message and sets *err to
    EXIT_ARGUMENT_INVALID.  Otherwise err is not touched
*/

static double getopt_double(char *arg, const struct my_option *optp, int *err)
{
  double num;
  int error;
  char *end= arg + 1000;                     /* Big enough as *arg is \0 terminated */
  num= my_strtod(arg, &end, &error);
  if (end[0] != 0 || error)
  {
    my_getopt_error_reporter(ERROR_LEVEL,
            "Invalid decimal value for option '%s'\n", optp->name);
    *err= EXIT_ARGUMENT_INVALID;
    return 0.0;
  }
  return getopt_double_limit_value(num, optp, NULL);
}

/*
  Init one value to it's default values

  SYNOPSIS
    my_getopt_init_one_value()
    option              Option to initialize
    value               Pointer to variable
*/

void my_getopt_init_one_value(const struct my_option *option, void *variable,
                              longlong value)
{
  DBUG_ENTER("init_one_value");
  switch ((option->var_type & GET_TYPE_MASK)) {
  case GET_BOOL:
    *((my_bool*) variable)= (my_bool) value;
    break;
  case GET_INT:
    *((int*) variable)= (int) getopt_ll_limit_value((int) value, option, NULL);
    break;
  case GET_ENUM:
    *((ulong*) variable)= (ulong) value;
    break;
  case GET_UINT:
    *((uint*) variable)= (uint) getopt_ull_limit_value((uint) value, option, NULL);
    break;
  case GET_LONG:
    *((long*) variable)= (long) getopt_ll_limit_value((long) value, option, NULL);
    break;
  case GET_ULONG:
    *((ulong*) variable)= (ulong) getopt_ull_limit_value((ulong) value, option, NULL);
    break;
  case GET_LL:
    *((longlong*) variable)= (longlong) getopt_ll_limit_value((longlong) value, option, NULL);
    break;
  case GET_ULL:
    *((ulonglong*) variable)= (ulonglong) getopt_ull_limit_value((ulonglong) value, option, NULL);
    break;
  case GET_SET:
  case GET_FLAGSET:
    *((ulonglong*) variable)= (ulonglong) value;
    break;
  case GET_BIT:
  {
    ulonglong bit= (option->block_size >= 0 ?
                    option->block_size :
                    -option->block_size);
    if (option->block_size < 0)
      value= !value;
    if (value)
      (*(ulonglong*)variable)|= bit;
    else
      (*(ulonglong*)variable)&= ~bit;
    break;
  }
  case GET_DOUBLE:
    *((double*) variable)= getopt_ulonglong2double(value);
    break;
  case GET_STR:
    /*
      Do not clear variable value if it has no default value.
      The default value may already be set.
      NOTE: To avoid compiler warnings, we first cast longlong to intptr,
      so that the value has the same size as a pointer.
    */
    if ((char*) (intptr) value)
      *((char**) variable)= (char*) (intptr) value;
    break;
  case GET_STR_ALLOC:
    /*
      Do not clear variable value if it has no default value.
      The default value may already be set.
      NOTE: To avoid compiler warnings, we first cast longlong to intptr,
      so that the value has the same size as a pointer.
    */
    if ((char*) (intptr) value)
    {
      char **pstr= (char **) variable;
      my_free(*pstr);
      *pstr= my_strdup(key_memory_defaults, (char*) (intptr) value, MYF(MY_WME));
    }
    break;
  default: /* dummy default to avoid compiler warnings */
    break;
  }
  DBUG_VOID_RETURN;
}


/*
  Init one value to it's default values

  SYNOPSIS
    fini_one_value()
    option		Option to initialize
    value		Pointer to variable
*/

static void fini_one_value(const struct my_option *option, void *variable,
			   longlong value __attribute__ ((unused)))
{
  DBUG_ENTER("fini_one_value");
  switch ((option->var_type & GET_TYPE_MASK)) {
  case GET_STR_ALLOC:
    my_free(*((char**) variable));
    *((char**) variable)= NULL;
    break;
  default: /* dummy default to avoid compiler warnings */
    break;
  }
  DBUG_VOID_RETURN;
}


void my_cleanup_options(const struct my_option *options)
{
  DBUG_ENTER("my_cleanup_options");
  init_variables(options, fini_one_value);
  DBUG_VOID_RETURN;
}


/*
  initialize all variables to their default values

  SYNOPSIS
    init_variables()
    options              Array of options
    func_init_one_value  Call this function to init the variable

  NOTES
    We will initialize the value that is pointed to by options->value.
    If the value is of type GET_ASK_ADDR, we will ask for the address
    for a value and initialize.
*/

static void init_variables(const struct my_option *options,
                           init_func_p func_init_one_value)
{
  DBUG_ENTER("init_variables");
  for (; options->name; options++)
  {
    void *value;
    DBUG_PRINT("options", ("name: '%s'", options->name));
    /*
      We must set u_max_value first as for some variables
      options->u_max_value == options->value and in this case we want to
      set the value to default value.
    */
    if (options->u_max_value)
      func_init_one_value(options, options->u_max_value, options->max_value);
    value= options->var_type & GET_ASK_ADDR ?
		  (*my_getopt_get_addr)("", 0, options, 0) : options->value;
    if (value)
      func_init_one_value(options, value, options->def_value);
  }
  DBUG_VOID_RETURN;
}

/** Prints variable or option name, replacing _ with - */
static uint print_name(const struct my_option *optp)
{
  const char *s= optp->name;

  for (;*s;s++)
    putchar(*s == '_' ? '-' : *s);
  return (uint)(s - optp->name);
}

/** prints option comment with indentation and wrapping.

  The comment column starts at startpos, and has width of width
  Current cursor position is curpos, returns new cursor position

  @note can print one character beyond width!
*/
static uint print_comment(const char *comment,
                          int curpos, int startpos, int width)
{
  const char *end= strend(comment);
  int endpos= startpos + width;

  for (; curpos < startpos; curpos++)
    putchar(' ');

  if (*comment == '.' || *comment == ',')
  {
    putchar(*comment);
    comment++;
    curpos++;
  }

  while (end - comment > endpos - curpos)
  {
    const char *line_end;
    for (line_end= comment + endpos - curpos;
         line_end > comment && *line_end != ' ';
         line_end--);
    for (; comment < line_end; comment++)
      putchar(*comment);
    while (*comment == ' ')
      comment++; /* skip the space, as a newline will take it's place now */
    putchar('\n');
    for (curpos= 0; curpos < startpos; curpos++)
      putchar(' ');
  }
  printf("%s", comment);
  return curpos + (int)(end - comment);
}


/*
  function: my_print_options

  Print help for all options and variables.
*/

void my_print_help(const struct my_option *options)
{
  uint col, name_space= 22, comment_space= 57;
  const struct my_option *optp;
  DBUG_ENTER("my_print_help");

  for (optp= options; optp->name; optp++)
  {
    const char *typelib_help= 0;
    if (!optp->comment)
      continue;
    if (optp->id && optp->id < 256)
    {
      printf("  -%c%s", optp->id, strlen(optp->name) ? ", " : "  ");
      col= 6;
    }
    else
    {
      printf("  ");
      col= 2;
    }
    if (strlen(optp->name))
    {
      printf("--");
      col+= 2 + print_name(optp);
      if (optp->arg_type == NO_ARG ||
	  (optp->var_type & GET_TYPE_MASK) == GET_BOOL ||
          (optp->var_type & GET_TYPE_MASK) == GET_BIT)
      {
	putchar(' ');
	col++;
      }
      else if ((optp->var_type & GET_TYPE_MASK) == GET_STR       ||
               (optp->var_type & GET_TYPE_MASK) == GET_STR_ALLOC ||
               (optp->var_type & GET_TYPE_MASK) == GET_ENUM      ||
               (optp->var_type & GET_TYPE_MASK) == GET_SET       ||
               (optp->var_type & GET_TYPE_MASK) == GET_FLAGSET    )
      {
	printf("%s=name%s ", optp->arg_type == OPT_ARG ? "[" : "",
	       optp->arg_type == OPT_ARG ? "]" : "");
	col+= (optp->arg_type == OPT_ARG) ? 8 : 6;
      }
      else
      {
	printf("%s=#%s ", optp->arg_type == OPT_ARG ? "[" : "",
	       optp->arg_type == OPT_ARG ? "]" : "");
	col+= (optp->arg_type == OPT_ARG) ? 5 : 3;
      }
    }
    if (optp->comment && *optp->comment)
    {
      uint count;

      if (col > name_space)
      {
	putchar('\n');
	col= 0;
      }

      col= print_comment(optp->comment, col, name_space, comment_space);
      if (optp->var_type & GET_AUTO)
      {
        col= print_comment(" (Automatically configured unless set explicitly)",
                           col, name_space, comment_space);
      }

      switch (optp->var_type & GET_TYPE_MASK) {
      case GET_ENUM:
        typelib_help= ". One of: ";
        count= optp->typelib->count;
        break;
      case GET_SET: 
        typelib_help= ". Any combination of: ";
        count= optp->typelib->count;
        break;
      case GET_FLAGSET:
        typelib_help= ". Takes a comma-separated list of option=value pairs, "
          "where value is on, off, or default, and options are: ";
        count= optp->typelib->count - 1;
        break;
      }
      if (typelib_help &&
          strstr(optp->comment, optp->typelib->type_names[0]) == NULL)
      {
        uint i;
        col= print_comment(typelib_help, col, name_space, comment_space);
        col= print_comment(optp->typelib->type_names[0], col, name_space, comment_space);
        for (i= 1; i < count; i++)
        {
          col= print_comment(", ", col, name_space, comment_space);
          col= print_comment(optp->typelib->type_names[i], col, name_space, comment_space);
        }
      }
      if ((optp->var_type & GET_TYPE_MASK) == GET_SET)
        col= print_comment(", or ALL to set all combinations", col, name_space, comment_space);
      if (optp->deprecation_substitute != NULL)
      {
        col= print_comment(". Deprecated, will be removed in a future release.",
                           col, name_space, comment_space);
        if (!IS_DEPRECATED_NO_REPLACEMENT(optp->deprecation_substitute))
        {
          char buf1[NAME_CHAR_LEN + 3];
          DBUG_ASSERT(strlen(optp->deprecation_substitute) < NAME_CHAR_LEN);
          strxmov(buf1, "--", optp->deprecation_substitute, NullS);
          convert_underscore_to_dash(buf1, strlen(buf1));
          col= print_comment(" Please use ", col, name_space, comment_space);
          col= print_comment(buf1, col, name_space, comment_space);
          col= print_comment(" instead.", col, name_space, comment_space);
        }
      }
    }
    putchar('\n');
    if ((optp->var_type & GET_TYPE_MASK) == GET_BOOL ||
        (optp->var_type & GET_TYPE_MASK) == GET_BIT)
    {
      if (optp->def_value != 0)
      {
        printf("%*s(Defaults to on; use --skip-", name_space, "");
        print_name(optp);
        printf(" to disable.)\n");
      }
    }
  }
  DBUG_VOID_RETURN;
}


/*
  function: my_print_options

  Print variables.
*/

void my_print_variables(const struct my_option *options)
{
  uint name_space= 34, length, nr;
  ulonglong llvalue;
  char buff[255];
  const struct my_option *optp;
  DBUG_ENTER("my_print_variables");

  for (optp= options; optp->name; optp++)
  {
    length= (uint)strlen(optp->name)+1;
    if (length > name_space)
      name_space= length;
  }

  printf("\nVariables (--variable-name=value)\n");
  printf("%-*s%s", name_space, "and boolean options {FALSE|TRUE}",
         "Value (after reading options)\n");
  for (length=1; length < 75; length++)
    putchar(length == name_space ? ' ' : '-');
  putchar('\n');
  
  for (optp= options; optp->name; optp++)
  {
    void *value= optp->var_type & GET_ASK_ADDR ?
		  (*my_getopt_get_addr)("", 0, optp, 0) : optp->value;
    if (value)
    {
      length= print_name(optp);
      for (; length < name_space; length++)
	putchar(' ');
      switch ((optp->var_type & GET_TYPE_MASK)) {
      case GET_SET:
        if (!(llvalue= *(ulonglong*) value))
	  printf("%s\n", "");
	else
        for (nr= 0; llvalue && nr < optp->typelib->count; nr++, llvalue >>=1)
	{
	  if (llvalue & 1)
            printf( llvalue > 1 ? "%s," : "%s\n", get_type(optp->typelib, nr));
	}
	break;
      case GET_FLAGSET:
        llvalue= *(ulonglong*) value;
        for (nr= 0; llvalue && nr < optp->typelib->count; nr++, llvalue >>=1)
	{
          printf("%s%s=", (nr ? "," : ""), get_type(optp->typelib, nr));
	  printf(llvalue & 1 ? "on" : "off");
	}
        printf("\n");
	break;
      case GET_ENUM:
        printf("%s\n", get_type(optp->typelib, *(ulong*) value));
	break;
      case GET_STR:
      case GET_STR_ALLOC:                    /* fall through */
	printf("%s\n", *((char**) value) ? *((char**) value) :
	       "(No default value)");
	break;
      case GET_BOOL:
	printf("%s\n", *((my_bool*) value) ? "TRUE" : "FALSE");
	break;
      case GET_BIT:
      {
        ulonglong bit= (optp->block_size >= 0 ?
                        optp->block_size :
                        -optp->block_size);
        my_bool reverse= optp->block_size < 0;
	printf("%s\n", ((*((ulonglong*) value) & bit) != 0) ^ reverse ?
               "TRUE" : "FALSE");
	break;
      }
      case GET_INT:
	printf("%d\n", *((int*) value));
	break;
      case GET_UINT:
	printf("%u\n", *((uint*) value));
	break;
      case GET_LONG:
	printf("%ld\n", *((long*) value));
	break;
      case GET_ULONG:
	printf("%lu\n", *((ulong*) value));
	break;
      case GET_LL:
	printf("%s\n", llstr(*((longlong*) value), buff));
	break;
      case GET_ULL:
	longlong10_to_str(*((ulonglong*) value), buff, 10);
	printf("%s\n", buff);
	break;
      case GET_DOUBLE:
	printf("%.10g\n", *(double*) value);
	break;
      case GET_NO_ARG:
	printf("(No default value)\n");
	break;
      default:
	printf("(Disabled)\n");
	break;
      }
    }
  }
  DBUG_VOID_RETURN;
}
