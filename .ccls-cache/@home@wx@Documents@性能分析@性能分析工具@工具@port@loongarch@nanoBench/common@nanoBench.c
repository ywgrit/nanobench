// nanoBench
//
// Copyright (C) 2019 Andreas Abel
//
// This program is free software: you can redistribute it and/or modify it under the terms of version 3 of the GNU Affero General Public License.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "nanoBench.h"
/* #include <stdio.h> */

long n_measurements = N_MEASUREMENTS_DEFAULT;
long unroll_count = UNROLL_COUNT_DEFAULT;
long loop_count = LOOP_COUNT_DEFAULT;
long warm_up_count = WARM_UP_COUNT_DEFAULT;
long initial_warm_up_count = INITIAL_WARM_UP_COUNT_DEFAULT;
size_t alignment_offset = ALIGNMENT_OFFSET_DEFAULT;
int drain_frontend = DRAIN_FRONTEND_DEFAULT;
int no_mem = NO_MEM_DEFAULT;
int no_normalization = NO_NORMALIZATION_DEFAULT;
int basic_mode = BASIC_MODE_DEFAULT;
int use_fixed_counters = USE_FIXED_COUNTERS_DEFAULT;
int aggregate_function = AGGREGATE_FUNCTION_DEFAULT;
int verbose = VERBOSE_DEFAULT;
int debug = DEBUG_DEFAULT;

// code是使用-code 参数时的二进制或者--asm 参数对应的汇编代码汇编出的二进制的text段
char* code = NULL;
// code的长度
size_t code_length = 0;

char* code_init = NULL;
size_t code_init_length = 0;

char* code_late_init = NULL;
size_t code_late_init_length = 0;

char* code_one_time_init = NULL;
size_t code_one_time_init_length = 0;

struct pfc_config pfc_configs[2000] = {{0}};
size_t n_pfc_configs = 0;
char* pfc_config_file_content = NULL;

struct msr_config msr_configs[2000] = {{0}};
size_t n_msr_configs = 0;
char* msr_config_file_content = NULL;
unsigned long cur_rdmsr = 0;

bool is_Intel_CPU = false;
bool is_AMD_CPU = false;
bool supports_tsc_deadline = false;
int displ_family;
int displ_model;
int Intel_perf_mon_ver = -1;
int Intel_FF_ctr_width = -1;
int Intel_programmable_ctr_width = -1;

int n_programmable_counters;

char* runtime_code;
char* runtime_one_time_init_code;
void* runtime_r14;
void* runtime_rbp;
void* runtime_rdi;
void* runtime_rsi;
void* runtime_rsp;
int64_t pfc_mem[MAX_PROGRAMMABLE_COUNTERS];
void* RSP_mem;

int64_t* measurement_results[MAX_PROGRAMMABLE_COUNTERS];
int64_t* measurement_results_base[MAX_PROGRAMMABLE_COUNTERS];

int cpu = -1;

const char* NOPS[] = {
    "",
    "\x90",
    "\x66\x90",
    "\x0f\x1f\x00",
    "\x0f\x1f\x40\x00",
    "\x0f\x1f\x44\x00\x00",
    "\x66\x0f\x1f\x44\x00\x00",
    "\x0f\x1f\x80\x00\x00\x00\x00",
    "\x0f\x1f\x84\x00\x00\x00\x00\x00",
    "\x66\x0f\x1f\x84\x00\x00\x00\x00\x00",
    "\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00",
    "\x66\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00",
    "\x66\x66\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00",
    "\x66\x66\x66\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00",
    "\x66\x66\x66\x66\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00",
    "\x66\x66\x66\x66\x66\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00"
};

void build_cpuid_string(char* buf, unsigned int r0, unsigned int r1, unsigned int r2, unsigned int r3) {
    memcpy(buf,    (char*)&r0, 4);
    memcpy(buf+4,  (char*)&r1, 4);
    memcpy(buf+8,  (char*)&r2, 4);
    memcpy(buf+12, (char*)&r3, 4);
}

bool check_cpuid() {
    unsigned int eax, ebx, ecx, edx;
    __cpuid(0, eax, ebx, ecx, edx);

    char proc_vendor_string[17] = {0};
    build_cpuid_string(proc_vendor_string, ebx, edx, ecx, 0);
    print_user_verbose("Vendor ID: %s\n", proc_vendor_string);

    char proc_brand_string[48];
    __cpuid(0x80000002, eax, ebx, ecx, edx);
    build_cpuid_string(proc_brand_string, eax, ebx, ecx, edx);
    __cpuid(0x80000003, eax, ebx, ecx, edx);
    build_cpuid_string(proc_brand_string+16, eax, ebx, ecx, edx);
    __cpuid(0x80000004, eax, ebx, ecx, edx);
    build_cpuid_string(proc_brand_string+32, eax, ebx, ecx, edx);
    print_user_verbose("Brand: %s\n", proc_brand_string);

    __cpuid(0x01, eax, ebx, ecx, edx);
    displ_family = ((eax >> 8) & 0xF);
    if (displ_family == 0x0F) {
        displ_family += ((eax >> 20) & 0xFF);
    }
    displ_model = ((eax >> 4) & 0xF);
    if (displ_family == 0x06 || displ_family == 0x0F) {
        displ_model += ((eax >> 12) & 0xF0);
    }
    print_user_verbose("DisplayFamily_DisplayModel: %.2X_%.2XH\n", displ_family, displ_model);
    print_user_verbose("Stepping ID: %u\n", (eax & 0xF));

    if (strcmp(proc_vendor_string, "GenuineIntel") == 0) {
        is_Intel_CPU = true;

        __cpuid(0x01, eax, ebx, ecx, edx);
        supports_tsc_deadline = (ecx >> 24) & 1;

        __cpuid(0x0A, eax, ebx, ecx, edx);
        Intel_perf_mon_ver = (eax & 0xFF);
        print_user_verbose("Performance monitoring version: %d\n", Intel_perf_mon_ver);
        if (Intel_perf_mon_ver < 2) {
            print_error("Error: performance monitoring version >= 2 required\n");
            return true;
        }

        print_user_verbose("Number of fixed-function performance counters: %u\n", edx & 0x1F);
        n_programmable_counters = ((eax >> 8) & 0xFF);
        print_user_verbose("Number of general-purpose performance counters: %u\n", n_programmable_counters);
        if (n_programmable_counters < 2) {
            print_error("Error: only %u programmable counters available; nanoBench requires at least 2\n", n_programmable_counters);
            return true;
        }

        Intel_FF_ctr_width = (edx >> 5) & 0xFF;
        Intel_programmable_ctr_width = (eax >> 16) & 0xFF;
        print_user_verbose("Bit widths of fixed-function performance counters: %u\n", Intel_FF_ctr_width);
        print_user_verbose("Bit widths of general-purpose performance counters: %u\n", Intel_programmable_ctr_width);
    } else if (strcmp(proc_vendor_string, "AuthenticAMD") == 0) {
        is_AMD_CPU = true;
        n_programmable_counters = 6;
    } else {
        print_error("Error: unsupported CPU found\n");
        return true;
    }

    return false;
}

void parse_counter_configs() {
    n_pfc_configs = 0;

    char* line;
    char* next_line = pfc_config_file_content;
    while ((line = strsep(&next_line, "\n")) != NULL) {
        if (strlen(line) == 0 || line[0] == '#') {
            continue;
        }

        pfc_configs[n_pfc_configs] = (struct pfc_config){0};
        pfc_configs[n_pfc_configs].ctr = -1;

        char* config_str = strsep(&line, " \t");

        if (line && strlen(line) > 0) {
            pfc_configs[n_pfc_configs].description = line;
        } else {
            pfc_configs[n_pfc_configs].description = config_str;
        }

        char buf[50];
        if (strlen(config_str) >= sizeof(buf)) {
            print_error("config string too long: %s\n", config_str);
            continue;
        }
        strcpy(buf, config_str);

        char* tok = buf;

        char* evt_num = strsep(&tok, ".");
        pfc_configs[n_pfc_configs].evt_num = strtoul(evt_num, NULL, 16);

        if (!tok) {
            print_error("invalid configuration: %s\n", config_str);
            continue;
        }

        char* umask = strsep(&tok, ".");
        pfc_configs[n_pfc_configs].umask = strtoul(umask, NULL, 16);

        char* ce;
        while ((ce = strsep(&tok, ".")) != NULL) {
            if (!strcmp(ce, "AnyT")) {
                pfc_configs[n_pfc_configs].any = true;
            } else if (!strcmp(ce, "EDG")) {
                pfc_configs[n_pfc_configs].edge = true;
            } else if (!strcmp(ce, "INV")) {
                pfc_configs[n_pfc_configs].inv = true;
            } else if (!strcmp(ce, "TakenAlone")) {
                pfc_configs[n_pfc_configs].taken_alone = true;
            } else if (!strncmp(ce, "CTR=", 4)) {
                unsigned long counter;
                counter = strtoul(ce+4, NULL, 0);
                if (counter > n_programmable_counters) {
                    print_error("invalid counter: %s\n", ce);
                    continue;
                }
                pfc_configs[n_pfc_configs].ctr = counter;
            } else if (!strncmp(ce, "CMSK=", 5)) {
                pfc_configs[n_pfc_configs].cmask = strtoul(ce+5, NULL, 0);
            } else if (!strncmp(ce, "MSR_3F6H=", 9)) {
                pfc_configs[n_pfc_configs].msr_3f6h = strtoul(ce+9, NULL, 0);
            } else if (!strncmp(ce, "MSR_PF=", 7)) {
                pfc_configs[n_pfc_configs].msr_pf = strtoul(ce+7, NULL, 0);
            } else if (!strncmp(ce, "MSR_RSP0=", 9)) {
                pfc_configs[n_pfc_configs].msr_rsp0 = strtoul(ce+9, NULL, 0);
            } else if (!strncmp(ce, "MSR_RSP1=", 9)) {
                pfc_configs[n_pfc_configs].msr_rsp1 = strtoul(ce+9, NULL, 0);
            }
        }
        n_pfc_configs++;
    }
}

#ifdef __KERNEL__
void parse_msr_configs() {
    n_msr_configs = 0;

    char* line;
    char* next_line = msr_config_file_content;
    while ((line = strsep(&next_line, "\n")) != NULL) {
        if (strlen(line) == 0 || line[0] == '#') {
            continue;
        }

        char* wrmsr_str = strsep(&line, " \t");
        char* rdmsr_str = strsep(&line, " \t");

        if (line && strlen(line) > 0) {
            msr_configs[n_msr_configs].description = line;
        } else {
            msr_configs[n_msr_configs].description = rdmsr_str;
            rdmsr_str = wrmsr_str;
            wrmsr_str = line;
        }

        strreplace(rdmsr_str, 'h', '\0'); strreplace(rdmsr_str, 'H', '\0');
        msr_configs[n_msr_configs].rdmsr = strtoul(rdmsr_str+4, NULL, 16);

        size_t n_wrmsr = 0;
        char* tok = wrmsr_str;
        char* ce;
        while ((ce = strsep(&tok, ".")) != NULL) {
            if (n_wrmsr >= 10) {
                print_error("Error: n_wrmsr >= 10");
                break;
            }

            char* msr_str = strsep(&ce, "=")+4;
            pr_debug("msr_str: %s", msr_str);
            strreplace(msr_str, 'h', '\0'); strreplace(msr_str, 'H', '\0');
            msr_configs[n_msr_configs].wrmsr[n_wrmsr] = strtoul(msr_str, NULL, 16);
            strreplace(ce, 'h', '\0'); strreplace(ce, 'H', '\0');
            msr_configs[n_msr_configs].wrmsr_val[n_wrmsr] = strtoul(ce, NULL, 0);
            n_wrmsr++;
        }
        msr_configs[n_msr_configs].n_wrmsr = n_wrmsr;
        n_msr_configs++;
    }
}
#endif

#ifndef __KERNEL__
uint64_t read_value_from_cmd(char* cmd) {
    FILE* fp;
    if(!(fp = popen(cmd, "r"))){
        print_error("Error reading from \"%s\"", cmd);
        return 0;
    }

    char buf[20];
    fgets(buf, sizeof(buf), fp);
    pclose(fp);

    uint64_t val;
    val = strtoul(buf, NULL, 0);
    return val;
}
#endif

uint64_t read_msr(unsigned int msr) {
    #ifdef __KERNEL__
        return native_read_msr(msr);
    #else
        char cmd[50];
        snprintf(cmd, sizeof(cmd), "rdmsr -c -p%d %#x", cpu, msr);
        return read_value_from_cmd(cmd);
    #endif
}

void write_msr(unsigned int msr, uint64_t value) {
    #ifdef __KERNEL__
        native_write_msr(msr, (uint32_t)value, (uint32_t)(value>>32));
    #else
        char cmd[50];
        snprintf(cmd, sizeof(cmd), "wrmsr -p%d %#x %#lx", cpu, msr, value);
        /* printf("cmd == %s\n", cmd); */
        if (system(cmd)) {
            print_error("\"%s\" failed. You may need to disable Secure Boot (see README.md).", cmd);
            exit(1);
        }
    #endif
}

void change_bit_in_msr(unsigned int msr, unsigned int bit, bool bit_value) {
    uint64_t msr_value = read_msr(msr);
    msr_value &= ~((uint64_t)1 << bit);
    msr_value |= ((uint64_t)bit_value << bit);
    write_msr(msr, msr_value);
}

void set_bit_in_msr(unsigned int msr, unsigned int bit) {
    change_bit_in_msr(msr, bit, true);
}

void clear_bit_in_msr(unsigned int msr, unsigned int bit) {
    change_bit_in_msr(msr, bit, false);
}

uint64_t read_pmc(unsigned int counter) {
    unsigned long lo, hi;
    asm volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(counter));
    return lo | ((uint64_t)hi) << 32;
}

void clear_perf_counters() {
    if (is_Intel_CPU) {
        for (int i=0; i<3; i++) {
            write_msr(MSR_IA32_FIXED_CTR0+i, 0);
        }
        for (int i=0; i<n_programmable_counters; i++) {
            write_msr(MSR_IA32_PMC0+i, 0);
        }
    } else {
        for (int i=0; i<n_programmable_counters; i++) {
            write_msr(CORE_X86_MSR_PERF_CTR+(2*i), 0);
        }
    }
}

void clear_perf_counter_configurations() {
    if (is_Intel_CPU) {
        write_msr(MSR_IA32_FIXED_CTR_CTRL, 0);
        for (int i=0; i<n_programmable_counters; i++) {
            write_msr(MSR_IA32_PERFEVTSEL0+i, 0);
        }
    } else {
        for (int i=0; i<n_programmable_counters; i++) {
            write_msr(CORE_X86_MSR_PERF_CTL + (2*i), 0);
        }
    }
}

void clear_overflow_status_bits() {
    if (is_Intel_CPU) {
        write_msr(IA32_PERF_GLOBAL_STATUS_RESET, read_msr(IA32_PERF_GLOBAL_STATUS));
    }
}

void enable_perf_ctrs_globally() {
    if (is_Intel_CPU) {
        write_msr(MSR_IA32_PERF_GLOBAL_CTRL, ((uint64_t)7 << 32) | ((1 << n_programmable_counters) - 1));
    }
}

void disable_perf_ctrs_globally() {
    if (is_Intel_CPU) {
        write_msr(MSR_IA32_PERF_GLOBAL_CTRL, 0);
    }
}

void enable_freeze_on_PMI(void) {
    if (is_Intel_CPU) {
        set_bit_in_msr(MSR_IA32_DEBUGCTL, 12);
    }
}

void disable_freeze_on_PMI(void) {
    if (is_Intel_CPU) {
        clear_bit_in_msr(MSR_IA32_DEBUGCTL, 12);
    }
}

void configure_perf_ctrs_FF_Intel(bool usr, bool os) {
    uint64_t fixed_ctrl = 0;
    fixed_ctrl |= (os << 8) | (os << 4) | os;
    fixed_ctrl |= (usr << 9) | (usr << 5) | (usr << 1);
    write_msr(MSR_IA32_FIXED_CTR_CTRL, fixed_ctrl);
}

size_t configure_perf_ctrs_programmable(size_t next_pfc_config, bool usr, bool os, int n_counters, int avoid_counters, char* descriptions[]) {
    if (is_Intel_CPU) {
        bool evt_added = false;
        for (int i=0; i<n_counters; i++) {
            if (next_pfc_config >= n_pfc_configs) {
                break;
            }

            struct pfc_config config = pfc_configs[next_pfc_config];
            if (config.taken_alone && evt_added) {
                break;
            }
            if ((config.ctr != -1) && (config.ctr != i)) {
                if (config.ctr >= n_counters) {
                    print_error("Counter %u is not available", config.ctr);
                    next_pfc_config++;
                }
                continue;
            }
            if (((avoid_counters >> i) & 1) && (config.ctr != i)) {
                continue;
            }
            next_pfc_config++;

            descriptions[i] = config.description;

            // perfevtselx 就是性能监测配置器，在龙芯架构手册第139页
            uint64_t perfevtselx = ((config.cmask & 0xFF) << 24);
            perfevtselx |= (config.inv << 23);
            perfevtselx |= (1ULL << 22);
            perfevtselx |= (config.any << 21);
            perfevtselx |= (config.edge << 18);
            perfevtselx |= (os << 17);
            perfevtselx |= (usr << 16);
            perfevtselx |= ((config.umask & 0xFF) << 8);
            perfevtselx |= (config.evt_num & 0xFF);
            write_msr(MSR_IA32_PERFEVTSEL0+i, perfevtselx);

            if (config.msr_3f6h) {
                write_msr(0x3f6, config.msr_3f6h);
            }
            if (config.msr_pf) {
                write_msr(MSR_PEBS_FRONTEND, config.msr_pf);
            }
            if (config.msr_rsp0) {
                write_msr(MSR_OFFCORE_RSP0, config.msr_rsp0);
            }
            if (config.msr_rsp1) {
                write_msr(MSR_OFFCORE_RSP1, config.msr_rsp1);
            }

            evt_added = true;
            if (config.taken_alone) {
                break;
            }
        }
    } else {
        for (int i=0; i<n_counters; i++) {
            if (next_pfc_config >= n_pfc_configs) {
                write_msr(CORE_X86_MSR_PERF_CTL + (2*i), 0);
                continue;
            }

            struct pfc_config config = pfc_configs[next_pfc_config];
            if ((config.ctr != -1) && (config.ctr != i)) {
                if (config.ctr >= n_counters) {
                    print_error("Counter %u is not available", config.ctr);
                    next_pfc_config++;
                }
                continue;
            }
            if (((avoid_counters >> i) & 1) && (config.ctr != i)) {
                continue;
            }
            next_pfc_config++;

            descriptions[i] = config.description;

            uint64_t perf_ctl = 0;
            perf_ctl |= ((config.evt_num) & 0xF00) << 24;
            perf_ctl |= (config.evt_num) & 0xFF;
            perf_ctl |= ((config.umask) & 0xFF) << 8;
            perf_ctl |= ((config.cmask) & 0x7F) << 24;
            perf_ctl |= (config.inv << 23);
            perf_ctl |= (1ULL << 22);
            perf_ctl |= (config.edge << 18);
            perf_ctl |= (os << 17);
            perf_ctl |= (usr << 16);

            write_msr(CORE_X86_MSR_PERF_CTL + (2*i), perf_ctl);
        }
    }
    return next_pfc_config;
}

void configure_MSRs(struct msr_config config) {
    for (size_t i=0; i<config.n_wrmsr; i++) {
        write_msr(config.wrmsr[i], config.wrmsr_val[i]);
    }
    cur_rdmsr = config.rdmsr;
}

// 该函数是和测试二进制相关的，不需要理会
size_t get_required_runtime_code_length() {
    size_t req_code_length = code_length + alignment_offset + 64;
    for (size_t i=0; i+7<code_length; i++) {
        if (starts_with_magic_bytes(&code[i], MAGIC_BYTES_CODE_PFC_START) || starts_with_magic_bytes(&code[i], MAGIC_BYTES_CODE_PFC_STOP)) {
            req_code_length += 100;
        }
    }
    return code_init_length + code_late_init_length + (drain_frontend?3*(192+64*15):0) + 2*unroll_count*req_code_length + 10000;
}

int get_distance_to_code(char* measurement_template, size_t templateI) {
    int dist = 0;
    while (!starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_CODE)) {
        if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_PFC_START)) {
            templateI += 8;
        } else {
            templateI++;
            dist++;
        }
    }
    return dist;
}

// TODO(wx): 这是重点，需要移植的核心
void create_runtime_code(char* measurement_template, long local_unroll_count, long local_loop_count) {
    // I表示index
    size_t templateI = 0;
    size_t codeI = 0;
    long unrollI = 0;
    size_t rcI = 0;
    size_t rcI_code_start = 0;
    size_t magic_bytes_pfc_start_I = 0;
    size_t magic_bytes_code_I = 0;

    // 如果code_contains_magic_bytes = 1，那么no_mem选项被使能了
    int code_contains_magic_bytes = 0;
    for (size_t i=0; i+7<code_length; i++) {
        if (starts_with_magic_bytes(&code[i], MAGIC_BYTES_CODE_PFC_START) || starts_with_magic_bytes(&code[i], MAGIC_BYTES_CODE_PFC_STOP)) {
            if (!no_mem) {
                print_error("starting/stopping the counters is only supported in no_mem mode");
                return;
            }
            code_contains_magic_bytes = 1;
            break;
        }
    }

    while (!starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_TEMPLATE_END)) {
        if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_INIT)) {
            // 跳过MAGIC_BYTES_INIT这个魔数
            templateI += 8;
            memcpy(&runtime_code[rcI], code_init, code_init_length);
            rcI += code_init_length;
    
            // TODO(wx): 如果需要循环，将循环次数放在r15寄存器中，这里是插入"mov R15 local_loop_count"这条指令，需要修改为loongarch指令
            if (local_loop_count > 0) {
                strcpy(&runtime_code[rcI], "\x49\xC7\xC7"); rcI += 3;
                *(int32_t*)(&runtime_code[rcI]) = (int32_t)local_loop_count; rcI += 4; // mov R15, local_loop_count 
            }

            // runtime_code的{SAVE_REGS_FLAGS、code_init、loop 、code_late_init}需要用nop指令64字节对齐，后面就是code了
            int dist = get_distance_to_code(measurement_template, templateI) + code_late_init_length;
            // 下面这行为什么这样写？因为n_fill可能等于0
            int n_fill = (64 - ((uintptr_t)&runtime_code[rcI+dist] % 64)) % 64;
            n_fill += alignment_offset;
            while (n_fill > 0) {
                int nop_len = min(15, n_fill);
                strcpy(&runtime_code[rcI], NOPS[nop_len]); rcI += nop_len;
                n_fill -= nop_len;
            }

            // 如果这个选项被使能，那么在code_init、code_late_init、code后会使用一系列单nop和15nop来清空front-end buffers
            if (drain_frontend) {
                strcpy(&runtime_code[rcI], "\x0F\xAE\xE8"); rcI += 3; // lfence
                for (int i=0; i<189; i++) {
                    strcpy(&runtime_code[rcI], NOPS[1]); rcI += 1;
                }
                for (int i=0; i<64; i++) {
                    strcpy(&runtime_code[rcI], NOPS[15]); rcI += 15;
                }
            }
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_PFC_START)) {
            magic_bytes_pfc_start_I = templateI;
            templateI += 8;
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_CODE)) {
            magic_bytes_code_I = templateI;
            templateI += 8;

            if (unrollI == 0 && codeI == 0) {
                if (code_late_init_length > 0) {
                    memcpy(&runtime_code[rcI], code_late_init, code_late_init_length);
                    rcI += code_late_init_length;
                }

                if (drain_frontend) {
                    // We first execute an lfence instruction, then, we fill the front-end buffers with 1-Byte NOPs, and then, we drain the buffers using
                    // 15-Byte NOPs; this makes sure that before the first 15-Byte NOP is predecoded, the front-end buffers contain only NOPs that can be
                    // issued at the maximum rate. The length of the added instructions is a multiple of 64, and thus doesn't affect the alignment.
                    strcpy(&runtime_code[rcI], "\x0F\xAE\xE8"); rcI += 3; // lfence
                    for (int i=0; i<189; i++) {
                        strcpy(&runtime_code[rcI], NOPS[1]); rcI += 1;
                    }
                    for (int i=0; i<64; i++) {
                        strcpy(&runtime_code[rcI], NOPS[15]); rcI += 15;
                    }
                }

                rcI_code_start = rcI;
            }

            if (!code_contains_magic_bytes) {
                // in this case, we can use memcpy, which is faster
                for (unrollI=0; unrollI<local_unroll_count; unrollI++) {
                    memcpy(&runtime_code[rcI], code, code_length);
                    rcI += code_length;
                }
            } else {
                while (unrollI < local_unroll_count) {
                    while (codeI < code_length) {
                        if (codeI+7 < code_length && starts_with_magic_bytes(&code[codeI], MAGIC_BYTES_CODE_PFC_START)) {
                            codeI += 8;
                            templateI = magic_bytes_pfc_start_I;
                            goto continue_outer_loop;
                        }
                        if (codeI+7 < code_length && starts_with_magic_bytes(&code[codeI], MAGIC_BYTES_CODE_PFC_STOP)) {
                            codeI += 8;
                            goto continue_outer_loop;
                        }
                        runtime_code[rcI++] = code[codeI];
                        codeI++;
                    }
                    unrollI++;
                    codeI = 0;
                }
            }

            // TODO(wx): 在最后一次unroll后，dec R15, jnz loop_start，需要修改为loongarch对应的指令
            if (unrollI >= local_unroll_count) {
                if (local_loop_count > 0) {
                    strcpy(&runtime_code[rcI], "\x49\xFF\xCF"); rcI += 3; // dec R15
                    strcpy(&runtime_code[rcI], "\x0F\x85"); rcI += 2;
                    *(int32_t*)(&runtime_code[rcI]) = (int32_t)(rcI_code_start-rcI-4); rcI += 4; // jnz loop_start
                }

                if (drain_frontend) {
                    // We add an lfence followed by nop instructions s.t. the front end gets drained and the following instruction begins on a 32-byte boundary.
                    strcpy(&runtime_code[rcI], "\x0F\xAE\xE8"); rcI += 3; // lfence
                    for (int i=0; i<189; i++) {
                        strcpy(&runtime_code[rcI], NOPS[1]); rcI += 1;
                    }

                    for (int i=0; i<61; i++) {
                        strcpy(&runtime_code[rcI], NOPS[15]); rcI += 15;
                    }

                    int dist_to_32Byte_boundary = 32 - ((uintptr_t)&runtime_code[rcI] % 32);
                    if (dist_to_32Byte_boundary <= (3*15) - 32) {
                        dist_to_32Byte_boundary += 32;
                    }

                    int len_nop1 = min(15, dist_to_32Byte_boundary - 2);
                    int len_nop2 = min(15, dist_to_32Byte_boundary - len_nop1 - 1);
                    int len_nop3 = dist_to_32Byte_boundary - len_nop1 - len_nop2;

                    strcpy(&runtime_code[rcI], NOPS[len_nop1]); rcI += len_nop1;
                    strcpy(&runtime_code[rcI], NOPS[len_nop2]); rcI += len_nop2;
                    strcpy(&runtime_code[rcI], NOPS[len_nop3]); rcI += len_nop3;
                }

                if (debug) {
                    runtime_code[rcI++] = '\xCC'; // INT3
                }
            }
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_PFC_END)) {
            if (unrollI < local_unroll_count) {
                templateI = magic_bytes_code_I;
            } else {
                templateI += 8;
            }
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_PFC)) {
            *(void**)(&runtime_code[rcI]) = pfc_mem;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_MSR)) {
            *(void**)(&runtime_code[rcI]) = (void*)cur_rdmsr;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_RSP_ADDRESS)) {
            *(void**)(&runtime_code[rcI]) = &RSP_mem;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_RUNTIME_R14)) {
            *(void**)(&runtime_code[rcI]) = runtime_r14;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_RUNTIME_RBP)) {
            *(void**)(&runtime_code[rcI]) = runtime_rbp;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_RUNTIME_RDI)) {
            *(void**)(&runtime_code[rcI]) = runtime_rdi;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_RUNTIME_RSI)) {
            *(void**)(&runtime_code[rcI]) = runtime_rsi;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&measurement_template[templateI], MAGIC_BYTES_RUNTIME_RSP)) {
            *(void**)(&runtime_code[rcI]) = runtime_rsp;
            templateI += 8; rcI += 8;
        } else {
            runtime_code[rcI++] = measurement_template[templateI++];
        }
        continue_outer_loop: ;
    }
    templateI += 8;
    do {
        runtime_code[rcI++] = measurement_template[templateI++];
    } while (measurement_template[templateI-1] != '\xC3'); // 0xC3 = ret
}

void create_and_run_one_time_init_code() {
    if (code_one_time_init_length == 0) return;

    char* template = (char*)&one_time_init_template;
    size_t templateI = 0;
    size_t rcI = 0;

    while (!starts_with_magic_bytes(&template[templateI], MAGIC_BYTES_TEMPLATE_END)) {
        if (starts_with_magic_bytes(&template[templateI], MAGIC_BYTES_INIT)) {
            templateI += 8;
            memcpy(&runtime_one_time_init_code[rcI], code_one_time_init, code_one_time_init_length);
            rcI += code_one_time_init_length;
        } else if (starts_with_magic_bytes(&template[templateI], MAGIC_BYTES_RSP_ADDRESS)) {
            *(void**)(&runtime_one_time_init_code[rcI]) = &RSP_mem;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&template[templateI], MAGIC_BYTES_RUNTIME_R14)) {
            *(void**)(&runtime_one_time_init_code[rcI]) = runtime_r14;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&template[templateI], MAGIC_BYTES_RUNTIME_RBP)) {
            *(void**)(&runtime_one_time_init_code[rcI]) = runtime_rbp;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&template[templateI], MAGIC_BYTES_RUNTIME_RDI)) {
            *(void**)(&runtime_one_time_init_code[rcI]) = runtime_rdi;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&template[templateI], MAGIC_BYTES_RUNTIME_RSI)) {
            *(void**)(&runtime_one_time_init_code[rcI]) = runtime_rsi;
            templateI += 8; rcI += 8;
        } else if (starts_with_magic_bytes(&template[templateI], MAGIC_BYTES_RUNTIME_RSP)) {
            *(void**)(&runtime_one_time_init_code[rcI]) = runtime_rsp;
            templateI += 8; rcI += 8;
        } else {
            runtime_one_time_init_code[rcI++] = template[templateI++];
        }
    }
    templateI += 8;
    do {
        runtime_one_time_init_code[rcI++] = template[templateI++];
    } while (template[templateI-1] != '\xC3'); // 0xC3 = ret

    ((void(*)(void))runtime_one_time_init_code)();
}

void run_initial_warmup_experiment() {
    if (!initial_warm_up_count) return;

    create_runtime_code((char*)&initial_warm_up_template, unroll_count, loop_count);

    for (int i=0; i<initial_warm_up_count; i++) {
        ((void(*)(void))runtime_code)();
    }
}

void run_experiment(char* measurement_template, int64_t* results[], int n_counters, long local_unroll_count, long local_loop_count) {
    create_runtime_code(measurement_template, local_unroll_count, local_loop_count);

    for (long ri=-warm_up_count; ri<n_measurements; ri++) {
        ((void(*)(void))runtime_code)();

        for (int c=0; c<n_counters; c++) {
            results[c][max(0L, ri)] = pfc_mem[c];
        }
    }
}

char* compute_result_str(char* buf, size_t buf_len, char* desc, int counter) {
    int64_t agg = get_aggregate_value(measurement_results[counter], n_measurements, no_normalization?1:100);
    int64_t agg_base = get_aggregate_value(measurement_results_base[counter], n_measurements, no_normalization?1:100);

    if (no_normalization) {
        snprintf(buf, buf_len, "%s: %lld\n", desc, (long long)(agg-agg_base));
    } else {
        int64_t n_rep = loop_count * unroll_count;
        if (loop_count == 0) {
            n_rep = unroll_count;
        }
        int64_t result = ((agg-agg_base) + n_rep/2)/n_rep;
        snprintf(buf, buf_len, "%s: %s%lld.%.2lld\n", desc, (result<0?"-":""), ll_abs(result/100), ll_abs(result)%100);
    }
    return buf;
}

int64_t get_aggregate_value(int64_t* values, size_t length, size_t scale) {
    if (aggregate_function == MIN) {
        int64_t min = values[0];
        for (int i=0; i<length; i++) {
            if (values[i] < min) {
                min = values[i];
            }
        }
        return min * scale;
    } else if (aggregate_function == MAX)  {
        int64_t max = values[0];
        for (int i=0; i<length; i++) {
            if (values[i] > max) {
                max = values[i];
            }
        }
        return max * scale;
    } else {
        qsort(values, length, sizeof(int64_t), cmpInt64);

        if (aggregate_function == AVG_20_80) {
            // computes the average of the values between the 20 and 80 percentile
            int64_t sum = 0;
            int count = 0;
            for (int i=length/5; i<length-(length/5); i++, count++) {
                sum += (values[i] * scale);
            }
            return sum/count;
        } else {
            return values[length/2] * scale;
        }
    }
}

int cmpInt64(const void *a, const void *b) {
    return *(int64_t*)a - *(int64_t*)b;
}

long long ll_abs(long long val) {
    if (val < 0) {
        return -val;
    } else {
        return val;
    }
}

void print_all_measurement_results(int64_t* results[], int n_counters) {
    int run_padding = (n_measurements<=10?1:(n_measurements<=100?2:(n_measurements<=1000?3:4)));

    char buf[120];

    sprintf(buf, "\t%*s      ", run_padding, "");
    for (int c=0; c<n_counters; c++) {
        sprintf(buf + strlen(buf), "        Ctr%d", c);
    }
    print_verbose("%s\n", buf);

    for (int i=0; i<n_measurements; i++) {
        sprintf(buf, "\trun %*d: ", run_padding, i);
        for (int c=0; c<n_counters; c++) {
            sprintf(buf + strlen(buf), "%12lld", (long long)results[c][i]);
        }
        print_verbose("%s\n", buf);
    }
    print_verbose("\n");
}

bool starts_with_magic_bytes(char* c, int64_t magic_bytes) {
    return (*((int64_t*)c) == magic_bytes);
}

void measurement_template_Intel_2() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "push rax                                \n"
        "lahf                                    \n"
        "seto al                                 \n"
        "push rax                                \n"
        "push rcx                                \n"
        "push rdx                                \n"
        "push r15                                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov qword ptr [r15 + 0], 0              \n"
        "mov qword ptr [r15 + 8], 0              \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 0], rdx                      \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 8], rdx                      \n"
        "lfence                                  \n"
        "pop r15; lfence                         \n"
        "pop rdx; lfence                         \n"
        "pop rcx; lfence                         \n"
        "pop rax; lfence                         \n"
        "cmp al, -127; lfence                    \n"
        "sahf; lfence                            \n"
        "pop rax                                 \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 0], rdx                      \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 8], rdx                      \n"
        "lfence                                  \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_template_Intel_4() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "push rax                                \n"
        "lahf                                    \n"
        "seto al                                 \n"
        "push rax                                \n"
        "push rcx                                \n"
        "push rdx                                \n"
        "push r15                                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov qword ptr [r15 + 0], 0              \n"
        "mov qword ptr [r15 + 8], 0              \n"
        "mov qword ptr [r15 + 16], 0             \n"
        "mov qword ptr [r15 + 24], 0             \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 0], rdx                      \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 8], rdx                      \n"
        "mov rcx, 2                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 16], rdx                     \n"
        "mov rcx, 3                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 24], rdx                     \n"
        "lfence                                  \n"
        "pop r15; lfence                         \n"
        "pop rdx; lfence                         \n"
        "pop rcx; lfence                         \n"
        "pop rax; lfence                         \n"
        "cmp al, -127; lfence                    \n"
        "sahf; lfence                            \n"
        "pop rax                                 \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 0], rdx                      \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 8], rdx                      \n"
        "mov rcx, 2                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 16], rdx                     \n"
        "mov rcx, 3                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 24], rdx                     \n"
        "lfence                                  \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_template_Intel_noMem_2() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "mov r8, 0                               \n"
        "mov r9, 0                               \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_START)"\n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r8, rdx                             \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r9, rdx                             \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r8, rdx                             \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r9, rdx                             \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_END)"  \n"
        "mov rax, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov [rax + 0], r8                       \n"
        "mov [rax + 8], r9                       \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_template_Intel_noMem_4() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "mov r8, 0                               \n"
        "mov r9, 0                               \n"
        "mov r10, 0                              \n"
        "mov r11, 0                              \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_START)"\n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r8, rdx                             \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r9, rdx                             \n"
        "mov rcx, 2                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r10, rdx                            \n"
        "mov rcx, 3                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r11, rdx                            \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r8, rdx                             \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r9, rdx                             \n"
        "mov rcx, 2                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r10, rdx                            \n"
        "mov rcx, 3                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r11, rdx                            \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_END)"  \n"
        "mov rax, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov [rax + 0], r8                       \n"
        "mov [rax + 8], r9                       \n"
        "mov [rax + 16], r10                     \n"
        "mov [rax + 24], r11                     \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_template_AMD() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "push rax                                \n"
        "lahf                                    \n"
        "seto al                                 \n"
        "push rax                                \n"
        "push rcx                                \n"
        "push rdx                                \n"
        "push r15                                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov qword ptr [r15 + 0], 0              \n"
        "mov qword ptr [r15 + 8], 0              \n"
        "mov qword ptr [r15 + 16], 0             \n"
        "mov qword ptr [r15 + 24], 0             \n"
        "mov qword ptr [r15 + 32], 0             \n"
        "mov qword ptr [r15 + 40], 0             \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 0], rdx                      \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 8], rdx                      \n"
        "mov rcx, 2                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 16], rdx                     \n"
        "mov rcx, 3                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 24], rdx                     \n"
        "mov rcx, 4                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 32], rdx                     \n"
        "mov rcx, 5                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15 + 40], rdx                     \n"
        "lfence                                  \n"
        "pop r15; lfence                         \n"
        "pop rdx; lfence                         \n"
        "pop rcx; lfence                         \n"
        "pop rax; lfence                         \n"
        "cmp al, -127; lfence                    \n"
        "sahf; lfence                            \n"
        "pop rax                                 \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 0], rdx                      \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 8], rdx                      \n"
        "mov rcx, 2                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 16], rdx                     \n"
        "mov rcx, 3                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 24], rdx                     \n"
        "mov rcx, 4                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 32], rdx                     \n"
        "mov rcx, 5                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15 + 40], rdx                     \n"
        "lfence                                  \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_template_AMD_noMem() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "mov r8, 0                               \n"
        "mov r9, 0                               \n"
        "mov r10, 0                              \n"
        "mov r11, 0                              \n"
        "mov r12, 0                              \n"
        "mov r13, 0                              \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_START)"\n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r8, rdx                             \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r9, rdx                             \n"
        "mov rcx, 2                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r10, rdx                            \n"
        "mov rcx, 3                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r11, rdx                            \n"
        "mov rcx, 4                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r12, rdx                            \n"
        "mov rcx, 5                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r13, rdx                            \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, 0                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r8, rdx                             \n"
        "mov rcx, 1                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r9, rdx                             \n"
        "mov rcx, 2                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r10, rdx                            \n"
        "mov rcx, 3                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r11, rdx                            \n"
        "mov rcx, 4                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r12, rdx                            \n"
        "mov rcx, 5                              \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r13, rdx                            \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_END)"  \n"
        "mov rax, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov [rax + 0], r8                       \n"
        "mov [rax + 8], r9                       \n"
        "mov [rax + 16], r10                     \n"
        "mov [rax + 24], r11                     \n"
        "mov [rax + 32], r12                     \n"
        "mov [rax + 40], r13                     \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_FF_template_Intel() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "push rax                                \n"
        "lahf                                    \n"
        "seto al                                 \n"
        "push rax                                \n"
        "push rcx                                \n"
        "push rdx                                \n"
        "push r15                                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov qword ptr [r15], 0                  \n"
        "mov qword ptr [r15+8], 0                \n"
        "mov qword ptr [r15+16], 0               \n"
        "mov qword ptr [r15+24], 0               \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15], rdx                          \n"
        "mov rcx, 0x40000000                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15+8], rdx                        \n"
        "mov rcx, 0x40000002                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15+24], rdx                       \n"
        "mov rcx, 0x40000001                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15+16], rdx                       \n"
        "lfence                                  \n"
        "pop r15; lfence                         \n"
        "pop rdx; lfence                         \n"
        "pop rcx; lfence                         \n"
        "pop rax; lfence                         \n"
        "cmp al, -127; lfence                    \n"
        "sahf; lfence                            \n"
        "pop rax                                 \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, 0x40000001                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "add [r15+16], rdx                       \n"
        "mov rcx, 0x40000000                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15+8], rdx                        \n"
        "mov rcx, 0x40000002                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15+24], rdx                       \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15], rdx                          \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_FF_template_Intel_noMem() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "mov r8, 0                               \n"
        "mov r9, 0                               \n"
        "mov r10, 0                              \n"
        "mov r11, 0                              \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_START)"\n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r8, rdx                             \n"
        "mov rcx, 0x40000000                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r9, rdx                             \n"
        "mov rcx, 0x40000002                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r10, rdx                            \n"
        "mov rcx, 0x40000001                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r11, rdx                            \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, 0x40000001                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r11, rdx                            \n"
        "mov rcx, 0x40000000                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r9, rdx                             \n"
        "mov rcx, 0x40000002                     \n"
        "lfence; rdpmc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r10, rdx                            \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r8, rdx                             \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_END)"  \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov [r15], r8                           \n"
        "mov [r15+8], r9                         \n"
        "mov [r15+16], r11                       \n"
        "mov [r15+24], r10                       \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_FF_template_AMD() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "push rax                                \n"
        "lahf                                    \n"
        "seto al                                 \n"
        "push rax                                \n"
        "push rcx                                \n"
        "push rdx                                \n"
        "push r15                                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov qword ptr [r15], 0                  \n"
        "mov qword ptr [r15+8], 0                \n"
        "mov qword ptr [r15+16], 0               \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15], rdx                          \n"
        "mov rcx, 0x000000E7                     \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15+8], rdx                        \n"
        "mov rcx, 0x000000E8                     \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15+16], rdx                       \n"
        "lfence                                  \n"
        "pop r15; lfence                         \n"
        "pop rdx; lfence                         \n"
        "pop rcx; lfence                         \n"
        "pop rax; lfence                         \n"
        "cmp al, -127; lfence                    \n"
        "sahf; lfence                            \n"
        "pop rax                                 \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, 0x000000E8                     \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "add [r15+16], rdx                       \n"
        "lfence                                  \n"
        "mov rcx, 0x000000E7                     \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15+8], rdx                        \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add [r15], rdx                          \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_FF_template_AMD_noMem() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "mov r8, 0                               \n"
        "mov r9, 0                               \n"
        "mov r10, 0                              \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_START)"\n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r8, rdx                             \n"
        "mov rcx, 0x000000E7                     \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r9, rdx                             \n"
        "mov rcx, 0x000000E8                     \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r10, rdx                            \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, 0x000000E8                     \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r10, rdx                            \n"
        "mov rcx, 0x000000E7                     \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r9, rdx                             \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r8, rdx                             \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_END)"  \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov [r15], r8                           \n"
        "mov [r15+8], r9                         \n"
        "mov [r15+16], r10                       \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_RDTSC_template() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "push rax                                \n"
        "lahf                                    \n"
        "seto al                                 \n"
        "push rax                                \n"
        "push rdx                                \n"
        "push r15                                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov qword ptr [r15], 0                  \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15], rdx                          \n"
        "lfence                                  \n"
        "pop r15; lfence                         \n"
        "pop rdx; lfence                         \n"
        "pop rax; lfence                         \n"
        "cmp al, -127; lfence                    \n"
        "sahf; lfence                            \n"
        "pop rax                                 \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "add [r15], rdx                          \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_RDTSC_template_noMem() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "mov r8, 0                               \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_START)"\n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r8, rdx                             \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence; rdtsc; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r8, rdx                             \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_END)"  \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov [r15], r8                           \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_RDMSR_template() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "push rax                                \n"
        "lahf                                    \n"
        "seto al                                 \n"
        "push rax                                \n"
        "push rcx                                \n"
        "push rdx                                \n"
        "push r15                                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov qword ptr [r15], 0                  \n"
        "mov rcx, "STRINGIFY(MAGIC_BYTES_MSR)"   \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub [r15], rdx                          \n"
        "lfence                                  \n"
        "pop r15; lfence                         \n"
        "pop rdx; lfence                         \n"
        "pop rcx; lfence                         \n"
        "pop rax; lfence                         \n"
        "cmp al, -127; lfence                    \n"
        "sahf; lfence                            \n"
        "pop rax                                 \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, "STRINGIFY(MAGIC_BYTES_MSR)"   \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "add [r15], rdx                          \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_RDMSR_template_noMem() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        "mov r8, 0                               \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_START)"\n"
        "mov rcx, "STRINGIFY(MAGIC_BYTES_MSR)"   \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "sub r8, rdx                             \n"
        "lfence                                  \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        "lfence                                  \n"
        "mov rcx, "STRINGIFY(MAGIC_BYTES_MSR)"   \n"
        "lfence; rdmsr; lfence                   \n"
        "shl rdx, 32; or rdx, rax                \n"
        "add r8, rdx                             \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_END)"  \n"
        "mov r15, "STRINGIFY(MAGIC_BYTES_PFC)"   \n"
        "mov [r15], r8                           \n"
        ".att_syntax noprefix                    ");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_cycleByCycle_template_Intel() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                 \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT) "   \n"
        "push rax                               \n"
        "push rcx                               \n"
        "push rdx                               \n"
        "mov rcx, 0x38F                         \n"
        "mov rax, 0xF                           \n"
        "mov rdx, 0x7                           \n"
        "wrmsr                                  \n"
        "pop rdx                                \n"
        "pop rcx                                \n"
        "pop rax                                \n"
        "lfence                                 \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE) "   \n"
        "lfence                                 \n"
        "mov rcx, 0x38F                         \n"
        "mov rax, 0x0                           \n"
        "mov rdx, 0x0                           \n"
        "wrmsr                                  \n"
        ".att_syntax prefix                     \n");
    RESTORE_REGS_FLAGS();
    asm(".quad " STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void measurement_cycleByCycle_template_Intel_noMem() {
    SAVE_REGS_FLAGS();
    asm(".intel_syntax noprefix                 \n"
        ".quad "STRINGIFY(MAGIC_BYTES_INIT) "   \n"
        "mov rcx, 0x38F                         \n"
        "mov rax, 0xF                           \n"
        "mov rdx, 0x7                           \n"
        "wrmsr                                  \n"
        "lfence                                 \n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE) "   \n"
        "lfence                                 \n"
        "mov rcx, 0x38F                         \n"
        "mov rax, 0x0                           \n"
        "mov rdx, 0x0                           \n"
        "wrmsr                                  \n"
        ".att_syntax prefix                     \n");
    RESTORE_REGS_FLAGS();
    asm(".quad " STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void one_time_init_template() {
    SAVE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_INIT));
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}

void initial_warm_up_template() {
    SAVE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_INIT)"     \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_START)"\n"
        ".quad "STRINGIFY(MAGIC_BYTES_CODE)"     \n"
        ".quad "STRINGIFY(MAGIC_BYTES_PFC_END)"  \n");
    RESTORE_REGS_FLAGS();
    asm(".quad "STRINGIFY(MAGIC_BYTES_TEMPLATE_END));
}
