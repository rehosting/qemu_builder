/*
 * Panda modules: external boards and devices loaded as shared objects.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CORE_PANDA_MODULES_H
#define HW_CORE_PANDA_MODULES_H

/*
 * panda_module_load: load the panda module at @path (module_load_path():
 * ABI check, then its type_init()s) and return the names of the QOM types
 * it registered (the caller unrefs the array; the names are QOM's). Only
 * before the machine is created. Loading a path again returns its types.
 */
GPtrArray *panda_module_load(const char *path, Error **errp);

/* The module that registered QOM type @typename, or NULL if built in. */
const char *panda_module_type_origin(const char *typename);

/*
 * panda_module_machine_spec: resolve a -M value that names a module,
 * "PATH[:NAME]" (it contains a '/' or ends in the host DSO suffix): load
 * PATH and return NAME, or the one machine type PATH registered. Returns
 * NULL with @errp unset when @spec is not a path, NULL with @errp set on
 * error.
 */
char *panda_module_machine_spec(const char *spec, Error **errp);

/*
 * panda_modules_check_vmstate: warn, once per type, about each realized
 * device of a module type that has no vmstate: @what (a savevm, a fast
 * restore) will not save or restore it. Returns at once when no module is
 * loaded.
 */
void panda_modules_check_vmstate(const char *what);

#endif
