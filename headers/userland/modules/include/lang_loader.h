#ifndef VIBE_USERLAND_LANG_LOADER_H
#define VIBE_USERLAND_LANG_LOADER_H

int lang_can_run(const char *name);
int lang_try_run(int argc, char **argv);
void lang_invalidate_directory_cache(void);
int lang_normalize_command_name(const char *name_or_path, char *normalized, int max_len);

#endif
