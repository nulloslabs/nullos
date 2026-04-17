#pragma once

#include <limine/limine.h>

extern volatile struct limine_framebuffer_request fb_req;
extern volatile struct limine_memmap_request mm_req;
extern volatile struct limine_hhdm_request hhdm_req;
extern volatile struct limine_module_request mod_req;
extern volatile struct limine_executable_file_request cmdline_req;
extern volatile struct limine_executable_address_request eaddr_req;
extern volatile struct limine_rsdp_request rsdp_req;
extern volatile struct limine_mp_request mp_req;
extern volatile struct limine_smbios_request smbios_req;