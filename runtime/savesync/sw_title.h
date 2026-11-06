/* savewind save sync: the two hooks runtime/services/fs.c calls (GPL-3.0-or-later).
 * Real on the PS5 title (the build links runtime/savesync/build-ps5.sh output); no-ops elsewhere.
 * Without /data/savagewind/savesync.ini they do nothing at runtime either. */
#ifndef SAVEWIND_SW_TITLE_H
#define SAVEWIND_SW_TITLE_H
#ifdef __PROSPERO__
void sw_title_before_mount(void);  /* OpenSaveDataFileSystem: pull a newer save (first mount only) */
void sw_title_after_commit(void);  /* IFileSystem::Commit: push once the writes settle */
#else
static inline void sw_title_before_mount(void) {}
static inline void sw_title_after_commit(void) {}
#endif
#endif
