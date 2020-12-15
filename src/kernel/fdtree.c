#include <pongo.h>

static inline uint32_t read_be_u32(uint8_t **ptr) {
    uint32_t val;

    memcpy(&val, *ptr, sizeof(val));
    *ptr += sizeof(val);

    return __bswap32(val);
}

static inline void align_4(uint8_t **ptr) {
    if ((uint64_t)*ptr & 0x3) {
        *ptr += 4 - ((uint64_t)*ptr & 0x3);
    }
}

// automatically skips nops
static inline uint32_t read_token(uint8_t **ptr) {
    uint32_t cur_token;

    do {
        cur_token = read_be_u32(ptr);
    } while(cur_token == FDT_NOP);

    return cur_token;
}

// verify header
bool fdt_parse_header(void *addr, fdt_header_t *header) {
    memcpy(header, addr, sizeof(*header));
    header->magic = __bswap32(header->magic);
    header->totalsize = __bswap32(header->totalsize);
    header->off_dt_struct = __bswap32(header->off_dt_struct);
    header->off_dt_strings = __bswap32(header->off_dt_strings);
    header->off_mem_rsvmap = __bswap32(header->off_mem_rsvmap);
    header->version = __bswap32(header->version);
    header->last_comp_version = __bswap32(header->last_comp_version);
    header->boot_cpuid_phys = __bswap32(header->boot_cpuid_phys);
    header->size_dt_strings = __bswap32(header->size_dt_strings);
    header->size_dt_struct = __bswap32(header->size_dt_struct);

    // char buf[200];
    // snprintf(buf, sizeof(buf), "magic=%#x mtotalsize=%#x off_dt_struct=%#x off_dt_strings=%#x off_mem_rsvmap=%#x version=%#x last_comp_version=%#x boot_cpuid_phys=%#x size_dt_strings=%#x size_dt_struct=%#x",
    //     header->magic, header->totalsize, header->off_dt_struct, header->off_dt_strings, header->off_mem_rsvmap, header->version, header->last_comp_version, header->boot_cpuid_phys, header->size_dt_strings, header->size_dt_struct);
    // screen_puts(buf);
    if (header->magic != FDT_HEADER_MAGIC) {
        return false;
    }

    return true;
}

void print_reserved_map(void *addr, fdt_header_t *header) {
    fdt_reserve_entry_t reserve_entry;
    uint64_t offset = (uint64_t)addr + header->off_mem_rsvmap;
    char buf[100];

    do {
        memcpy(&reserve_entry, (void *)offset, sizeof(reserve_entry));
        reserve_entry.address = __bswap64(reserve_entry.address);
        reserve_entry.size = __bswap64(reserve_entry.size);
        snprintf(buf, sizeof(buf), "reserved: %#llx (size %#llx)", reserve_entry.address, reserve_entry.size);
        screen_puts(buf);
        offset += sizeof(reserve_entry);
    } while(reserve_entry.address || reserve_entry.size);
}

static int parse_fdt_node(uint8_t **ptr, char *strings, int depth, int (*cb_node)(const char*, int), int (*cb_prop)(void*, const char*, int, const char*, void*, uint32_t), void *cb_args) {
    uint32_t cur_token = read_token(ptr);
    char *node_name;

    if (cur_token != FDT_BEGIN_NODE) {
        return 1;
    }

    node_name = (char *)*ptr;
    if (depth != 0 && cb_node) {
        int res = cb_node(node_name, depth);
        if (res != 0) {
            return res;
        }
    }
    *ptr += strlen(node_name) + 1;
    align_4(ptr);

    while((cur_token = read_token(ptr)) == FDT_PROP) {
        fdt_prop_t prop;
        char *prop_name;

        prop.len = read_be_u32(ptr);
        prop.nameoff = read_be_u32(ptr);

        prop_name = strings + prop.nameoff;
        int res = cb_prop(cb_args, node_name, depth, prop_name, *ptr, prop.len);
        if (res != 0) {
            return res;
        }
        *ptr += prop.len;
        align_4(ptr);
    }
    
    while (cur_token == FDT_BEGIN_NODE) {
        // push back ptr
        *ptr -= sizeof(cur_token);
        int res = parse_fdt_node(ptr, strings, depth + 1, cb_node, cb_prop, cb_args);
        if (res != 0) {
            return res;
        }

        cur_token = read_token(ptr);
    }
    if (cur_token != FDT_END_NODE) {
        return 1;
    }

    return 0;
}

int dump_fdtree_node_cb(const char *node_name, int depth) {
    char buf[100];
    snprintf(buf, sizeof(buf), "%*s- %s", depth * 2, "", node_name);
    screen_puts(buf);
    return 0;
}

int dump_fdtree_prop_cb(void *cb_args, const char *node_name, int depth, const char *prop_name, void *val, uint32_t len) {
    char buf[100];
    uint8_t *ptr = (uint8_t *)val;

    snprintf(buf, sizeof(buf), "%*s%-*s (size %#x)", (depth + 2) * 2, "", 15, prop_name, len);
    screen_puts(buf);

    for (uint i = 0; i < len; i += 4) {
        snprintf(buf, sizeof(buf), "%*s%02x %02x %02x %02x", (depth + 3) * 2, "",
                    ptr[i + 0], ptr[i + 1], ptr[i + 2], ptr[i + 3]);
        screen_puts(buf);
    }

    // continue
    return 0;
}

void dump_fdtree(void *addr, fdt_header_t *header) {
    uint8_t *cur_ptr = (uint8_t *)addr + header->off_dt_struct;
    char *strings = (char *)addr + header->off_dt_strings;

    screen_puts("dumping fdtree:");
    parse_fdt_node(&cur_ptr, strings, 0, dump_fdtree_node_cb, dump_fdtree_prop_cb, NULL);
    screen_puts("");
    screen_puts("");
}

typedef struct {
    const char *node_prefix;
    const char *prop_name;
    void *buf;
    uint32_t buflen;
} fdtree_find_args_t;

int find_fdtree_prop_cb(void *cb_args, const char *node_name, int depth, const char *prop_name, void *val, uint32_t len) {
    fdtree_find_args_t *find_args = (fdtree_find_args_t *) cb_args;

    if (!strncmp(node_name, find_args->node_prefix, strlen(find_args->node_prefix))
        && !strcmp(prop_name, find_args->prop_name)
        && (len == find_args->buflen)) {
        memcpy(find_args->buf, val, len);
        return 2; // found
    }
    return 0; // continue
}

bool fdtree_find_prop(void *addr, fdt_header_t *header, const char *node_prefix, const char *prop_name, void *buf, uint32_t buflen) {
    uint8_t *cur_ptr = (uint8_t *)addr + header->off_dt_struct;
    char *strings = (char *)addr + header->off_dt_strings;
    fdtree_find_args_t find_args = {
        .node_prefix = node_prefix,
        .prop_name = prop_name,
        .buf = buf,
        .buflen = buflen,
    };

    return (parse_fdt_node(&cur_ptr, strings, 0, NULL, find_fdtree_prop_cb, &find_args) == 2);
}