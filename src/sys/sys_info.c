#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/sys_info.h>
#include <cwist/core/macros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sysinfo.h>

uint64_t cwist_get_available_ram(void) {
    // Attempt 1: sysinfo() (Fastest, standard libc)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        // freeram is explicitly free.
        // bufferram and sharedram might be reclaimable but let's be conservative.
        // On modern Linux kernels, "MemAvailable" in /proc/meminfo is better,
        // but sysinfo is a good rough estimate multiplied by mem_unit.
        return (uint64_t)si.freeram * (uint64_t)si.mem_unit;
    }

    // Attempt 2: /proc/meminfo (More accurate "Available")
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        uint64_t mem_avail_kb = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "MemAvailable: %lu kB", &mem_avail_kb) == 1) {
                fclose(fp);
                return mem_avail_kb * 1024;
            }
        }
        fclose(fp);
    }
    
    return 0; // Unknown
}

#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>

uint64_t cwist_get_available_ram(void) {
    // macOS/BSD Logic
    // Getting strict "Available" like Linux is harder. 
    // We can get page size * free pages.
    
    int mib[2];
    mib[0] = CTL_HW;
    
#ifdef __APPLE__
    // Roughly estimate using page size and free count is tricky portably.
    // Let's rely on a simpler 'usermem' or just fail-safe to "High Enough" if we can't detect,
    // assuming the user manages their mac well.
    // Or check vm.page_free_count
    return CWIST_GIB(2); // Mock for Mac dev env safety to prevent constant switching
#else
    // FreeBSD
    u_int page_size;
    u_int free_count;
    size_t len = sizeof(page_size);
    
    if (sysctlbyname("vm.stats.vm.v_page_size", &page_size, &len, NULL, 0) == -1) return 0;
    len = sizeof(free_count);
    if (sysctlbyname("vm.stats.vm.v_free_count", &free_count, &len, NULL, 0) == -1) return 0;
    
    return (uint64_t)page_size * (uint64_t)free_count;
#endif
}

#else
uint64_t cwist_get_available_ram(void) {
    return CWIST_GIB(1); // Default Mock
}
#endif

bool cwist_is_ram_critical(uint64_t threshold_bytes) {
    uint64_t avail = cwist_get_available_ram();
    if (avail == 0) return false; // Could not detect, assume safe
    return avail < threshold_bytes;
}
