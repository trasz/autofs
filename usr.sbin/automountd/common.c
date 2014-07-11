/*-
 * Copyright (c) 2014 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/linker.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <paths.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libutil.h>

#include "autofs_ioctl.h"

#include "common.h"

extern FILE *yyin;
extern char *yytext;
extern int yylex(void);

static void	parse_master_yyin(struct node *root, const char *master);
static void	parse_map_yyin(struct node *parent, const char *map,
		    const char *executable_key);

char *
checked_strdup(const char *s)
{
	char *c;

	assert(s != NULL);

	c = strdup(s);
	if (c == NULL)
		log_err(1, "strdup");
	return (c);
}

/*
 * Take two pointers to strings, concatenate the contents with "/" in the
 * middle, make the first pointer point to the result, the second pointer
 * to NULL, and free the old strings.
 *
 * Concatenate pathnames, basically.
 */
static void
concat(char **p1, char **p2)
{
	int ret;
	char *path;

	assert(p1 != NULL);
	assert(p2 != NULL);

	if (*p1 == NULL)
		*p1 = checked_strdup("");

	if (*p2 == NULL)
		*p2 = checked_strdup("");

	ret = asprintf(&path, "%s/%s", *p1, *p2);
	if (ret < 0)
		log_err(1, "asprintf");

	/*
	 * XXX
	 */
	//free(*p1);
	//free(*p2);

	*p1 = path;
	*p2 = NULL;
}

/*
 * Concatenate two strings, inserting separator between them, unless not needed.
 */
char *
separated_concat(const char *s1, const char *s2, char separator)
{
	char *result;
	int ret;

	assert(s1 != NULL);
	assert(s2 != NULL);

	if (s1[0] == '\0' || s2[0] == '\0' ||
	    s1[strlen(s1) - 1] == separator || s2[0] == separator) {
		ret = asprintf(&result, "%s%s", s1, s2);
	} else {
		ret = asprintf(&result, "%s%c%s", s1, separator, s2);
	}
	if (ret < 0)
		log_err(1, "asprintf");

	//log_debugx("separated_concat: got %s and %s, returning %s", s1, s2, result);

	return (result);
}

void
create_directory(const char *path)
{
	char *component, *copy, *tofree, *partial;
	int error;

	assert(path[0] == '/');

	/*
	 * +1 to skip the leading slash.
	 */
	copy = tofree = checked_strdup(path + 1);

	partial = NULL;
	for (;;) {
		component = strsep(&copy, "/");
		if (component == NULL)
			break;
		concat(&partial, &component);
		//log_debugx("checking \"%s\" for existence", partial);
		error = access(partial, F_OK);
		if (error == 0)
			continue;
		if (errno != ENOENT)
			log_err(1, "cannot access %s", partial);
		log_debugx("directory %s does not exist, creating",
		    partial);
		error = mkdir(partial, 0755);
		if (error != 0)
			log_err(1, "cannot create %s", partial);
	}

	free(tofree);
}

struct node *
node_new_root(void)
{
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		log_err(1, "calloc");
	// XXX
	n->n_key = checked_strdup("/");
	n->n_options = checked_strdup("");

	TAILQ_INIT(&n->n_children);

	return (n);
}

struct node *
node_new(struct node *parent, char *key, char *options, char *location,
    const char *config_file, int config_line)
{
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		log_err(1, "calloc");

	TAILQ_INIT(&n->n_children);
	assert(key != NULL);
	n->n_key = key;
	if (options != NULL)
		n->n_options = options;
	else
		n->n_options = strdup("");
	n->n_location = location;
	assert(config_file != NULL);
	n->n_config_file = config_file;
	assert(config_line >= 0);
	n->n_config_line = config_line;

	assert(parent != NULL);
	n->n_parent = parent;
	TAILQ_INSERT_TAIL(&parent->n_children, n, n_next);

	return (n);
}

struct node *
node_new_map(struct node *parent, char *key, char *options, char *map,
    const char *config_file, int config_line)
{
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		log_err(1, "calloc");

	TAILQ_INIT(&n->n_children);
	assert(key != NULL);
	n->n_key = key;
	if (options != NULL)
		n->n_options = options;
	else
		n->n_options = strdup("");
	n->n_map = map;
	assert(config_file != NULL);
	n->n_config_file = config_file;
	assert(config_line >= 0);
	n->n_config_line = config_line;

	assert(parent != NULL);
	n->n_parent = parent;
	TAILQ_INSERT_TAIL(&parent->n_children, n, n_next);

	return (n);
}

static void
node_delete(struct node *n)
{
	struct node *n2, *tmp;

	assert (n != NULL);

	TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp)
		node_delete(n2);

	if (n->n_parent != NULL)
		TAILQ_REMOVE(&n->n_parent->n_children, n, n_next);

	free(n);
}

static bool
node_is_include(const struct node *n)
{
	if (n->n_key == NULL)
		return (false);
	if (n->n_key[0] != '+')
		return (false);
	return (true);
}

/*
 * Move (reparent) node 'n' to make it sibling of 'previous', placed
 * just after it.
 */
static void
node_move_after(struct node *n, struct node *previous)
{

	TAILQ_REMOVE(&n->n_parent->n_children, n, n_next);
	n->n_parent = previous->n_parent;
	TAILQ_INSERT_AFTER(&previous->n_parent->n_children, previous, n, n_next);
}

static void
node_expand_includes(struct node *root, bool is_master)
{
	struct node *n, *n2, *tmp, *tmp2, *tmproot;
	char *include = NULL;
	int error, ret;

	TAILQ_FOREACH_SAFE(n, &root->n_children, n_next, tmp) {
		if (node_is_include(n) == false)
			continue;

		error = access(AUTO_INCLUDE_PATH, F_OK);
		if (error != 0) {
			log_errx(1, "directory services not configured; "
			    "%s does not exist", AUTO_INCLUDE_PATH);
		}

		/*
		 * "+1" to skip leading "+".
		 */
		ret = asprintf(&include, "%s %s",
		    AUTO_INCLUDE_PATH, n->n_key + 1);
		if (ret < 0)
			log_err(1, "asprintf");
		log_debugx("include \"%s\" maps to executable \"%s\"",
		    n->n_key, include);

		yyin = popen(include, "r");
		if (yyin == NULL)
			log_err(1, "unable to execute \"%s\"", include);

		tmproot = node_new_root();
		if (is_master)
			parse_master_yyin(tmproot, include);
		else
			parse_map_yyin(tmproot, include, NULL);

		error = pclose(yyin);
		yyin = NULL;
		if (error != 0)
			log_errx(1, "execution of \"%s\" failed", include);

		/*
		 * Entries to be included are now in tmproot.  We need to merge
		 * them with the rest, preserving their place and ordering.
		 */
		TAILQ_FOREACH_REVERSE_SAFE(n2,
		    &tmproot->n_children, nodehead, n_next, tmp2) {
			node_move_after(n2, n);
		}

		node_delete(n);
		node_delete(tmproot);
	}
}

static char *
expand_ampersand(char *string, const char *key)
{
	char c, *expanded;
	int i, ret, before_len = 0;
	bool backslashed = false;

	expanded = checked_strdup(string);

	for (i = 0; string[i] != '\0'; i++) {
		c = string[i];
		if (c == '\\' && backslashed == false) {
			backslashed = true;
			continue;
		}
		if (backslashed) {
			backslashed = false;
			continue;
		}
		backslashed = false;
		if (c != '&')
			continue;

		/*
		 * The 'before_len' variable contains the number
		 * of characters before the '&'.
		 */
		before_len = i;
		//assert(i + 1 < (int)strlen(string));

		ret = asprintf(&expanded, "%.*s%s%s",
		    before_len, string, key, string + before_len + 1);
		if (ret < 0)
			log_err(1, "asprintf");

		log_debugx("\"%s\" expanded with key \"%s\" to \"%s\"", string, key, expanded);

		/*
		 * Figure out where to start searching for next variable.
		 */
		string = expanded;
		i = before_len + strlen(key);
		backslashed = false;
		//assert(i < (int)strlen(string));
	}

	return (expanded);
}

/*
 * Expand "&" in n_location.  If the key is NULL, try to use
 * key from map entries themselves.  Keep in mind that maps
 * consist of tho levels of node structures, the key is one
 * level up.
 *
 * Variant with NULL key is for "automount -LL".
 */
void
node_expand_ampersand(struct node *n, const char *key)
{
	struct node *child;

	if (n->n_location != NULL) {
		if (key == NULL) {
			if (n->n_parent != NULL && n->n_parent->n_key != NULL &&
			    strcmp(n->n_parent->n_key, "*") != 0) {
				n->n_location = expand_ampersand(n->n_location,
				    n->n_parent->n_key);
			}
		} else {
			n->n_location = expand_ampersand(n->n_location, key);
		}
	}

	TAILQ_FOREACH(child, &n->n_children, n_next)
		node_expand_ampersand(child, key);
}

/*
 * Expand "*" in n_key.
 */
void
node_expand_wildcard(struct node *n, const char *key)
{
	struct node *child;

	assert(key != NULL);

	if (n->n_key != NULL) {
		if (strcmp(n->n_key, "*") == 0)
			n->n_key = checked_strdup(key);
	}

	TAILQ_FOREACH(child, &n->n_children, n_next)
		node_expand_wildcard(child, key);
}

void
node_expand_defined(struct node *n)
{
	struct node *child;

	if (n->n_location != NULL)
		n->n_location = defined_expand(n->n_location);

	TAILQ_FOREACH(child, &n->n_children, n_next)
		node_expand_defined(child);
}

bool
node_is_direct_map(const struct node *n)
{

	for (;;) {
		assert(n->n_parent != NULL);
		if (n->n_parent->n_parent == NULL)
			break;
		n = n->n_parent;
	}

	assert(n->n_key != NULL);
	if (strcmp(n->n_key, "/-") != 0)
		return (false);

	return (true);
}

static void
node_expand_maps(struct node *n, bool indirect)
{
	struct node *n2, *tmp;

	TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp) {
		if (node_is_direct_map(n2)) {
			if (indirect)
				continue;
		} else {
			if (indirect == false)
				continue;
		}

		/*
		 * This is the first-level map node; the one that contains
		 * the key and subnodes with mountpoints and actual map names.
		 */
		if (n2->n_map == NULL)
			continue;

		if (indirect) {
			log_debugx("map \"%s\" is an indirect map, parsing",
			    n2->n_map);
		} else {
			log_debugx("map \"%s\" is a direct map, parsing",
			    n2->n_map);
		}
		parse_map(n2, n2->n_map, NULL);
	}
}

static void
node_expand_direct_maps(struct node *n)
{

	node_expand_maps(n, false);
}

void
node_expand_indirect_maps(struct node *n)
{

	node_expand_maps(n, true);
}

static char *
node_path_x(const struct node *n, char *x)
{
	char *path;
	size_t len;

	if (n->n_parent == NULL)
		return (x);

	/*
	 * Return "/-" for direct maps only if we were asked for path
	 * to the "/-" node itself, not to any of its subnodes.
	 */
	if (n->n_parent->n_parent == NULL &&
	    strcmp(n->n_key, "/-") == 0 &&
	    x[0] != '\0') {
		return (x);
	}

	path = separated_concat(n->n_key, x, '/');
	free(x);

	/*
	 * Strip trailing slash.
	 */
	len = strlen(path);
	assert(len > 0);
	if (path[len - 1] == '/')
		path[len - 1] = '\0';

	return (node_path_x(n->n_parent, path));
}

/*
 * Return full path for node, consisting of concatenated
 * paths of node itself and all its parents, up to the root.
 */
char *
node_path(const struct node *n)
{

	return (node_path_x(n, checked_strdup("")));
}

static char *
node_options_x(const struct node *n, char *x)
{
	char *options;

	options = separated_concat(x, n->n_options, ',');
	if (n->n_parent == NULL)
		return (options);

	return (node_options_x(n->n_parent, options));
}

/*
 * Return options for node, consisting of concatenated
 * options from the node itself and all its parents,
 * up to the root.
 */
char *
node_options(const struct node *n)
{

	return (node_options_x(n, checked_strdup("")));
}

static void
node_print_indent(const struct node *n, int indent)
{
	const struct node *n2, *first_child;
	char *path, *options;

	path = node_path(n);
	options = node_options(n);
	first_child = TAILQ_FIRST(&n->n_children);

	/*
	 * Do not show both parent and child node if they have the same
	 * mountpoint; only show the child node.  This means the typical,
	 * "key location", map entries are shown in a single line;
	 * the "key mountpoint1 location2 mountpoint2 location2" entries
	 * take multiple lines.
	 */
	if (first_child == NULL || TAILQ_NEXT(first_child, n_next) != NULL ||
	    strcmp(path, node_path(first_child)) != 0) {
		assert(n->n_location == NULL || n->n_map == NULL);
		printf("%*.s%-*s %s%-*s %-*s # %s map %s at %s:%d\n",
		    indent, "",
		    25 - indent,
		    path,
		    options[0] != '\0' ? "-" : " ",
		    20,
		    options[0] != '\0' ? options : "",
		    20,
		    n->n_location != NULL ? n->n_location : n->n_map != NULL ? n->n_map : "",
		    node_is_direct_map(n) ? "direct" : "indirect",
		    indent == 0 ? "referenced" : "defined",
		    n->n_config_file, n->n_config_line);
	}

	TAILQ_FOREACH(n2, &n->n_children, n_next)
		node_print_indent(n2, indent + 2);

	free(path);
	free(options);
}

void
node_print(const struct node *n)
{
	const struct node *n2;

	TAILQ_FOREACH(n2, &n->n_children, n_next)
		node_print_indent(n2, 0);
}

struct node *
node_find(struct node *node, const char *path)
{
	struct node *child, *found;
	char *tmp;

	//log_debugx("looking up %s in %s", path, node->n_key);

	tmp = node_path(node);
	if (strncmp(tmp, path, strlen(tmp)) != 0) {
		free(tmp);
		return (NULL);
	}
	free(tmp);

	TAILQ_FOREACH(child, &node->n_children, n_next) {
		found = node_find(child, path);
		if (found != NULL)
			return (found);
	}

	return (node);
}

/*
 * Canonical form of a map entry looks like this:
 *
 * key [-options] [ [/mountpoint] [-options2] location ... ]
 *
 * Entries for executable maps are slightly different, as they
 * lack the 'key' field and are always single-line; the key field
 * for those maps is taken from 'executable_key' argument.
 *
 * We parse it in such a way that a map always has two levels - first
 * for key, and the second, for the mountpoint.
 */
static void
parse_map_yyin(struct node *parent, const char *map, const char *executable_key)
{
	char *key = NULL, *options = NULL, *mountpoint = NULL,
	    *options2 = NULL, *location = NULL;
	int ret;
	struct node *node;

	lineno = 1;

	if (executable_key != NULL)
		key = checked_strdup(executable_key);

	for (;;) {
		ret = yylex();
		if (ret == 0 || ret == NEWLINE) {
			if (key != NULL || options != NULL) {
				log_errx(1, "truncated entry in %s, line %d",
				    map, lineno);
			}
			if (ret == 0 || executable_key != NULL) {
				/*
				 * End of file.
				 */
				break;
			} else {
				key = options = NULL;
				continue;
			}
		}
		if (key == NULL) {
			key = checked_strdup(yytext);
			if (key[0] == '+') {
				node_new(parent, key, NULL, NULL, map, lineno);
				key = options = NULL;
				continue;
			}
			continue;
		} else if (yytext[0] == '-') {
			if (options != NULL) {
				log_errx(1, "duplicated options in %s, line %d",
				    map, lineno);
			}
			/*
			 * +1 to skip leading "-".
			 */
			options = checked_strdup(yytext + 1);
			continue;
		}

		//log_debugx("adding map node, %s", key);
		node = node_new(parent, key, options, NULL, map, lineno);
		key = options = NULL;

		for (;;) {
			if (yytext[0] == '/') {
				if (mountpoint != NULL) {
					log_errx(1, "duplicated mountpoint "
					    "in %s, line %d", map, lineno);
				}
				if (options2 != NULL || location != NULL) {
					log_errx(1, "mountpoint out of order "
					    "in %s, line %d", map, lineno);
				}
				mountpoint = checked_strdup(yytext);
				goto again;
			}

			if (yytext[0] == '-') {
				if (options2 != NULL) {
					log_errx(1, "duplicated options "
					    "in %s, line %d", map, lineno);
				}
				if (location != NULL) {
					log_errx(1, "options out of order "
					    "in %s, line %d", map, lineno);
				}
				options2 = checked_strdup(yytext + 1);
				goto again;
			}

			if (location != NULL) {
				log_errx(1, "too many arguments "
				    "in %s, line %d", map, lineno);
			}
			location = checked_strdup(yytext);

			if (mountpoint == NULL)
				mountpoint = checked_strdup("/");
			if (options2 == NULL)
				options2 = checked_strdup("");

#if 0
			log_debugx("adding map node, %s %s %s",
			    mountpoint, options2, location);
#endif
			node_new(node, mountpoint, options2, location,
			    map, lineno);
			mountpoint = options2 = location = NULL;
again:
			ret = yylex();
			if (ret == 0 || ret == NEWLINE) {
				if (mountpoint != NULL || options2 != NULL ||
				    location != NULL) {
					log_errx(1, "truncated entry "
					    "in %s, line %d", map, lineno);
				}
				break;
			}
		}
	}
}

static bool
file_is_executable(const char *path)
{
	struct stat sb;
	int error;

	error = stat(path, &sb);
	if (error != 0)
		log_err(1, "cannot stat %s", path);
	if ((sb.st_mode & S_IXUSR) || (sb.st_mode & S_IXGRP) ||
	    (sb.st_mode & S_IXOTH))
		return (true);
	return (false);
}

/*
 * Parse a special map, eg. "-hosts".
 */
static void
parse_special_map(struct node *parent, const char *map, const char *key)
{
	char *path = NULL;
	int error, ret;

	assert(map[0] == '-');

	if (key == NULL) {
		log_debugx("skipping map %s due to forced -nobrowse", map);
		return;
	}

	/*
	 * +1 to skip leading "-" in map name.
	 */
	ret = asprintf(&path, "%s/special_%s %s", AUTO_SPECIAL_PREFIX,
	    map + 1, key);
	if (ret < 0)
		log_err(1, "asprintf");

	log_debugx("special map \"%s\", key \"%s\"; will execute \"%s\"",
	    map, key, path);
	yyin = popen(path, "r");
	if (yyin == NULL) {
		log_err(1, "failed to handle special map \"%s\"; "
		    "execution of \"%s\" failed", map, path);
	}

	parse_map_yyin(parent, map, key);

	error = pclose(yyin);
	if (error != 0) {
		log_errx(1, "failed to handle special map \"%s\"; "
		    "execution of \"%s\" failed", map, path);
	}

	node_expand_includes(parent, false);
	node_expand_direct_maps(parent);

	free(path);
}

/*
 * Retrieve and parse map from directory services, eg. LDAP.
 * Note that it's different from executable maps, in that
 * the include script outputs the whole map to standard output
 * (as opposed to executable maps that only output a single
 * entry, without the key), and it takes the map name as an
 * argument, instead of key.
 */
static void
parse_included_map(struct node *parent, const char *map)
{
	char *path = NULL;
	int error, ret;

	assert(map[0] != '-');
	assert(map[0] != '/');

	error = access(AUTO_INCLUDE_PATH, F_OK);
	if (error != 0) {
		log_errx(1, "directory services not configured;"
		    " %s does not exist", AUTO_INCLUDE_PATH);
	}

	ret = asprintf(&path, "%s %s", AUTO_INCLUDE_PATH, map);
	if (ret < 0)
		log_err(1, "asprintf");

	log_debugx("remote map \"%s\"; will execute \"%s\"",
	    map, path);
	yyin = popen(path, "r");
	if (yyin == NULL) {
		log_err(1, "failed to handle remote map \"%s\"; "
		    "execution of \"%s\" failed", map, path);
	}

	parse_map_yyin(parent, map, NULL);

	error = pclose(yyin);
	if (error != 0) {
		log_errx(1, "failed to handle remote map \"%s\"; "
		    "execution of \"%s\" failed", map, path);
	}

	node_expand_includes(parent, false);
	node_expand_direct_maps(parent);

	free(path);
}

void
parse_map(struct node *parent, const char *map, const char *key)
{
	char *path = NULL;
	int error, ret;
	bool executable;

	assert(map != NULL);
	assert(map[0] != '\0');

	log_debugx("parsing map \"%s\"", map);

	if (map[0] == '-')
		return (parse_special_map(parent, map, key));

	if (map[0] == '/') {
		path = checked_strdup(map);
	} else {
		ret = asprintf(&path, "%s/%s", AUTO_MAP_PREFIX, map);
		if (ret < 0)
			log_err(1, "asprintf");
		log_debugx("map \"%s\" maps to \"%s\"", map, path);

		/*
		 * See if the file exists.  If not, try to obtain the map
		 * from directory services.
		 */
		error = access(path, F_OK);
		if (error != 0) {
			log_debugx("map file \"%s\" does not exist; falling "
			    "back to directory services", path);
			return (parse_included_map(parent, map));
		}
	}

	executable = file_is_executable(path);

	if (executable) {
		log_debugx("map \"%s\" is executable", map);

		if (key != NULL) {
			ret = asprintf(&path, "%s %s", path, key);
			if (ret < 0)
				log_err(1, "asprintf");
			log_debugx("will execute \"%s\"", path);

		}
		yyin = popen(path, "r");
		if (yyin == NULL)
			log_err(1, "unable to execute \"%s\"", path);
	} else {
		yyin = fopen(path, "r");
		if (yyin == NULL)
			log_err(1, "unable to open \"%s\"", path);
	}

	free(path);
	path = NULL;

	parse_map_yyin(parent, map, executable ? key : NULL);

	if (executable) {
		error = pclose(yyin);
		if (error != 0) {
			log_errx(1, "execution of dynamic map \"%s\" failed",
			    map);
		}
	} else {
		fclose(yyin);
	}
	yyin = NULL;

	log_debugx("done parsing map \"%s\"", map);

	node_expand_includes(parent, false);
	node_expand_direct_maps(parent);
}

static void
parse_master_yyin(struct node *root, const char *master)
{
	char *mountpoint = NULL, *map = NULL, *options = NULL;
	int ret;

	/*
	 * XXX: 1 gives incorrect values; wtf?
	 */
	lineno = 0;

	for (;;) {
		ret = yylex();
		if (ret == 0 || ret == NEWLINE) {
			if (mountpoint != NULL) {
				//log_debugx("adding map for %s", mountpoint);
				node_new_map(root, mountpoint, options, map,
				    master, lineno);
			}
			if (ret == 0) {
				break;
			} else {
				mountpoint = map = options = NULL;
				continue;
			}
		}
		if (mountpoint == NULL) {
			mountpoint = checked_strdup(yytext);
		} else if (map == NULL) {
			map = checked_strdup(yytext);
		} else if (options == NULL) {
			/*
			 * +1 to skip leading "-".
			 */
			options = checked_strdup(yytext + 1);
		} else {
			log_errx(1, "too many arguments in %s, line %d",
			    master, lineno);
		}
	}
}

void
parse_master(struct node *root, const char *master)
{

	log_debugx("parsing auto_master file at \"%s\"", master);

	yyin = fopen(master, "r");
	if (yyin == NULL)
		err(1, "unable to open %s", master);

	parse_master_yyin(root, master);

	fclose(yyin);
	yyin = NULL;

	log_debugx("done parsing \"%s\"", master);

	node_expand_includes(root, true);
	node_expand_direct_maps(root);
}

/*
 * Two things daemon(3) does, that we actually also want to do
 * when running in foreground, is closing the stdin and chdiring
 * to "/".  This is what we do here.
 */
void
lesser_daemon(void)
{
	int error, fd;

	error = chdir("/");
	if (error != 0)
		log_warn("chdir");

	fd = open(_PATH_DEVNULL, O_RDWR, 0);
	if (fd < 0) {
		log_warn("cannot open %s", _PATH_DEVNULL);
		return;
	}

	error = dup2(fd, STDIN_FILENO);
	if (error != 0)
		log_warn("dup2");

	error = close(fd);
	if (error != 0) {
		/* Bloody hell. */
		log_warn("close");
	}
}

int
main(int argc, char **argv)
{
	char *cmdname;

	if (argv[0] == NULL)
		log_errx(1, "NULL command name");

	cmdname = basename(argv[0]);

	if (strcmp(cmdname, "automount") == 0)
		return (main_automount(argc, argv));
	else if (strcmp(cmdname, "automountd") == 0)
		return (main_automountd(argc, argv));
	else if (strcmp(cmdname, "autounmountd") == 0)
		return (main_autounmountd(argc, argv));
	else
		log_errx(1, "binary name should be either \"automount\", "
		    "\"automountd\", or \"autounmountd\"");
}
