#ifndef P8_PATCHER_H
#define P8_PATCHER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * apply_p8t_patch
 * 
 * Takes the original Lua script string and a path to a .p8t patch file.
 * Returns a newly allocated string with the patch applied, or NULL if the patch 
 * file doesn't exist or is invalid.
 * The caller is responsible for freeing the original script if a new one is returned.
 */
char *apply_p8t_patch(const char *old_script, const char *p8t_path);

#ifdef __cplusplus
}
#endif

#endif // P8_PATCHER_H
