#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MODE_FILE  0100644
#define MODE_EXEC  0100755
#define MODE_DIR   0040000

// PROVIDED
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// PROVIDED
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// PROVIDED
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// PROVIDED
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// Local struct - avoids needing index.h in test_tree build
typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[512];
} LocalEntry;

// Forward declaration
static int write_tree_level(LocalEntry *entries, int count,
                             const char *prefix, ObjectID *id_out);

static int write_tree_level(LocalEntry *entries, int count,
                             const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *rel = entries[i].path + strlen(prefix);
        char *slash = strchr(rel, '/');

        if (!slash) {
            TreeEntry *e = &tree.entries[tree.count];
            e->mode = entries[i].mode;
            e->hash = entries[i].hash;
            strncpy(e->name, rel, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            tree.count++;
            i++;
        } else {
            char subdir[256];
            size_t dir_len = slash - rel;
            if (dir_len >= sizeof(subdir)) dir_len = sizeof(subdir) - 1;
            strncpy(subdir, rel, dir_len);
            subdir[dir_len] = '\0';

            char full_prefix[512];
            snprintf(full_prefix, sizeof(full_prefix) - 1,
                     "%s%s/", prefix, subdir);

            int j = i;
            while (j < count &&
                   strncmp(entries[j].path, full_prefix,
                           strlen(full_prefix)) == 0)
                j++;

            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i,
                                 full_prefix, &sub_id) != 0)
                return -1;

            TreeEntry *e = &tree.entries[tree.count];
            e->mode = MODE_DIR;
            e->hash = sub_id;
            strncpy(e->name, subdir, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            tree.count++;
            i = j;
        }
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    LocalEntry entries[10000];
    int count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f) && count < 10000) {
            uint32_t mode;
            char hex[65];
            unsigned long mtime;
            uint32_t size;
            char path[512];
            if (sscanf(line, "%o %64s %lu %u %511s",
                       &mode, hex, &mtime, &size, path) == 5) {
                entries[count].mode = mode;
                strncpy(entries[count].path, path,
                        sizeof(entries[count].path) - 1);
                entries[count].path[sizeof(entries[count].path)-1] = '\0';
                hex_to_hash(hex, &entries[count].hash);
                count++;
            }
        }
        fclose(f);
    }

    if (count == 0) {
        Tree empty;
        empty.count = 0;
        void *data;
        size_t len;
        if (tree_serialize(&empty, &data, &len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }

    // Sort by path
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(entries[i].path, entries[j].path) > 0) {
                LocalEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    return write_tree_level(entries, count, "", id_out);
}
