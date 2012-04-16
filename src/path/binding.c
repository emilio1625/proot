/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2010, 2011, 2012 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#include <sys/stat.h> /* lstat(2), */
#include <unistd.h>   /* lstat(2), */
#include <stdlib.h>   /* realpath(3), calloc(3), free(3), */
#include <string.h>   /* string(3),  */
#include <assert.h>   /* assert(3), */
#include <limits.h>   /* PATH_MAX, */
#include <errno.h>    /* errno, E* */

#include "path/binding.h"
#include "path/path.h"
#include "path/canon.h"
#include "notice.h"
#include "config.h"

struct binding_path {
	char path[PATH_MAX];
	size_t length;
};

struct binding {
	struct binding_path host;
	struct binding_path guest;

	int sanitized;
	int need_substitution;

	struct binding *next;
};

static struct binding *bindings = NULL;

/**
 * Insert @binding into the list of @bindings, in a reversely sorted
 * manner.
 */
static void insort_binding(struct binding *binding)
{
	struct binding *previous = NULL;
	struct binding *next = NULL;

	for (next = bindings; next != NULL; previous = next, next = next->next) {
		if (strcmp(binding->host.path, next->host.path) > 0)
			break;
	}

	if (previous)
		previous->next = binding;
	else
		bindings = binding;

	binding->next = next;
}

/**
 * Save @path in the list of paths that are bound for the
 * translation mechanism.
 */
void bind_path(const char *host_path, const char *guest_path, bool must_exist)
{
	struct binding *binding;
	const char *tmp;

	binding = calloc(1, sizeof(struct binding));
	if (binding == NULL) {
		notice(WARNING, SYSTEM, "calloc()");
		return;
	}

	if (realpath(host_path, binding->host.path) == NULL) {
		if (must_exist)
			notice(WARNING, SYSTEM, "realpath(\"%s\")", host_path);
		goto error;
	}

	binding->host.length = strlen(binding->host.path);

	/* Special case when the host rootfs is bound. */
	if (binding->host.length == 1)
		binding->host.length = 0;

	tmp = guest_path ? guest_path : host_path;
	if (strlen(tmp) >= PATH_MAX) {
		notice(ERROR, INTERNAL, "binding location \"%s\" is too long", tmp);
		goto error;
	}

	strcpy(binding->guest.path, tmp);

	/* The sanitization of binding->guest is delayed until
	 * init_module_path(). */
	binding->guest.length = 0;
	binding->sanitized = 0;

	insort_binding(binding);

	return;

error:
	free(binding);
	binding = NULL;
	return;
}

/**
 * Print all bindings (verbose purpose).
 */
void print_bindings()
{
	struct binding *binding;

	for (binding = bindings; binding != NULL; binding = binding->next) {
		if (compare_paths2(binding->host.path, binding->host.length,
				   binding->guest.path, binding->guest.length)
			== PATHS_ARE_EQUAL)
			notice(INFO, USER, "binding = %s", binding->host.path);
		else
			notice(INFO, USER, "binding = %s:%s",
				binding->host.path, binding->guest.path);
	}
}

/**
 * Substitute the guest path (if any) with the host path in @path.
 * This function returns:
 *
 *     * -1 if it is not a binding location
 *
 *     * 0 if it is a binding location but no substitution is needed
 *       ("symetric" binding)
 *
 *     * 1 if it is a binding location and a substitution was performed
 *       ("asymmetric" binding)
 */
int substitute_binding(int which, char path[PATH_MAX])
{
	struct binding *binding;
	size_t path_length = strlen(path);

	for (binding = bindings; binding != NULL; binding = binding->next) {
		struct binding_path *ref;
		struct binding_path *anti_ref;
		enum path_comparison comparison;

		if (!binding->sanitized)
			continue;

		switch (which) {
		case BINDING_GUEST_REF:
			ref      = &binding->guest;
			anti_ref = &binding->host;
			break;

		case BINDING_HOST_REF:
			ref      = &binding->host;
			anti_ref = &binding->guest;
			break;

		default:
			notice(WARNING, INTERNAL, "unexpected value for binding reference");
			return -1;
		}

		comparison = compare_paths2(ref->path, ref->length, path, path_length);
		if (   comparison != PATHS_ARE_EQUAL
		    && comparison != PATH1_IS_PREFIX)
			continue;

		if (which == BINDING_HOST_REF) {
			/* Don't substitute systematically the prefix of the
			 * rootfs when used as an asymmetric binding, as with:
			 *
			 *     proot -m /usr:/location /usr/local/slackware
			 */
			if (root_length != 1 /* rootfs != "/" */
			    && belongs_to_guestfs(path))
				continue;

			/* Avoid an extra trailing '/' when in the
			 * asymmetric binding of the host rootfs. */
			if (ref->length == 0 &&	path_length == 1)
				path_length = 0;
		}

		/* Is it a "symetric" binding?  */
		if (binding->need_substitution == 0)
			return 0;

		/* Ensure the substitution will not create a buffer
		 * overflow. */
		if (path_length - ref->length + anti_ref->length >= PATH_MAX)  {
			notice(WARNING, INTERNAL, "Can't handle binding %s: pathname too long");
			return -1;
		}

		/* Replace the guest path with the host path. */
		memmove(path + anti_ref->length, path + ref->length, path_length - ref->length);
		memcpy(path, anti_ref->path, anti_ref->length);
		path[path_length - ref->length + anti_ref->length] = '\0';

		/* Special case when the host rootfs is bound at
		 * the guest path pointed to by path[]. */
		if (path[0] == '\0') {
			path[0] = '/';
			path[1] = '\0';
		}

		return 1;
	}

	return -1;
}

/**
 * Create a "dummy" path up to the canonicalized guest path @c_path,
 * it cheats programs that walk up to it.
 */
static void create_dummy(char c_path[PATH_MAX], const char *real_path)
{
	char t_current_path[PATH_MAX];
	char t_path[PATH_MAX];
	struct stat statl;
	int status;
	int is_final;
	const char *cursor;
	int type;

	/* Determine the type of the element to be bound:
	   1: file
	   0: directory
	*/
	status = stat(real_path, &statl);
	if (status != 0)
		goto error;

	type = (S_ISREG(statl.st_mode));

	status = join_paths(2, t_path, root, c_path);
	if (status < 0)
		goto error;

	status = lstat(t_path, &statl);
	if (status == 0)
		return;

	if (errno != ENOENT)
		goto error;

	/* Skip the "root" part since we know it exists. */
	strcpy(t_current_path, root);
	cursor = t_path + root_length;

	is_final = 0;
	while (!is_final) {
		char component[NAME_MAX];
		char tmp[PATH_MAX];

		status = next_component(component, &cursor);
		if (status < 0)
			goto error;
		is_final = status;

		strcpy(tmp, t_current_path);
		status = join_paths(2, t_current_path, tmp, component);
		if (status < 0)
			goto error;

		/* Note that the final component can't always be a
		 * directory, actually its type matters since not only
		 * the entry in the parent directory is important for
		 * some tools like 'find'.  */
		if (is_final) {
			if (type) {
				status = open(t_current_path, O_CREAT, 0766);
				if (status < 0)
					goto error;
				close(status);
			}
			else {
				status = mkdir(t_current_path, 0777);
				if (status < 0 && errno != EEXIST)
					goto error;
			}
		}
		else {
			status = mkdir(t_current_path, 0777);
			if (status < 0 && errno != EEXIST)
				goto error;
		}
	}

	notice(INFO, USER, "create the binding location \"%s\"", c_path);
	return;

error:
	notice(WARNING, USER, "can't create parent directories for \"%s\"", c_path);
}

/**
 * Initialize internal data of the path translator.
 */
void init_bindings()
{
	struct binding *binding;

	/* Now the module is initialized so we can call
	 * canonicalize() safely. */
	for (binding = bindings; binding != NULL; binding = binding->next) {
		char tmp[PATH_MAX];
		int status;

		assert(!binding->sanitized);

		strcpy(tmp, binding->guest.path);

		/* In case binding->guest.path is relative.  */
		if (!getcwd(binding->guest.path, PATH_MAX)) {
			notice(WARNING, SYSTEM, "can't sanitize binding \"%s\"");
			goto error;
		}

		/* Sanitize the guest path of the binding within the
		   alternate rootfs since it is assumed by
		   substitute_binding().  Note the host path is already
		   sanitized in bind_path().  */
		status = canonicalize(0, tmp, 1, binding->guest.path, 0);
		if (status < 0) {
			notice(WARNING, INTERNAL, "sanitizing the binding location \"%s\": %s",
			       tmp, strerror(-status));
			goto error;
		}

		if (strcmp(binding->guest.path, "/") == 0) {
			notice(WARNING, USER, "can't create a binding in \"/\"");
			goto error;
		}

		binding->guest.length = strlen(binding->guest.path);
		binding->need_substitution =
			compare_paths(binding->host.path,
				binding->guest.path) != PATHS_ARE_EQUAL;

		/* Remove the trailing slash as expected by substitute_binding(). */
		if (binding->guest.path[binding->guest.length - 1] == '/') {
			binding->guest.path[binding->guest.length - 1] = '\0';
			binding->guest.length--;
		}

		create_dummy(binding->guest.path, binding->host.path);

		binding->sanitized = 1;
		continue;

	error:
		/* TODO: remove this element from the list instead. */
		binding->sanitized = 0;
	}
}
