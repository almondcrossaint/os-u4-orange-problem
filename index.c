// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry *index_find(Index *index, const char *path)
{
    for (int i = 0; i < index->count; i++)
    {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path)
{
    for (int i = 0; i < index->count; i++)
    {
        if (strcmp(index->entries[i].path, path) == 0)
        {
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

int index_status(const Index *index)
{
    printf("Staged changes:\n");
    for (int i = 0; i < index->count; i++)
    {
        printf("  staged:     %s\n", index->entries[i].path);
    }
    if (index->count == 0)
        printf("  (nothing to show)\n\n");
    else
        printf("\n");

    printf("Unstaged changes:\n");
    int found = 0;
    for (int i = 0; i < index->count; i++)
    {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0)
        {
            printf("  deleted:    %s\n", index->entries[i].path);
            found = 1;
        }
        else if (st.st_mtime != index->entries[i].mtime_sec || st.st_size != index->entries[i].size)
        {
            printf("  modified:   %s\n", index->entries[i].path);
            found = 1;
        }
    }
    if (!found)
        printf("  (nothing to show)\n\n");
    else
        printf("\n");

    printf("Untracked files:\n");
    DIR *dir = opendir(".");
    found = 0;
    if (dir)
    {
        struct dirent *ent;
        while ((ent = readdir(dir)))
        {
            if (ent->d_name[0] == '.' || strcmp(ent->d_name, "pes") == 0)
                continue;

            int tracked = 0;
            for (int i = 0; i < index->count; i++)
            {
                if (strcmp(index->entries[i].path, ent->d_name) == 0)
                {
                    tracked = 1;
                    break;
                }
            }

            if (!tracked)
            {
                printf("  untracked:  %s\n", ent->d_name);
                found = 1;
            }
        }
        closedir(dir);
    }
    if (!found)
        printf("  (nothing to show)\n\n");
    else
        printf("\n");

    return 0;
}

// ─── TODO IMPLEMENTATIONS ───────────────────────────────────────────────────

int index_load(Index *index)
{
    FILE *f = fopen(INDEX_FILE, "r");
    index->count = 0;

    if (!f)
        return 0;

    char line[1024];

    while (fgets(line, sizeof(line), f))
    {
        IndexEntry *e = &index->entries[index->count];
        char hash_hex[HASH_HEX_SIZE + 1];

        if (sscanf(line, "%o %64s %ld %u %255[^\n]",
                   &e->mode,
                   hash_hex,
                   &e->mtime_sec,
                   &e->size,
                   e->path) == 5)
        {

            hex_to_hash(hash_hex, &e->hash);
            index->count++;
        }
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index)
{
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f)
        return -1;

    for (int i = 0; i < index->count; i++)
    {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&index->entries[i].hash, hex);

        fprintf(f, "%o %s %ld %u %s\n",
                index->entries[i].mode,
                hex,
                index->entries[i].mtime_sec,
                index->entries[i].size,
                index->entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    rename(tmp_path, INDEX_FILE);
    return 0;
}

int index_add(Index *index, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    void *data = malloc(size ? size : 1);
    if (!data)
    {
        fclose(f);
        return -1;
    }

    if (fread(data, 1, size, f) != (size_t)size)
    {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0)
    {
        free(data);
        return -1;
    }
    free(data);

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    IndexEntry *entry = index_find(index, path);
    if (!entry)
    {
        if (index->count >= MAX_INDEX_ENTRIES)
            return -1;
        entry = &index->entries[index->count++];
    }

    entry->mode = st.st_mode;
    entry->mtime_sec = st.st_mtime;
    entry->size = st.st_size;
    entry->hash = id;
    strcpy(entry->path, path);

    return index_save(index);
}