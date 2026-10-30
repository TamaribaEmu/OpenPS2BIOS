/* SPDX-License-Identifier: MIT */
#include "openbios/iso9660.h"
#ifdef OB_ISO_TRACE
#include "openbios/platform.h"
#define TRACE(label, value) do { ob_puts("[openbios:iso] " label "="); ob_puthex(value); ob_puts("\n"); } while (0)
#else
#define TRACE(label, value) ((void)0)
#endif

struct iso_record {
    uint32_t lba;
    uint32_t size;
    uint8_t flags;
    uint8_t identifier_length;
    const uint8_t *identifier;
    const uint8_t *raw; /* whole directory record */
};

static int ascii_equal(uint8_t a, uint8_t b) {
    if (a >= 'a' && a <= 'z') a = (uint8_t)(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 'a' + 'A');
    return a == b;
}

static int whitespace(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p) {
    return ob_word32(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3]);
}

static int both16(const uint8_t *p, uint32_t *value) {
    uint32_t little = ob_le16(p);
    uint32_t big = be16(p + 2);
    if (little != big) return OB_EFORMAT;
    *value = little;
    return OB_OK;
}

static int both32(const uint8_t *p, uint32_t *value) {
    uint32_t little = ob_le32(p);
    uint32_t big = be32(p + 4);
    if (little != big) { TRACE("endian little", little); TRACE("endian big", big); return OB_EFORMAT; }
    *value = little;
    return OB_OK;
}

static int sector_read(const struct ob_iso *iso, uint32_t lba,
                       uint8_t sector[OB_ISO_SECTOR_SIZE]) {
    if (!iso || !iso->read_sector || lba >= iso->sector_count) return OB_EBOUNDS;
    if (iso->read_sector(iso->ctx, lba, sector) != 0) return OB_EBOUNDS;
    return OB_OK;
}

static int record_parse(const struct ob_iso *iso, const uint8_t *bytes,
                        uint32_t available, uint32_t sector_remaining,
                        struct iso_record *out) {
    uint32_t extent, size, volume_sequence, data_lba, blocks;
    uint8_t length, identifier_length, ext_attr;
    uint32_t i;
    if (!bytes || !out || available == 0) return OB_EFORMAT;
    length = bytes[0];
    if (length < 34 || length > available || length > sector_remaining)
        return OB_EFORMAT;
    identifier_length = bytes[32];
    if (identifier_length == 0 || (uint32_t)33 + identifier_length > length)
        return OB_EFORMAT;
    if (identifier_length % 2 == 0 && (uint32_t)34 + identifier_length > length)
        return OB_EFORMAT;
    if (!(identifier_length == 1 && (bytes[33] == 0 || bytes[33] == 1)))
        for (i = 0; i < identifier_length; ++i)
            if (bytes[33 + i] >= 0x80) return OB_EFORMAT;
    if (bytes[26] != 0 || bytes[27] != 0) return OB_EFORMAT;
    if (both32(bytes + 2, &extent) != OB_OK ||
        both32(bytes + 10, &size) != OB_OK ||
        both16(bytes + 28, &volume_sequence) != OB_OK)
        return OB_EFORMAT;
    if (volume_sequence != 1 || (bytes[25] & 0x80)) return OB_EFORMAT;
    ext_attr = bytes[1];
    if (extent > UINT32_MAX - ext_attr) return OB_EBOUNDS;
    data_lba = extent + ext_attr;
    blocks = size / OB_ISO_SECTOR_SIZE;
    if (size % OB_ISO_SECTOR_SIZE) ++blocks;
    if (data_lba > iso->sector_count || blocks > iso->sector_count - data_lba)
        return OB_EBOUNDS;
    out->lba = data_lba;
    out->size = size;
    out->flags = bytes[25];
    out->identifier_length = identifier_length;
    out->identifier = bytes + 33;
    out->raw = bytes;
    return OB_OK;
}

int ob_iso_mount(struct ob_iso *iso, ob_iso_sector_reader read_sector,
                 void *ctx, uint32_t sector_count) {
    uint8_t pvd[OB_ISO_SECTOR_SIZE];
    struct ob_iso candidate;
    struct iso_record root;
    uint32_t volume_size, volume_set_size, volume_sequence, block_size;
    uint8_t root_length;
    int rc;
    if (!iso || !read_sector) return OB_EINVAL;
    ob_zero(iso, sizeof(*iso));
    if (sector_count <= 16) return OB_EBOUNDS;
    if (read_sector(ctx, 16, pvd) != 0) return OB_EBOUNDS;
    if (pvd[0] != 1 || !ob_equal(pvd + 1, "CD001", 5) || pvd[6] != 1)
        return OB_EFORMAT;
    if (both32(pvd + 80, &volume_size) != OB_OK ||
        both16(pvd + 120, &volume_set_size) != OB_OK ||
        both16(pvd + 124, &volume_sequence) != OB_OK ||
        both16(pvd + 128, &block_size) != OB_OK)
        return OB_EFORMAT;
    if (volume_size <= 16 || volume_size > sector_count) return OB_EBOUNDS;
    if (volume_set_size != 1 || volume_sequence != 1) return OB_EFORMAT;
    if (block_size != OB_ISO_SECTOR_SIZE) return OB_EFORMAT;
    root_length = pvd[156];
    if (root_length < 34) return OB_EFORMAT;
    candidate.read_sector = read_sector;
    candidate.ctx = ctx;
    candidate.sector_count = volume_size;
    candidate.root_lba = 0;
    candidate.root_size = 0;
    rc = record_parse(&candidate, pvd + 156, root_length,
                      OB_ISO_SECTOR_SIZE - 156, &root);
    if (rc != OB_OK) return rc;
    if (!(root.flags & 0x02) || root.identifier_length != 1 ||
        root.identifier[0] != 0 || !root.size) return OB_EFORMAT;
    candidate.root_lba = root.lba;
    candidate.root_size = root.size;
    *iso = candidate;
    return OB_OK;
}

static int version_end(const uint8_t *name, size_t length, size_t *end) {
    size_t i;
    for (i = length; i > 0; --i) {
        if (name[i - 1] == ';') {
            size_t j;
            if (i == length) break;
            for (j = i; j < length; ++j)
                if (name[j] < '0' || name[j] > '9') break;
            if (j == length) {
                *end = i - 1;
                return OB_OK;
            }
            break;
        }
    }
    *end = length;
    return OB_OK;
}

static int name_matches(const uint8_t *identifier, size_t identifier_length,
                        const char *component, size_t component_length) {
    size_t left = identifier_length, right = component_length, i;
    version_end(identifier, identifier_length, &left);
    version_end((const uint8_t *)component, component_length, &right);
    while (left && identifier[left - 1] == '.') --left;
    while (right && component[right - 1] == '.') --right;
    if (left != right) return 0;
    for (i = 0; i < left; ++i)
        if (!ascii_equal(identifier[i], (uint8_t)component[i])) return 0;
    return 1;
}

static void entry_fill(const struct iso_record *r, struct ob_iso_entry *e) {
    uint32_t i, n = r->identifier_length;
    e->lba = r->lba;
    e->size = r->size;
    e->flags = r->flags;
    for (i = 0; i < 7; ++i) e->date[i] = r->raw[18 + i];
    if (n == 1 && (r->identifier[0] == 0 || r->identifier[0] == 1)) {
        e->name[0] = '.';
        n = r->identifier[0] ? 2 : 1;
        e->name[1] = '.';
    } else {
        if (n > OB_ISO_NAME_MAX - 1) n = OB_ISO_NAME_MAX - 1;
        for (i = 0; i < n; ++i) e->name[i] = (char)r->identifier[i];
    }
    e->name[n] = 0;
}

static int directory_find(const struct ob_iso *iso, uint32_t dir_lba,
                          uint32_t dir_size, const char *component,
                          size_t component_length, struct iso_record *found,
                          struct ob_iso_entry *entry) {
    uint8_t sector[OB_ISO_SECTOR_SIZE];
    uint32_t position = 0;
    int matches = 0;
    while (position < dir_size) {
        uint32_t lba = dir_lba + position / OB_ISO_SECTOR_SIZE;
        uint32_t within = position % OB_ISO_SECTOR_SIZE;
        uint32_t available = dir_size - position;
        uint32_t sector_available = OB_ISO_SECTOR_SIZE - within;
        int rc = sector_read(iso, lba, sector);
        if (rc != OB_OK) return rc;
        if (available > sector_available) available = sector_available;
        if (!sector[within]) {
            position += available;
            continue;
        }
        {
            struct iso_record item;
            uint8_t record_length = sector[within];
            rc = record_parse(iso, sector + within, available, sector_available, &item);
            if (rc != OB_OK) { TRACE("bad record offset", position); return rc; }
            if (!(item.identifier_length == 1 &&
                  (item.identifier[0] == 0 || item.identifier[0] == 1)) &&
                name_matches(item.identifier, item.identifier_length,
                             component, component_length)) {
                *found = item;
                if (entry) entry_fill(&item, entry);
                ++matches;
            }
            position += record_length;
        }
    }
    TRACE("directory matches", (uint32_t)matches);
    if (matches > 1) return OB_EFORMAT;
    return matches ? OB_OK : OB_ENOTFOUND;
}

static int prefix_equal(const char *path, const char *prefix, size_t length) {
    size_t i;
    for (i = 0; i < length; ++i)
        if (!path[i] || !ascii_equal((uint8_t)path[i], (uint8_t)prefix[i])) return 0;
    return 1;
}
/* Skip the device: "cdrom0:" or "cdrom:" without a unit number. */
static const char *skip_device(const char *path) {
    size_t n = 5;
    if (!prefix_equal(path, "cdrom", 5)) return path;
    while (path[n] >= '0' && path[n] <= '9') ++n;
    return path[n] == ':' ? path + n + 1 : path;
}

int ob_iso_find(const struct ob_iso *iso, const char *path,
                struct ob_iso_file *file) {
    const char *cursor;
    uint32_t dir_lba, dir_size;
    size_t path_length = 0;
    if (!iso || !iso->read_sector || !path || !file) return OB_EINVAL;
    while (path_length <= OB_ISO_PATH_MAX && path[path_length]) ++path_length;
    if (!path_length || path_length > OB_ISO_PATH_MAX) return OB_EINVAL;
    cursor = skip_device(path);
    while (*cursor == '/' || *cursor == '\\') ++cursor;
    dir_lba = iso->root_lba;
    dir_size = iso->root_size;
    if (!*cursor) return OB_EINVAL;
    while (*cursor) {
        const char *start = cursor;
        size_t length;
        struct iso_record found;
        int final, rc;
        while (*cursor && *cursor != '/' && *cursor != '\\') {
            if ((uint8_t)*cursor >= 0x80) return OB_EINVAL;
            ++cursor;
        }
        length = (size_t)(cursor - start);
        while (*cursor == '/' || *cursor == '\\') ++cursor;
        final = !*cursor;
        if (!length) continue;
        if (length == 1 && start[0] == '.') continue;
        if (length == 2 && start[0] == '.' && start[1] == '.') return OB_EINVAL;
        rc = directory_find(iso, dir_lba, dir_size, start, length, &found, 0);
        if (rc != OB_OK) return rc;
        if (final) {
            TRACE("found flags", found.flags);
            if (found.flags & 0x02) return OB_EFORMAT;
            file->lba = found.lba;
            file->size = found.size;
            return OB_OK;
        }
        if (!(found.flags & 0x02)) return OB_EFORMAT;
        dir_lba = found.lba;
        dir_size = found.size;
    }
    return OB_EINVAL;
}

int ob_iso_lookup(const struct ob_iso *iso, const char *path, struct ob_iso_entry *entry) {
    const char *cursor;
    uint32_t dir_lba, dir_size;
    if (!iso || !iso->read_sector || !path || !entry) return OB_EINVAL;
    cursor = skip_device(path);
    dir_lba = iso->root_lba;
    dir_size = iso->root_size;
    /* The root directory itself. */
    ob_zero(entry, sizeof(*entry));
    entry->lba = dir_lba;
    entry->size = dir_size;
    entry->flags = 0x02;
    entry->name[0] = '.';
    for (;;) {
        const char *start;
        size_t length;
        struct iso_record found;
        int rc;
        while (*cursor == '/' || *cursor == '\\') ++cursor;
        if (!*cursor) return OB_OK;
        if (!(entry->flags & 0x02)) return OB_ENOTFOUND;
        start = cursor;
        while (*cursor && *cursor != '/' && *cursor != '\\') {
            if ((uint8_t)*cursor >= 0x80) return OB_EINVAL;
            ++cursor;
        }
        length = (size_t)(cursor - start);
        if (length == 1 && start[0] == '.') continue;
        if (length == 2 && start[0] == '.' && start[1] == '.') return OB_EINVAL;
        rc = directory_find(iso, dir_lba, dir_size, start, length, &found, entry);
        if (rc != OB_OK) return rc;
        dir_lba = found.lba;
        dir_size = found.size;
    }
}

int ob_iso_dir_next(const struct ob_iso *iso, uint32_t dir_lba, uint32_t dir_size,
                    uint32_t *position, struct ob_iso_entry *entry) {
    uint8_t sector[OB_ISO_SECTOR_SIZE];
    if (!iso || !position || !entry) return OB_EINVAL;
    while (*position < dir_size) {
        uint32_t lba = dir_lba + *position / OB_ISO_SECTOR_SIZE;
        uint32_t within = *position % OB_ISO_SECTOR_SIZE;
        uint32_t sector_available = OB_ISO_SECTOR_SIZE - within;
        uint32_t available = dir_size - *position;
        struct iso_record item;
        int rc = sector_read(iso, lba, sector);
        if (rc != OB_OK) return rc;
        if (available > sector_available) available = sector_available;
        if (!sector[within]) { *position += available; continue; }
        rc = record_parse(iso, sector + within, available, sector_available, &item);
        if (rc != OB_OK) return rc;
        *position += sector[within];
        entry_fill(&item, entry);
        return OB_OK;
    }
    return OB_ENOTFOUND;
}

int ob_iso_read(const struct ob_iso *iso, const struct ob_iso_file *file,
                uint32_t offset, void *dst, uint32_t size) {
    uint8_t sector[OB_ISO_SECTOR_SIZE];
    uint8_t *output = dst;
    uint32_t left = size;
    uint64_t file_blocks;
    if (!iso || !iso->read_sector || !file || (!dst && size)) return OB_EINVAL;
    if (offset > file->size || size > file->size - offset) return OB_EBOUNDS;
    file_blocks = ((uint64_t)file->size + OB_ISO_SECTOR_SIZE - 1) /
        OB_ISO_SECTOR_SIZE;
    if (file->lba > iso->sector_count || file_blocks > iso->sector_count - file->lba)
        return OB_EBOUNDS;
    while (left) {
        uint32_t absolute = offset;
        uint32_t lba_offset = absolute / OB_ISO_SECTOR_SIZE;
        uint32_t in_sector = absolute % OB_ISO_SECTOR_SIZE;
        uint32_t chunk = OB_ISO_SECTOR_SIZE - in_sector;
        uint32_t i;
        int rc;
        if (chunk > left) chunk = left;
        if (file->lba > UINT32_MAX - lba_offset) return OB_EBOUNDS;
        rc = sector_read(iso, file->lba + lba_offset, sector);
        if (rc != OB_OK) return rc;
        for (i = 0; i < chunk; ++i) output[i] = sector[in_sector + i];
        output += chunk;
        offset += chunk;
        left -= chunk;
    }
    return OB_OK;
}

static int parse_boot_line(const char *line, size_t length, char *out,
                           size_t capacity) {
    size_t start = 0, equal = length, key_end, value_start, value_end, i;
    while (start < length && whitespace((uint8_t)line[start])) ++start;
    for (i = start; i < length; ++i) if (line[i] == '=') { equal = i; break; }
    if (equal == length) return OB_ENOTFOUND;
    key_end = equal;
    while (key_end > start && whitespace((uint8_t)line[key_end - 1])) --key_end;
    if (key_end - start != 5 || !ascii_equal((uint8_t)line[start], 'B') ||
        !ascii_equal((uint8_t)line[start + 1], 'O') ||
        !ascii_equal((uint8_t)line[start + 2], 'O') ||
        !ascii_equal((uint8_t)line[start + 3], 'T') ||
        !ascii_equal((uint8_t)line[start + 4], '2')) return OB_ENOTFOUND;
    value_start = equal + 1;
    while (value_start < length && whitespace((uint8_t)line[value_start])) ++value_start;
    value_end = length;
    while (value_end > value_start && whitespace((uint8_t)line[value_end - 1])) --value_end;
    if (value_end > value_start + 1 && line[value_start] == '"' &&
        line[value_end - 1] == '"') {
        ++value_start;
        --value_end;
    }
    if (value_start == value_end) return OB_EFORMAT;
    if (value_end - value_start + 1 > capacity) return OB_EBOUNDS;
    for (i = 0; i < value_end - value_start; ++i) {
        if ((uint8_t)line[value_start + i] < 0x20) return OB_EFORMAT;
        out[i] = line[value_start + i];
    }
    out[i] = 0;
    return OB_OK;
}

int ob_iso_boot_path(const struct ob_iso *iso, char *out, size_t capacity) {
    struct ob_iso_file file;
    char line[256];
    uint8_t chunk[128];
    uint32_t offset = 0;
    size_t line_length = 0;
    int rc;
    if (!iso || !out || capacity == 0) return OB_EINVAL;
    out[0] = 0;
    rc = ob_iso_find(iso, "SYSTEM.CNF;1", &file);
    TRACE("find SYSTEM.CNF result", (uint32_t)rc);
    if (rc != OB_OK) return rc;
    if (file.size > OB_ISO_SYSTEM_CNF_MAX) return OB_EBOUNDS;
    while (offset < file.size) {
        uint32_t amount = file.size - offset;
        uint32_t i;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        rc = ob_iso_read(iso, &file, offset, chunk, amount);
        if (rc != OB_OK) return rc;
        for (i = 0; i < amount; ++i) {
            uint8_t c = chunk[i];
            if (c == '\r' || c == '\n') {
                rc = parse_boot_line(line, line_length, out, capacity);
                line_length = 0;
                if (rc == OB_OK) return OB_OK;
                if (rc != OB_ENOTFOUND) return rc;
            } else {
                if (line_length + 1 >= sizeof(line)) return OB_EBOUNDS;
                line[line_length++] = (char)c;
            }
        }
        offset += amount;
    }
    if (line_length) return parse_boot_line(line, line_length, out, capacity);
    return OB_ENOTFOUND;
}
