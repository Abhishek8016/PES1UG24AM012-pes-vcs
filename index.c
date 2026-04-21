#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declarations from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
void hash_to_hex(const ObjectID *id, char *hex_out);
int hex_to_hash(const char *hex, ObjectID *id_out);

// PROVIDED
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    // Use static to avoid stack overflow (Index struct is ~6MB)
    static Index sorted;
    sorted = *index;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry),
          compare_index_entries);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/index_tmp_XXXXXX", PES_DIR);
    int fd = mkstemp(tmp_path);
    if (fd < 0) { perror("mkstemp"); return -1; }

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_path); return -1; }

    for (int i = 0; i < sorted.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted.entries[i].hash, hex);
        fprintf(f, "%o %s %lu %u %s\n",
                sorted.entries[i].mode,
                hex,
                (unsigned long)sorted.entries[i].mtime_sec,
                (unsigned int)sorted.entries[i].size,
                sorted.entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

// PROVIDED
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// PROVIDED
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f) &&
           index->count < MAX_INDEX_ENTRIES) {
        uint32_t mode;
        char hex[65];
        unsigned long mtime;
        uint32_t size;
        char path[512];
        if (sscanf(line, "%o %64s %lu %u %511s",
                   &mode, hex, &mtime, &size, path) == 5) {
            IndexEntry *e = &index->entries[index->count];
            e->mode      = mode;
            e->mtime_sec = mtime;
            e->size      = size;
            strncpy(e->path, path, sizeof(e->path) - 1);
            e->path[sizeof(e->path) - 1] = '\0';
            hex_to_hash(hex, &e->hash);
            index->count++;
        }
    }
    fclose(f);
    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s'\n", path);
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    size_t file_size = (size_t)st.st_size;
    unsigned char *data = NULL;
    if (file_size > 0) {
        data = malloc(file_size);
        if (!data) { fclose(f); return -1; }
        size_t r = fread(data, 1, file_size, f);
        (void)r;
    }
    fclose(f);

    ObjectID id;
    unsigned char empty_buf[1] = {0};
    if (object_write(OBJ_BLOB,
                     file_size > 0 ? (void*)data : (void*)empty_buf,
                     file_size, &id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash      = id;
        existing->mtime_sec = st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
        existing->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        IndexEntry *e = &index->entries[index->count++];
        e->hash      = id;
        e->mtime_sec = st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        e->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    return index_save(index);
}
