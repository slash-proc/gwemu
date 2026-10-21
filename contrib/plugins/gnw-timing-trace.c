/*
 * Bounded guest instruction and data-access trace for STM32H7B0 timing
 * analysis. This records guest behavior; it does not affect guest time.
 * The output is intentionally opt-in and may be expensive on the host.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define DEFAULT_MAX_EVENTS 500000

typedef struct TraceEvent {
    uint64_t pc;
    uint64_t vaddr;
    uint64_t paddr;
    uint32_t size;
    uint32_t vcpu;
    uint8_t insn_bytes[4];
    uint8_t kind;       /* I = instruction, R = data read, W = data write */
    uint8_t flags;      /* bit 0 = MMIO, bit 1 = resolved data physical address */
    uint8_t reserved[2];
} TraceEvent;

typedef struct InstructionSummary {
    uint64_t pc;
    uint64_t count;
    uint8_t size;
    uint8_t insn_bytes[4];
} InstructionSummary;

static FILE *output;
static uint64_t event_count;
static uint64_t instruction_event_count;
static bool output_failed;
static GPtrArray *insn_templates;
static GHashTable *instruction_summaries;
static GMutex events_lock;
static char *output_path;
static uint64_t max_events = DEFAULT_MAX_EVENTS;
static uint64_t code_cache_flushes;
static uint64_t pc_start;
static uint64_t pc_end = UINT64_MAX;
static bool have_pc_start;
static bool have_pc_end;
static bool record_instructions = true;
static bool record_instruction_summary;
static bool record_memory_accesses = true;
static uint64_t summary_instruction_events;
static uint64_t data_start;
static uint64_t data_end = UINT64_MAX;
static bool have_data_start;
static bool have_data_end;
static bool truncated;
static gint capture_stopped;

static guint instruction_summary_hash(gconstpointer data)
{
    const InstructionSummary *summary = data;
    guint hash = g_int64_hash(&summary->pc) * 31 + summary->size;

    for (uint8_t i = 0; i < summary->size; i++) {
        hash = hash * 31 + summary->insn_bytes[i];
    }
    return hash;
}

static gboolean instruction_summary_equal(gconstpointer a, gconstpointer b)
{
    const InstructionSummary *left = a;
    const InstructionSummary *right = b;

    return left->pc == right->pc && left->size == right->size &&
           memcmp(left->insn_bytes, right->insn_bytes, left->size) == 0;
}

static InstructionSummary *instruction_summary_get(uint64_t pc,
                                                     struct qemu_plugin_insn *insn)
{
    InstructionSummary lookup = { .pc = pc,
                                  .size = qemu_plugin_insn_size(insn) };
    InstructionSummary *summary;

    qemu_plugin_insn_data(insn, lookup.insn_bytes, sizeof(lookup.insn_bytes));
    g_mutex_lock(&events_lock);
    summary = g_hash_table_lookup(instruction_summaries, &lookup);
    if (!summary) {
        summary = g_memdup2(&lookup, sizeof(lookup));
        g_hash_table_add(instruction_summaries, summary);
    }
    g_mutex_unlock(&events_lock);
    return summary;
}

static gint instruction_summary_compare(gconstpointer a, gconstpointer b)
{
    const InstructionSummary *left = *(InstructionSummary * const *)a;
    const InstructionSummary *right = *(InstructionSummary * const *)b;

    if (left->pc != right->pc) {
        return left->pc < right->pc ? -1 : 1;
    }
    return memcmp(left->insn_bytes, right->insn_bytes,
                  MIN(left->size, right->size));
}

static bool parse_u64(const char *name, const char *value, uint64_t *out)
{
    char *end;
    guint64 parsed;

    if (!value || !*value || *value == '-') {
        fprintf(stderr, "gnw-timing-trace: invalid %s value\n", name);
        return false;
    }
    errno = 0;
    parsed = g_ascii_strtoull(value, &end, 0);
    if (errno == ERANGE || *end != '\0') {
        fprintf(stderr, "gnw-timing-trace: invalid %s value: %s\n",
                name, value);
        return false;
    }
    *out = parsed;
    return true;
}

static void append_event(const TraceEvent *event)
{
    char bytes[sizeof(event->insn_bytes) * 2 + 1] = { 0 };

    if (g_atomic_int_get(&capture_stopped)) {
        return;
    }
    g_mutex_lock(&events_lock);
    if (g_atomic_int_get(&capture_stopped)) {
        g_mutex_unlock(&events_lock);
        return;
    }
    if (event_count >= max_events) {
        truncated = true;
        g_atomic_int_set(&capture_stopped, 1);
        fprintf(stderr, "gnw-timing-trace: truncated at event limit\n");
        g_mutex_unlock(&events_lock);
        return;
    }
    if (event->kind == 'I') {
        for (uint32_t i = 0; i < event->size; i++) {
            g_snprintf(bytes + i * 2, 3, "%02x", event->insn_bytes[i]);
        }
    }
    /* One complete line per append. The output is line-buffered, so each
     * event reaches the OS before returning without retaining the trace in
     * RAM. A process crash preserves all successfully appended records. */
    if (fprintf(output, "%c,%u,0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64
                ",%u,%u,%u,%s\n", event->kind, event->vcpu, event->pc,
                event->vaddr, event->paddr, event->size,
                !!(event->flags & 1),
                event->kind == 'I' ? 0 : !!(event->flags & 2), bytes) < 0) {
        output_failed = true;
        g_atomic_int_set(&capture_stopped, 1);
        perror("gnw-timing-trace: writing output");
    } else {
        event_count++;
        instruction_event_count += event->kind == 'I';
    }
    g_mutex_unlock(&events_lock);
}

static void insn_exec(unsigned int vcpu, void *userdata)
{
    const TraceEvent *template = userdata;
    TraceEvent event = *template;

    event.vcpu = vcpu;
    append_event(&event);
}

static void insn_summary_exec(unsigned int vcpu, void *userdata)
{
    InstructionSummary *summary = userdata;

    if (g_atomic_int_get(&capture_stopped)) {
        return;
    }
    g_mutex_lock(&events_lock);
    if (summary_instruction_events < max_events) {
        summary->count++;
        summary_instruction_events++;
    } else {
        truncated = true;
        g_atomic_int_set(&capture_stopped, 1);
        fprintf(stderr, "gnw-timing-trace: truncated at event limit\n");
    }
    g_mutex_unlock(&events_lock);
}

static void mem_access(unsigned int vcpu, qemu_plugin_meminfo_t info,
                       uint64_t vaddr, void *userdata)
{
    uint64_t pc = (uintptr_t)userdata;
    struct qemu_plugin_hwaddr *hwaddr = qemu_plugin_get_hwaddr(info, vaddr);
    uint64_t paddr = hwaddr ? qemu_plugin_hwaddr_phys_addr(hwaddr) : vaddr;

    if (have_data_start || have_data_end) {
        if (!hwaddr || (have_data_start && paddr < data_start) ||
            (have_data_end && paddr >= data_end)) {
            return;
        }
    }
    TraceEvent event = {
        .pc = pc,
        .vaddr = vaddr,
        .paddr = paddr,
        .size = 1U << qemu_plugin_mem_size_shift(info),
        .vcpu = vcpu,
        .kind = qemu_plugin_mem_is_store(info) ? 'W' : 'R',
        .flags = (hwaddr && qemu_plugin_hwaddr_is_io(hwaddr) ? 1 : 0) |
                 (hwaddr ? 2 : 0),
    };

    append_event(&event);
}

static bool pc_selected(uint64_t pc)
{
    return (!have_pc_start || pc >= pc_start) &&
           (!have_pc_end || pc < pc_end);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t count = qemu_plugin_tb_n_insns(tb);

    if (g_atomic_int_get(&capture_stopped)) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        TraceEvent *event_template;

        if (!pc_selected(pc)) {
            continue;
        }

        if (qemu_plugin_insn_size(insn) > 4) {
            fprintf(stderr, "gnw-timing-trace: unsupported instruction width\n");
            g_mutex_lock(&events_lock);
            g_atomic_int_set(&capture_stopped, 1);
            output_failed = true;
            g_mutex_unlock(&events_lock);
            return;
        }
        if (record_instructions) {
            event_template = g_new0(TraceEvent, 1);
            event_template->pc = pc;
            event_template->vaddr = pc;
            event_template->paddr = pc;
            event_template->size = qemu_plugin_insn_size(insn);
            qemu_plugin_insn_data(insn, event_template->insn_bytes,
                                  sizeof(event_template->insn_bytes));
            event_template->kind = 'I';

            /* Plugin instruction userdata remains live for the translated TB.
             * The flush callback releases these templates after TB invalidation. */
            g_mutex_lock(&events_lock);
            g_ptr_array_add(insn_templates, event_template);
            g_mutex_unlock(&events_lock);

            qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
                                                    QEMU_PLUGIN_CB_NO_REGS,
                                                    event_template);
        } else if (record_instruction_summary) {
            InstructionSummary *summary = instruction_summary_get(pc, insn);

            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, insn_summary_exec, QEMU_PLUGIN_CB_NO_REGS, summary);
        }
        if (record_memory_accesses) {
            qemu_plugin_register_vcpu_mem_cb(insn, mem_access,
                                             QEMU_PLUGIN_CB_NO_REGS,
                                             QEMU_PLUGIN_MEM_RW,
                                             (void *)(uintptr_t)pc);
        }
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *userdata)
{
    g_mutex_lock(&events_lock);
    if (record_instruction_summary) {
        GPtrArray *ordered = g_ptr_array_new();
        GHashTableIter iter;
        gpointer value;

        g_hash_table_iter_init(&iter, instruction_summaries);
        while (g_hash_table_iter_next(&iter, &value, NULL)) {
            g_ptr_array_add(ordered, value);
        }
        g_ptr_array_sort(ordered, instruction_summary_compare);
        /* The summary is aggregated and written only at clean shutdown. */
        for (guint i = 0; i < ordered->len; i++) {
            const InstructionSummary *summary = g_ptr_array_index(ordered, i);

            /* TB translation covers both paths of conditional branches.
             * Keep the summary limited to instructions that actually ran;
             * zero-count rows are translated code, not part of this trace. */
            if (!summary->count) {
                continue;
            }
            fprintf(output, "0x%" PRIx64 ",%u,", summary->pc, summary->size);
            for (uint32_t j = 0; j < summary->size; j++) {
                fprintf(output, "%02x", summary->insn_bytes[j]);
            }
            fprintf(output, ",%" PRIu64 "\n", summary->count);
        }
        g_ptr_array_free(ordered, true);
    }
    if (ferror(output)) {
        output_failed = true;
    }
    if (fclose(output) != 0) {
        output_failed = true;
    }
    if (output_failed) {
        fprintf(stderr, "gnw-timing-trace: output write/close failed\n");
    }
    fprintf(stderr, "gnw-timing-trace: wrote %" PRIu64
            " instruction executions and %" PRIu64 " events to %s%s%s\n",
            record_instruction_summary ? summary_instruction_events :
                                         instruction_event_count,
            event_count, output_path,
            truncated ? " (truncated at event limit)" : "",
            output_failed ? " (output failed)" : "");
    fprintf(stderr, "gnw-timing-trace: released templates at %" PRIu64
            " code-cache flushes\n", code_cache_flushes);
    g_ptr_array_free(insn_templates, true);
    g_hash_table_destroy(instruction_summaries);
    g_mutex_unlock(&events_lock);
}

static void code_cache_flush(qemu_plugin_id_t id)
{
    GHashTableIter iter;
    gpointer value;

    /* Callback userdata must remain valid while a translated block can call
     * it. QEMU guarantees the flush callback runs with all vCPUs stopped, so
     * templates belonging to the discarded translation cache are now safe to
     * release. Keeping only live-TB templates avoids growth across repeated
     * code-cache flushes/self-modifying workloads. */
    g_ptr_array_set_size(insn_templates, 0);
    /* Discard summary entries created for code that was translated but
     * never executed. No invalidated TB can still reference these entries.
     * Executed summaries are bounded by the instruction event limit. */
    g_hash_table_iter_init(&iter, instruction_summaries);
    while (g_hash_table_iter_next(&iter, &value, NULL)) {
        if (!((InstructionSummary *)value)->count) {
            g_hash_table_iter_remove(&iter);
        }
    }
    code_cache_flushes++;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    if (!info->system_emulation) {
        fprintf(stderr, "gnw-timing-trace: requires system emulation\n");
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) option = g_strsplit(argv[i], "=", 2);

        if (!option[0] || !option[1] || option[2]) {
            fprintf(stderr, "gnw-timing-trace: expected key=value: %s\n",
                    argv[i]);
            return -1;
        }
        const char *key = option[0];
        const char *arg = option[1];

        if (!strcmp(key, "out")) {
            g_free(output_path);
            output_path = g_strdup(arg);
        } else if (!strcmp(key, "max")) {
            if (!parse_u64(key, arg, &max_events) || !max_events ||
                max_events > G_MAXUINT) {
                return -1;
            }
        } else if (!strcmp(key, "start")) {
            if (!parse_u64(key, arg, &pc_start)) {
                return -1;
            }
            have_pc_start = true;
        } else if (!strcmp(key, "end")) {
            if (!parse_u64(key, arg, &pc_end)) {
                return -1;
            }
            have_pc_end = true;
        } else if (!strcmp(key, "data-start")) {
            if (!parse_u64(key, arg, &data_start)) {
                return -1;
            }
            have_data_start = true;
        } else if (!strcmp(key, "data-end")) {
            if (!parse_u64(key, arg, &data_end)) {
                return -1;
            }
            have_data_end = true;
        } else if (!strcmp(key, "insn")) {
            if (!strcmp(arg, "1") || !strcmp(arg, "on") ||
                !strcmp(arg, "true")) {
                record_instructions = true;
            } else if (!strcmp(arg, "0") || !strcmp(arg, "off") ||
                       !strcmp(arg, "false")) {
                record_instructions = false;
            } else {
                fprintf(stderr, "gnw-timing-trace: invalid insn value: %s\n", arg);
                return -1;
            }
        } else if (!strcmp(key, "insn-summary")) {
            if (!strcmp(arg, "1") || !strcmp(arg, "on") ||
                !strcmp(arg, "true")) {
                record_instruction_summary = true;
                record_instructions = false;
            } else if (!strcmp(arg, "0") || !strcmp(arg, "off") ||
                       !strcmp(arg, "false")) {
                record_instruction_summary = false;
            } else {
                fprintf(stderr, "gnw-timing-trace: invalid insn-summary value: %s\n",
                        arg);
                return -1;
            }
        } else if (!strcmp(key, "mem")) {
            if (!strcmp(arg, "1") || !strcmp(arg, "on") ||
                !strcmp(arg, "true")) {
                record_memory_accesses = true;
            } else if (!strcmp(arg, "0") || !strcmp(arg, "off") ||
                       !strcmp(arg, "false")) {
                record_memory_accesses = false;
            } else {
                fprintf(stderr, "gnw-timing-trace: invalid mem value: %s\n", arg);
                return -1;
            }
        } else {
            fprintf(stderr, "gnw-timing-trace: unknown option: %s\n", key);
            return -1;
        }
    }

    if (!output_path || !*output_path ||
        (have_pc_start && have_pc_end && pc_start >= pc_end) ||
        (have_data_start && have_data_end && data_start >= data_end) ||
        (record_instruction_summary && record_instructions)) {
        fprintf(stderr, "gnw-timing-trace: require out=, valid address ranges, and no conflicting trace modes\n");
        return -1;
    }

    if (record_instruction_summary && record_memory_accesses) {
        fprintf(stderr, "gnw-timing-trace: summary mode requires mem=off\n");
        return -1;
    }
    output = fopen(output_path, "w");
    if (!output) {
        perror("gnw-timing-trace: opening output");
        return -1;
    }
    if (setvbuf(output, NULL, _IOLBF, 4096) != 0 ||
        fprintf(output, "%s\n", record_instruction_summary ?
                "pc,size,insn_bytes,count" :
                "kind,vcpu,pc,vaddr,paddr,size,io,paddr_valid,insn_bytes") < 0) {
        fprintf(stderr, "gnw-timing-trace: cannot initialize output\n");
        fclose(output);
        return -1;
    }
    insn_templates = g_ptr_array_new_with_free_func(g_free);
    instruction_summaries = g_hash_table_new_full(instruction_summary_hash,
                                                   instruction_summary_equal,
                                                   g_free, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_flush_cb(id, code_cache_flush);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
