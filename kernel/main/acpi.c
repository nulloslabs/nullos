#include <freestanding/stddef.h>
#include <main/limine_req.h>
#include <io/io.h>
#include <main/string.h>
#include <main/acpi.h>
#include <main/madt.h>
#include <io/hpet.h>
#include <io/terminal.h>
#include <main/halt.h>
#include <main/spinlock.h>
#include <mm/mm.h>

static struct fadt_descriptor* fadt = NULL;
static uint16_t slp_typa = 0xFFFF;
static uint16_t slp_typb = 0xFFFF;
static struct acpi_header* acpi_root = NULL;

static uint32_t read_acpi(struct acpi_gas *gas) {
    if (!gas || !gas->address) return 0;
    if (gas->address_space_id == 0) {
        uintptr_t addr = gas->address + hhdm_offset;
        if (gas->register_bit_width == 8)  return *(volatile uint8_t*)addr;
        if (gas->register_bit_width == 16) return *(volatile uint16_t*)addr;
        return *(volatile uint32_t*)addr;
    } else {
        if (gas->register_bit_width == 8)  return inb(gas->address);
        if (gas->register_bit_width == 16) return inw(gas->address);
        return inl(gas->address);
    }
}

static void write_acpi(struct acpi_gas *gas, uint32_t val) {
    if (!gas || !gas->address) return;
    if (gas->address_space_id == 0) {
        uintptr_t addr = gas->address + hhdm_offset;
        if (gas->register_bit_width == 8)       *(volatile uint8_t*)addr  = (uint8_t)val;
        else if (gas->register_bit_width == 16) *(volatile uint16_t*)addr = (uint16_t)val;
        else *(volatile uint32_t*)addr = val;
    } else {
        if (gas->register_bit_width == 8)       outb(gas->address, (uint8_t)val);
        else if (gas->register_bit_width == 16) outw(gas->address, (uint16_t)val);
        else outl(gas->address, val);
    }
}
static aml_obj_t ns[AML_NS_MAX];
static int ns_n = 0;

// ACPI device registry
static acpi_device_registry_t acpi_devices = {0};
static int acpi_dev_count = 0;

static spinlock_t acpi_lock = SPINLOCK_INIT;

// ---- name/path helpers ----

static void nameseg(uint8_t **p, char out[5]) {
    memcpy(out, *p, 4); out[4] = 0; *p += 4;
}

// Parse an AML NameString into a relative path string (no allocation)
static void parse_path(uint8_t **p, char *out) {
    char *o = out;
    if (**p == 0x5C)      { *o++ = '\\'; (*p)++; }
    else while (**p == 0x5E) { *o++ = '^'; (*p)++; }
    if (**p == 0x00)      { (*p)++; }
    else if (**p == 0x2E) { // DualNamePath
        (*p)++;
        char s[5]; nameseg(p, s); memcpy(o, s, 4); o+=4; *o++='.';
        nameseg(p, s); memcpy(o, s, 4); o+=4;
    } else if (**p == 0x2F) { // MultiNamePath
        (*p)++;
        int cnt = *(*p)++;
        for (int i=0; i<cnt; i++) {
            if (i) *o++='.';
            char s[5]; nameseg(p, s); memcpy(o, s, 4); o+=4;
        }
    } else {
        char s[5]; nameseg(p, s); memcpy(o, s, 4); o+=4;
    }
    *o = 0;
}

// Resolve a relative name against a scope into an absolute path
static void abs_path(const char *scope, const char *name, char *out) {
    if (!name || !name[0]) { strncpy(out, scope, AML_NAME_MAX-1); return; }
    if (name[0] == '\\')   { strncpy(out, name, AML_NAME_MAX-1); return; }
    char sc[AML_NAME_MAX]; strncpy(sc, scope, AML_NAME_MAX-1);
    const char *n = name;
    while (*n == '^') {
        char *dot = strrchr(sc, '.'); if (dot) *dot=0; else { sc[0]='\\'; sc[1]=0; }
        n++;
    }
    if (*n) {
        size_t sl = strlen(sc);
        if (sc[1]) { sc[sl]='.'; sc[sl+1]=0; }
        strncat(sc, n, AML_NAME_MAX-1-strlen(sc));
    }
    strncpy(out, sc, AML_NAME_MAX-1); out[AML_NAME_MAX-1]=0;
}

// ---- namespace lookup ----

static aml_obj_t *ns_exact(const char *path) {
    for (int i=0; i<ns_n; i++)
        if (strcmp(ns[i].path, path)==0) return &ns[i];
    return NULL;
}

static aml_obj_t *ns_find(const char *scope, const char *relname) {
    if (!relname[0]) return NULL;
    if (relname[0]=='\\') return ns_exact(relname);
    char sc[AML_NAME_MAX]; strncpy(sc, scope, AML_NAME_MAX-1);
    char full[AML_NAME_MAX];
    for (;;) {
        abs_path(sc, relname, full);
        aml_obj_t *o = ns_exact(full); if(o) return o;
        char *dot = strrchr(sc, '.'); if(!dot) break; *dot=0;
    }
    abs_path("\\", relname, full);
    return ns_exact(full);
}

// ---- ACPI Device Enumeration helpers ----

// Helper: string formatting 
static void fmt_path(char *buf, int max, const char *prefix, const char *suffix) {
    strncpy(buf, prefix, max - 1);
    buf[max - 1] = 0;
    size_t pl = strlen(buf);
    size_t sl = strlen(suffix);
    for (size_t i = 0; i < sl && pl + i < (size_t)(max - 1); i++)
        buf[pl + i] = suffix[i];
    buf[pl + sl] = 0;
}

// Add a device to the registry, or return existing 
static acpi_device_t* dev_registry_add(const char *path) {
    for (int i = 0; i < acpi_dev_count; i++) {
        if (strcmp(acpi_devices.devices[i].path, path) == 0)
            return &acpi_devices.devices[i];
    }
    if (acpi_dev_count >= ACPI_MAX_DEVICES) return NULL;
    acpi_device_t *dev = &acpi_devices.devices[acpi_dev_count++];
    memset(dev, 0, sizeof(acpi_device_t));
    strncpy(dev->path, path, AML_NAME_MAX - 1);
    return dev;
}

static void dev_eval_hid(const char *scope) {
    char path[AML_NAME_MAX];
    fmt_path(path, AML_NAME_MAX, scope, "._HID");
    aml_obj_t *obj = ns_exact(path);
    if (!obj || obj->type != AML_INT) return;
    acpi_device_t *dev = dev_registry_add(scope);
    if (!dev) return;
    dev->has_hid = 1;
    dev->hid = obj->ival;
    uint32_t v = (uint32_t)obj->ival;
    if (v) {
        char c3 = (v >> 24) & 0xFF;
        char c2 = (v >> 16) & 0xFF;
        char c1 = (v >>  8) & 0xFF;
        char c0 = (v >>  0) & 0xFF;
        int valid = 1;
        char buf[5] = {c0, c1, c2, c3, 0};
        for (int i = 0; i < 4; i++) {
            char ch = buf[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) {
                valid = 0; break;
            }
        }
        if (valid) strncpy(dev->hid_str, buf, AML_NAME_MAX - 1);
    }
}

static void dev_eval_adr(const char *scope) {
    char path[AML_NAME_MAX];
    fmt_path(path, AML_NAME_MAX, scope, "._ADR");
    aml_obj_t *obj = ns_exact(path);
    if (!obj || obj->type != AML_INT) return;
    acpi_device_t *dev = dev_registry_add(scope);
    if (!dev) return;
    dev->has_adr = 1;
    dev->adr = obj->ival;
}

static void dev_eval_uid(const char *scope) {
    char path[AML_NAME_MAX];
    fmt_path(path, AML_NAME_MAX, scope, "._UID");
    aml_obj_t *obj = ns_exact(path);
    if (!obj || obj->type != AML_INT) return;
    acpi_device_t *dev = dev_registry_add(scope);
    if (!dev) return;
    dev->uid = obj->ival;
}

static void dev_check_crs(const char *scope) {
    char path[AML_NAME_MAX];
    fmt_path(path, AML_NAME_MAX, scope, "._CRS");
    aml_obj_t *obj = ns_exact(path);
    if (!obj) return;
    if (obj->type != AML_METHOD && obj->type != AML_BUFFER && obj->type != AML_INT) return;
    acpi_device_t *dev = dev_registry_add(scope);
    if (!dev) return;
    dev->has_crs = 1;
}

// ---- PkgLength ----

// Returns total package length (including the pkglen bytes themselves).
// *p is advanced past the PkgLength bytes.
static uint32_t pkglen(uint8_t **p) {
    uint8_t lead = *(*p)++;
    int follow = (lead>>6)&3;
    uint32_t len = lead&0x3F;
    for (int i=0; i<follow; i++) len |= (uint32_t)(*(*p)++) << (4+i*8);
    return len;
}

// ---- integer term detection ----

static int is_int_op(uint8_t op) {
    return op==0x00||op==0x01||op==0xFF||op==0x0A||op==0x0B||op==0x0C||op==0x0E;
}

static uint64_t parse_int(uint8_t **p) {
    uint8_t op=*(*p)++;
    switch(op) {
        case 0x00: return 0;
        case 0x01: return 1;
        case 0xFF: return ~0ULL;
        case 0x0A: return *(*p)++;
        case 0x0B: { uint16_t v=*(uint16_t*)*p; *p+=2; return v; }
        case 0x0C: { uint32_t v=*(uint32_t*)*p; *p+=4; return v; }
        case 0x0E: { uint64_t v=*(uint64_t*)*p; *p+=8; return v; }
    }
    return 0;
}

// ---- DSDT namespace scanner ----

static void aml_scan(uint8_t *start, uint8_t *end, const char *scope) {
    uint8_t *p = start;
    while (p < end) {
        uint8_t op = *p;

        if (op == 0x10) { // ScopeOp
            p++;
            uint8_t *ps=p; uint32_t len=pkglen(&p); uint8_t *pe=ps+len;
            if (pe>end||pe<p) break;
            char rn[AML_NAME_MAX]; parse_path(&p, rn);
            char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);
            aml_scan(p, pe, fn); p=pe;
        }
        else if (op == 0x14) { // MethodOp
            p++;
            uint8_t *ps=p; uint32_t len=pkglen(&p); uint8_t *pe=ps+len;
            if (pe>end||pe<p) break;
            char rn[AML_NAME_MAX]; parse_path(&p, rn);
            uint8_t flags = *p++;
            char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);
            if (ns_n < AML_NS_MAX) {
                aml_obj_t *obj=&ns[ns_n++];
                strncpy(obj->path, fn, AML_NAME_MAX-1);
                obj->type=AML_METHOD; obj->method.body=p;
                obj->method.blen=(uint32_t)(pe-p); obj->method.argc=flags&7;
            }
            p=pe;
        }
        else if (op == 0x08) { // NameOp
            p++;
            char rn[AML_NAME_MAX]; parse_path(&p, rn);
            char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);
            if (p<end && is_int_op(*p)) {
                uint64_t val=parse_int(&p);
                aml_obj_t *ex=ns_exact(fn);
                if (ex && ex->type==AML_INT) { ex->ival=val; }
                else if (!ex && ns_n<AML_NS_MAX) {
                    aml_obj_t *obj=&ns[ns_n++];
                    strncpy(obj->path, fn, AML_NAME_MAX-1);
                    obj->type=AML_INT; obj->ival=val;
                }
            } else if (p<end) {
                // Check for Buffer data - store it in namespace as AML_BUFFER
                uint8_t dop=*p++;
                if (dop==0x11) { // Buffer opcode
                    uint8_t *ps=p; uint32_t l=pkglen(&p);
                    uint8_t *pe_buf=ps+l;
                    if (pe_buf>end) pe_buf=end;
                    // Skip the buffer size operand
                    if (p<pe_buf && is_int_op(*p)) { parse_int(&p); }
                    // p now points to the raw buffer data
                    uint32_t buf_data_len = (uint32_t)(pe_buf - p);
                    if (buf_data_len > 0 && ns_n<AML_NS_MAX) {
                        aml_obj_t *obj=&ns[ns_n++];
                        strncpy(obj->path, fn, AML_NAME_MAX-1);
                        obj->type=AML_BUFFER;
                        obj->buffer.data=p;
                        obj->buffer.len=buf_data_len;
                    }
                    p=pe_buf;
                } else if (dop==0x12||dop==0x13) { // Package or VarPackage
                    uint8_t *ps=p; uint32_t l=pkglen(&p); p=ps+l;
                    if (p>end) p=end;
                }
            }
        }
        else if (op == 0x5B && p+1<end) {
            p++;
            uint8_t ext=*p++;
            if (ext == 0x80) { // OpRegionOp
                char rn[AML_NAME_MAX]; parse_path(&p, rn);
                char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);
                uint8_t space=*p++;
                int dyn=0; uint64_t base=0; char base_fld[AML_NAME_MAX]="";
                if (p<end && is_int_op(*p)) { base=parse_int(&p); }
                else if (p<end) {
                    dyn=1;
                    char bn[AML_NAME_MAX]; parse_path(&p, bn);
                    abs_path(scope, bn, base_fld);
                }
                uint32_t rlen=0;
                if (p<end && is_int_op(*p)) rlen=(uint32_t)parse_int(&p);
                else if (p<end) { char tmp[AML_NAME_MAX]; parse_path(&p, tmp); }
                if (ns_n<AML_NS_MAX) {
                    aml_obj_t *obj=&ns[ns_n++];
                    strncpy(obj->path, fn, AML_NAME_MAX-1);
                    obj->type=AML_REGION; obj->region.space=space;
                    obj->region.dyn=dyn; obj->region.base=base;
                    strncpy(obj->region.base_fld, base_fld, AML_NAME_MAX-1);
                    obj->region.len=rlen;
                }
            }
            else if (ext == 0x81) { // FieldOp
                uint8_t *ps=p; uint32_t len=pkglen(&p); uint8_t *pe=ps+len;
                if (pe>end||pe<p) { p=end; break; }
                char rn[AML_NAME_MAX]; parse_path(&p, rn);
                char rgn[AML_NAME_MAX]; abs_path(scope, rn, rgn);
                p++; // flags
                uint32_t bit_off=0;
                while (p<pe) {
                    if      (*p==0x00) { p++; uint8_t*s=p; bit_off+=pkglen(&p); (void)s; }
                    else if (*p==0x01) { p+=3; }
                    else if (*p==0x02) { p++; }
                    else if (*p==0x03) { p+=4; }
                    else {
                        // NamedField: 4 bytes name + PkgLength width
                        char fn[5]; memcpy(fn, p, 4); fn[4]=0; p+=4;
                        uint8_t *bws=p; uint32_t bw=pkglen(&p); (void)bws;
                        char full[AML_NAME_MAX]; abs_path(scope, fn, full);
                        if (ns_n<AML_NS_MAX) {
                            aml_obj_t *obj=&ns[ns_n++];
                            strncpy(obj->path, full, AML_NAME_MAX-1);
                            obj->type=AML_FIELD;
                            strncpy(obj->field.rgn, rgn, AML_NAME_MAX-1);
                            obj->field.bit_off=bit_off; obj->field.bit_wid=bw;
                        }
                        bit_off+=bw;
                    }
                }
                p=pe;
            }
            else if (ext==0x82||ext==0x85) { // DeviceOp / ThermalZoneOp
                uint8_t *ps=p; uint32_t len=pkglen(&p); uint8_t *pe=ps+len;
                if (pe>end||pe<p) { p=end; break; }
                char rn[AML_NAME_MAX]; parse_path(&p, rn);
                char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);
                aml_scan(p, pe, fn); p=pe;
            }
            else if (ext==0x83) { // ProcessorOp
                uint8_t *ps=p; uint32_t len=pkglen(&p); uint8_t *pe=ps+len;
                if (pe>end||pe<p) { p=end; break; }
                char rn[AML_NAME_MAX]; parse_path(&p, rn);
                p+=5; // ProcessorID, PBLKAddr, PBLKLen
                char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);
                aml_scan(p, pe, fn); p=pe;
            }
            else if (ext==0x84) { // PowerResOp
                uint8_t *ps=p; uint32_t len=pkglen(&p); uint8_t *pe=ps+len;
                if (pe>end||pe<p) { p=end; break; }
                char rn[AML_NAME_MAX]; parse_path(&p, rn);
                p+=3;
                char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);
                aml_scan(p, pe, fn); p=pe;
            }
            else if (ext==0x86||ext==0x87||ext==0x88) { // IndexField/BankField/DataRegion - skip
                uint8_t *ps=p; uint32_t len=pkglen(&p); p=ps+len;
                if (p>end) p=end;
            }
            else { p=end; break; } // unknown extended op - bail this scope
        }
        else if (op==0xA0||op==0xA1||op==0xA2) { // If/Else/While - scan bodies for declarations
            p++;
            uint8_t *ps=p; uint32_t len=pkglen(&p); uint8_t *pe=ps+len;
            if (pe>end||pe<p) { p=end; break; }
            aml_scan(p, pe, scope); p=pe;
        }
        else {
            p++; // unknown opcode - advance 1 byte
        }
    }
}

// ============================================================================
// AML field I/O
// ============================================================================

// Resolve a dynamic region base field. Falls back to last-segment search
// if the exact path fails (scope mismatch between scanner and runtime).
static aml_obj_t *dyn_base_lookup(const char *base_fld) {
    if (!base_fld[0]) return NULL;
    aml_obj_t *bf = ns_exact(base_fld);
    if (bf) return bf;
    const char *seg = strrchr(base_fld, '.');
    if (!seg) seg = strrchr(base_fld, '\\');
    if (seg) seg++; else seg = base_fld;
    if (!seg[0]) return NULL;
    return ns_find("\\", seg);
}

static uint64_t fld_read(aml_obj_t *fld) {
    if (!fld||fld->type!=AML_FIELD) return 0;
    aml_obj_t *rgn=ns_exact(fld->field.rgn);
    if (!rgn||rgn->type!=AML_REGION) return 0;
    uint64_t base=rgn->region.base;
    if (rgn->region.dyn) {
        aml_obj_t *bf=dyn_base_lookup(rgn->region.base_fld);
        if (bf) base=fld_read(bf);
    }
    uint32_t boff=fld->field.bit_off/8;
    uint32_t bsh =fld->field.bit_off%8;
    uint32_t bwid=fld->field.bit_wid;
    uint64_t mask=(bwid>=64)?~0ULL:((1ULL<<bwid)-1);
    uint32_t bytes=(bsh+bwid+7)/8; if(bytes>8) bytes=8;
    uint64_t raw=0;
    if (rgn->region.space==1) { // IO - byte-by-byte
        for (uint32_t i=0; i<bytes; i++)
            raw|=(uint64_t)inb((uint16_t)(base+boff+i))<<(i*8);
    } else { // Memory
        if (!base||base==~0ULL) return 0;
        uintptr_t addr=(uintptr_t)(base+boff)+hhdm_offset;
        for (uint32_t i=0; i<bytes; i++)
            raw|=(uint64_t)*(volatile uint8_t*)(addr+i)<<(i*8);
    }
    return (raw>>bsh)&mask;
}

static void fld_write(aml_obj_t *fld, uint64_t val) {
    if (!fld||fld->type!=AML_FIELD) return;
    aml_obj_t *rgn=ns_exact(fld->field.rgn);
    if (!rgn||rgn->type!=AML_REGION) return;
    uint64_t base=rgn->region.base;
    if (rgn->region.dyn) {
        aml_obj_t *bf=dyn_base_lookup(rgn->region.base_fld);
        if (bf) base=fld_read(bf);
    }
    if (!base||base==~0ULL) return;
    uint32_t boff=fld->field.bit_off/8;
    uint32_t bsh =fld->field.bit_off%8;
    uint32_t bwid=fld->field.bit_wid;
    uint64_t mask=(bwid>=64)?~0ULL:((1ULL<<bwid)-1);
    uint32_t bytes=(bsh+bwid+7)/8; if(bytes>8) bytes=8;
    if (rgn->region.space==1) { // IO
        uint64_t ex=0;
        for (uint32_t i=0; i<bytes; i++) ex|=(uint64_t)inb((uint16_t)(base+boff+i))<<(i*8);
        ex=(ex&~(mask<<bsh))|((val&mask)<<bsh);
        for (uint32_t i=0; i<bytes; i++) outb((uint16_t)(base+boff+i),(uint8_t)(ex>>(i*8)));
    } else { // Memory
        uintptr_t addr=(uintptr_t)(base+boff)+hhdm_offset;
        uint64_t ex=0;
        for (uint32_t i=0; i<bytes; i++) ex|=(uint64_t)*(volatile uint8_t*)(addr+i)<<(i*8);
        ex=(ex&~(mask<<bsh))|((val&mask)<<bsh);
        for (uint32_t i=0; i<bytes; i++) *(volatile uint8_t*)(addr+i)=(uint8_t)(ex>>(i*8));
    }
}

static int is_name_start(uint8_t b) {
    return (b>='A'&&b<='Z')||b=='_'||b=='\\'||b=='^'||b==0x2E||b==0x2F;
}

static aml_val_t eval(uint8_t **pp, uint8_t *end, aml_ctx_t *ctx);
static aml_val_t exec_body(uint8_t *body, uint32_t blen, aml_ctx_t *ctx);

// Write to a Store destination (name, local, arg, or discard)
static void store_to(uint8_t **pp, uint8_t *end, aml_ctx_t *ctx, uint64_t val) {
    if (*pp>=end) return;
    uint8_t dst=**pp;
    if (dst==0x5B && *pp+1<end && *(*pp+1)==0x31) { *pp+=2; return; } // DebugOp - discard
    if (dst>=0x60&&dst<=0x67) { (*pp)++; ctx->locals[dst-0x60]=val; return; }
    if (dst>=0x68&&dst<=0x6E) { (*pp)++; ctx->args[dst-0x68]=val; return; }
    if (dst==0x88) { // IndexOp as dst - evaluate and discard
        (*pp)++; eval(pp,end,ctx); eval(pp,end,ctx);
        uint8_t ir=(*pp<end)?**pp:0;
        if (ir==0x00) (*pp)++;
        else if (is_name_start(ir)) { char n[AML_NAME_MAX]; parse_path(pp,n); }
        return;
    }
    if (is_name_start(dst)) {
        char name[AML_NAME_MAX]; parse_path(pp,name);
        aml_obj_t *obj=ns_find(ctx->scope,name);
        if (obj) {
            if (obj->type==AML_FIELD) fld_write(obj,val);
            else if (obj->type==AML_INT) obj->ival=val;
        }
        return;
    }
    (*pp)++; // unknown dst - skip
}

static aml_val_t eval(uint8_t **pp, uint8_t *end, aml_ctx_t *ctx) {
    if (*pp>=end) return VOID;
    uint8_t op=**pp;

    // Integer literals
    if (is_int_op(op)) return V(parse_int(pp));

    // Locals / Args
    if (op>=0x60&&op<=0x67) { (*pp)++; return V(ctx->locals[op-0x60]); }
    if (op>=0x68&&op<=0x6E) { (*pp)++; return V(ctx->args[op-0x68]); }

    // NoOp
    if (op==0xA3) { (*pp)++; return VOID; }

    // ReturnOp
    if (op==0xA4) { (*pp)++; aml_val_t v=eval(pp,end,ctx); return RET(v.v); }

    // StoreOp: Store(src, dst)
    if (op==0x70) {
        (*pp)++;
        aml_val_t src=eval(pp,end,ctx);
        store_to(pp,end,ctx,src.v);
        return V(src.v);
    }

    // CopyObjectOp: same semantics as Store for our purposes
    if (op==0x9D) {
        (*pp)++;
        aml_val_t src=eval(pp,end,ctx);
        store_to(pp,end,ctx,src.v);
        return V(src.v);
    }

    // IfOp
    if (op==0xA0) {
        (*pp)++;
        uint8_t *ps=*pp; uint32_t len=pkglen(pp); uint8_t *pe=ps+len;
        if (pe>end) pe=end;
        aml_val_t cond=eval(pp,pe,ctx);
        aml_val_t result=VOID;
        if (cond.v) {
            while (*pp<pe) { result=eval(pp,pe,ctx); if(result.t==RET_RET){*pp=pe;break;} }
        }
        *pp=pe;
        // Check for trailing ElseOp
        if (*pp<end && **pp==0xA1) {
            (*pp)++;
            uint8_t *es=*pp; uint32_t el=pkglen(pp); uint8_t *ee=es+el;
            if (ee>end) ee=end;
            if (!cond.v) {
                while (*pp<ee) { result=eval(pp,ee,ctx); if(result.t==RET_RET){*pp=ee;break;} }
            }
            *pp=ee;
        }
        return result;
    }

    // ElseOp (standalone, skip)
    if (op==0xA1) {
        (*pp)++;
        uint8_t *ps=*pp; uint32_t len=pkglen(pp); *pp=ps+len;
        if (*pp>end) *pp=end;
        return VOID;
    }

    // WhileOp - skip (not needed for _PTS)
    if (op==0xA2) {
        (*pp)++;
        uint8_t *ps=*pp; uint32_t len=pkglen(pp); *pp=ps+len;
        if (*pp>end) *pp=end;
        return VOID;
    }

    // AddOp: Add(a, b, dst)
    if (op==0x72) {
        (*pp)++;
        aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx);
        uint64_t res=a.v+b.v;
        store_to(pp,end,ctx,res);
        return V(res);
    }

    // SubtractOp
    if (op==0x74) {
        (*pp)++;
        aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx);
        uint64_t res=a.v-b.v; store_to(pp,end,ctx,res); return V(res);
    }

    // AndOp
    if (op==0x7B) {
        (*pp)++;
        aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx);
        uint64_t res=a.v&b.v; store_to(pp,end,ctx,res); return V(res);
    }

    // OrOp
    if (op==0x7D) {
        (*pp)++;
        aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx);
        uint64_t res=a.v|b.v; store_to(pp,end,ctx,res); return V(res);
    }

    // LNotOp: returns 0xFFFFFFFF if operand is 0, else 0
    if (op==0x92) { (*pp)++; aml_val_t v=eval(pp,end,ctx); return V(v.v?0:0xFFFFFFFFu); }

    // LAndOp, LOrOp
    if (op==0x90) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return V((a.v&&b.v)?0xFFFFFFFFu:0); }
    if (op==0x91) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return V((a.v||b.v)?0xFFFFFFFFu:0); }

    // Comparison ops
    if (op==0x93) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return V(a.v==b.v?0xFFFFFFFFu:0); }
    if (op==0x94) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return V(a.v>b.v?0xFFFFFFFFu:0); }
    if (op==0x95) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return V(a.v<b.v?0xFFFFFFFFu:0); }

    // NotifyOp: 2 args, no result - skip (not needed for shutdown)
    if (op==0x86) { (*pp)++; eval(pp,end,ctx); eval(pp,end,ctx); return VOID; }

    // SizeOfOp
    if (op==0x87) { (*pp)++; eval(pp,end,ctx); return V(0); }

    // IndexOp: Index(src, idx, dst)
    if (op==0x88) {
        (*pp)++;
        eval(pp,end,ctx); eval(pp,end,ctx); // src, idx
        uint8_t dr=(*pp<end)?**pp:0;
        if (dr==0x00) (*pp)++;
        else if (is_name_start(dr)) { char n[AML_NAME_MAX]; parse_path(pp,n); }
        return VOID;
    }

    // Extended opcodes
    if (op==0x5B && *pp+1<end) {
        (*pp)++;
        uint8_t ext=*(*pp)++;
        if (ext==0x22||ext==0x21) { eval(pp,end,ctx); return VOID; }  // Sleep/Stall
        if (ext==0x31)            { return VOID; }                      // DebugOp (lvalue)
        if (ext==0x23)            { (*pp)+=2; return V(0); }            // AcquireOp
        if (ext==0x27)            { eval(pp,end,ctx); return VOID; }    // ReleaseOp
        if (ext==0x12)            { eval(pp,end,ctx); return VOID; }    // CondRefOf
        return VOID;
    }

    // Name reference or method call
    if (is_name_start(op)) {
        char name[AML_NAME_MAX]; parse_path(pp, name);
        aml_obj_t *obj=ns_find(ctx->scope, name);
        if (obj) {
            if (obj->type==AML_INT)    return V(obj->ival);
            if (obj->type==AML_FIELD)  return V(fld_read(obj));
            if (obj->type==AML_METHOD) {
                if (ctx->depth>=AML_DEPTH_MAX) return VOID;
                aml_ctx_t ch={0}; ch.depth=ctx->depth+1;
                // Method scope = parent of method's path
                strncpy(ch.scope, obj->path, AML_NAME_MAX-1);
                char *dot=strrchr(ch.scope,'.'); if(dot)*dot=0; else{ch.scope[0]='\\';ch.scope[1]=0;}
                // Consume arguments
                for (int i=0; i<obj->method.argc && *pp<end; i++) {
                    aml_val_t a=eval(pp,end,ctx); ch.args[i]=a.v;
                }
                // Execute and return result as plain value (not RET_RET to caller)
                aml_val_t r=exec_body(obj->method.body, obj->method.blen, &ch);
                return V(r.v);
            }
        }
        return VOID;
    }

    (*pp)++; // unknown opcode
    return VOID;
}

static aml_val_t exec_body(uint8_t *body, uint32_t blen, aml_ctx_t *ctx) {
    uint8_t *p=body, *end=body+blen;
    aml_val_t last=VOID;
    while (p<end) {
        last=eval(&p,end,ctx);
        if (last.t==RET_RET) return last;
    }
    return last;
}

// ============================================================================
// ACPI Device Enumeration & _CRS Evaluation
// ============================================================================

// Track which scopes are device objects during AML scan augmentation 
static void aml_scan_devices(uint8_t *start, uint8_t *end, const char *scope) {
    uint8_t *p = start;
    while (p < end) {
        uint8_t op = *p++;
        uint8_t *pe = p;

        if (op == 0x10) { // ScopeOp
            uint8_t *ps = p; uint32_t len = pkglen(&p);
            pe = ps + len; if (pe > end) pe = end;
            char rn[AML_NAME_MAX]; parse_path(&p, rn);
            char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);
            aml_scan_devices(ps, pe, fn); p = pe;
            continue;
        }

        if (op == 0x5B && p < end) {
            uint8_t ext = *p;
            // DeviceOp: ExtOp(0x5B, 0x82) 
            if (ext == 0x82 && p + 1 < end) {
                p++; // consume ext 
                uint8_t *ps = p; uint32_t len = pkglen(&p);
                pe = ps + len; if (pe > end) pe = end;
                char rn[AML_NAME_MAX]; parse_path(&p, rn);
                char fn[AML_NAME_MAX]; abs_path(scope, rn, fn);

                // This is a Device() object — scan its children for _HID, _ADR, _CRS 
                aml_scan_devices(ps, pe, fn);

                // Evaluate _HID, _ADR, _UID, _CRS for this device 
                dev_eval_hid(fn);
                dev_eval_adr(fn);
                dev_eval_uid(fn);
                dev_check_crs(fn);

                p = pe;
                continue;
            }
            // Other ext op — skip 
            p++;
            continue;
        }

        // NameOp (0x08) or method — skip, not a device 
        if (op == 0x08) {
            // NameOp: skip name + data 
            char rn[AML_NAME_MAX]; parse_path(&p, rn);
            // skip data — rough skip 
            if (p < end && *p == 0x0D) { p++; if (p < end) { uint32_t l = pkglen(&p); p += l; } }
            else if (p < end && (*p == 0x0A || *p == 0x0B || *p == 0x0C)) { p++; while (p < end && ((*p & 0x80) || p < end - 1)) { p++; if (p >= end) break; } if (p < end) p++; }
            else if (p < end && is_name_start(*p)) { }
            continue;
        }
        if (op == 0x14) { // MethodOp 
            uint8_t *ps = p; uint32_t len = pkglen(&p);
            pe = ps + len; if (pe > end) pe = end;
            char rn[AML_NAME_MAX]; parse_path(&p, rn);
            // Method is not a device, skip 
            p = pe;
            continue;
        }
    }
}

// ---- _CRS ResourceTemplate parser ----

static void crs_parse_raw_aml(uint8_t *aml, uint32_t aml_len, acpi_resource_list_t *out) {
    uint8_t *p = aml;
    uint8_t *end = aml + aml_len;
    out->count = 0;

    while (p + 1 < end && out->count < ACPI_MAX_RESOURCES) {
        uint8_t b = *p;

        // ---- Small item: bit7=0, bits6:3=tag, bits2:0=length ----
        // ACPI 6.4 §6.4.2: small item tags:
        //   0x01 = IRQ, 0x02 = DMA, 0x04 = Start Dependent, 0x05 = End Dependent,
        //   0x06 = IO Port, 0x07 = Fixed IO Port, 0x0E = Vendor Small, 0x0F = End Tag
        if ((b & 0x80) == 0) {
            int type = (b >> 3) & 0x0F;
            int len  = b & 0x07;
            uint8_t *data = p + 1;
            if (data + len > end) break;

            if (type == 0x0F) {
                // End Tag (0x79) — stop parsing
                break;
            }
            if (type == 0x01 && len >= 2) {
                // IRQ Descriptor — 2-byte IRQ mask, bits[i]=1 means IRQ i active
                uint16_t mask = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
                for (int i = 0; i < 16 && out->count < ACPI_MAX_RESOURCES; i++) {
                    if (mask & (1u << i)) {
                        acpi_resource_t *irq = &out->resources[out->count++];
                        irq->type = ACPI_RES_IRQ;
                        irq->irq  = (uint32_t)i;
                        irq->valid = 1;
                    }
                }
            }
            if (type == 0x06 && len >= 7) {
                // IO Port Descriptor (0x47):
                //   data[0]=Info, data[1-2]=AddrMin, data[3-4]=AddrMax,
                //   data[5]=Align, data[6]=Length
                acpi_resource_t *res = &out->resources[out->count++];
                res->type = ACPI_RES_IO;
                res->io_space_id = 1;
                res->base   = (uint64_t)data[1] | ((uint64_t)data[2] << 8);
                res->length = (uint64_t)data[6];
                res->valid  = 1;
            }
            if (type == 0x07 && len >= 3) {
                // Fixed IO Port Descriptor (0x4B):
                //   data[0-1]=BaseAddr, data[2]=Length
                acpi_resource_t *res = &out->resources[out->count++];
                res->type = ACPI_RES_IO;
                res->io_space_id = 1;
                res->base   = (uint64_t)data[0] | ((uint64_t)data[1] << 8);
                res->length = (uint64_t)data[2];
                res->valid  = 1;
            }
            p += 1 + len;
            continue;
        }

        // ---- Large item: bit7=1, bits6:0=tag, bytes 1-2=length (LE) ----
        // ACPI 6.4 §6.4.3 large item tags:
        //   0x01=Memory24, 0x02=GenericRegister, 0x04=VendorLarge,
        //   0x05=Memory32, 0x06=FixedMemory32,
        //   0x07=DWordAddressSpace, 0x08=WordAddressSpace,
        //   0x09=ExtendedInterrupt, 0x0A=QWordAddressSpace,
        //   0x0B=ExtendedAddressSpace
        int type = b & 0x7F;
        if (p + 3 > end) break;
        uint16_t len = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
        uint8_t *data = p + 3;   // data[0] is byte 3 of descriptor
        if (data + len > end) break;

        acpi_resource_t *res = &out->resources[out->count];

        if (type == 0x01 && len >= 9) {
            // Memory 24 Descriptor (0x81):
            //   data[0]=Info, data[1-2]=BaseMin(<<8), data[3-4]=BaseMax(<<8),
            //   data[5-6]=Align, data[7-8]=Length(<<8)
            res->type = ACPI_RES_MMIO;
            res->io_space_id = 0;
            res->base   = ((uint64_t)data[1] | ((uint64_t)data[2] << 8)) << 8;
            res->length = ((uint64_t)data[7] | ((uint64_t)data[8] << 8)) << 8;
            res->valid  = 1;
            out->count++;
        }
        else if (type == 0x05 && len >= 17) {
            // Memory 32 Descriptor (0x85):
            //   data[0]=Info, data[1-4]=BaseMin, data[5-8]=BaseMax,
            //   data[9-12]=Align, data[13-16]=Length
            res->type = ACPI_RES_MMIO;
            res->io_space_id = 0;
            res->base   = (uint64_t)data[1]  | ((uint64_t)data[2]  <<  8)
                        | ((uint64_t)data[3]  << 16) | ((uint64_t)data[4]  << 24);
            res->length = (uint64_t)data[13] | ((uint64_t)data[14] <<  8)
                        | ((uint64_t)data[15] << 16) | ((uint64_t)data[16] << 24);
            res->valid  = 1;
            out->count++;
        }
        else if (type == 0x06 && len >= 9) {
            // Fixed Memory 32 Descriptor (0x86):
            //   data[0]=Info, data[1-4]=BaseAddr, data[5-8]=Length
            res->type = ACPI_RES_MMIO;
            res->io_space_id = 0;
            res->base   = (uint64_t)data[1] | ((uint64_t)data[2] <<  8)
                        | ((uint64_t)data[3] << 16) | ((uint64_t)data[4] << 24);
            res->length = (uint64_t)data[5] | ((uint64_t)data[6] <<  8)
                        | ((uint64_t)data[7] << 16) | ((uint64_t)data[8] << 24);
            res->valid  = 1;
            out->count++;
        }
        else if (type == 0x07 && len >= 23) {
            // DWord Address Space Descriptor (0x87) — most common for USB MMIO:
            //   data[0]=ResourceType(0=Mem,1=IO), data[1]=GenFlags, data[2]=TypeFlags,
            //   data[3-6]=Granularity, data[7-10]=AddrMin, data[11-14]=AddrMax,
            //   data[15-18]=TranslationOffset, data[19-22]=AddrLength
            uint8_t atype = data[0];
            if (atype == 0) {
                res->type = ACPI_RES_MMIO;
                res->io_space_id = 0;
                res->base   = (uint64_t)data[7]  | ((uint64_t)data[8]  <<  8)
                            | ((uint64_t)data[9]  << 16) | ((uint64_t)data[10] << 24);
                res->length = (uint64_t)data[19] | ((uint64_t)data[20] <<  8)
                            | ((uint64_t)data[21] << 16) | ((uint64_t)data[22] << 24);
                res->valid  = 1;
                out->count++;
            } else if (atype == 1) {
                res->type = ACPI_RES_IO;
                res->io_space_id = 1;
                res->base   = (uint64_t)data[7]  | ((uint64_t)data[8]  <<  8)
                            | ((uint64_t)data[9]  << 16) | ((uint64_t)data[10] << 24);
                res->length = (uint64_t)data[19] | ((uint64_t)data[20] <<  8)
                            | ((uint64_t)data[21] << 16) | ((uint64_t)data[22] << 24);
                res->valid  = 1;
                out->count++;
            } else if (atype == 3 && out->count < ACPI_MAX_RESOURCES) {
                // Interrupt via DWord Address Space (rare — prefer Extended Interrupt below)
                acpi_resource_t *irq = &out->resources[out->count++];
                irq->type = ACPI_RES_IRQ;
                irq->irq  = (uint64_t)data[7]  | ((uint64_t)data[8]  <<  8)
                           | ((uint64_t)data[9]  << 16) | ((uint64_t)data[10] << 24);
                irq->valid = 1;
            }
        }
        else if (type == 0x09 && len >= 6) {
            // Extended Interrupt Descriptor (0x89):
            //   data[0]=Flags, data[1]=InterruptCount,
            //   data[2+i*4 .. 5+i*4] = IRQ[i] (32-bit LE each)
            uint8_t count = data[1];
            for (int i = 0; i < count && out->count < ACPI_MAX_RESOURCES; i++) {
                if ((uint16_t)(2 + (uint16_t)(i + 1) * 4) > len) break;
                acpi_resource_t *irq = &out->resources[out->count++];
                irq->type = ACPI_RES_IRQ;
                irq->irq  = (uint32_t)data[2 + i*4]        | ((uint32_t)data[3 + i*4] <<  8)
                           | ((uint32_t)data[4 + i*4] << 16) | ((uint32_t)data[5 + i*4] << 24);
                irq->valid = 1;
            }
        }
        else if (type == 0x0A && len >= 43) {
            // QWord Address Space Descriptor (0x8A) — for 64-bit MMIO:
            //   data[0]=ResourceType, data[1]=GenFlags, data[2]=TypeFlags,
            //   data[3-10]=Granularity, data[11-18]=AddrMin, data[19-26]=AddrMax,
            //   data[27-34]=TranslationOffset, data[35-42]=AddrLength
            if (data[0] == 0) {
                res->type = ACPI_RES_MMIO;
                res->io_space_id = 0;
                res->base =
                    (uint64_t)data[11] | ((uint64_t)data[12] <<  8) |
                    ((uint64_t)data[13] << 16) | ((uint64_t)data[14] << 24) |
                    ((uint64_t)data[15] << 32) | ((uint64_t)data[16] << 40) |
                    ((uint64_t)data[17] << 48) | ((uint64_t)data[18] << 56);
                res->length =
                    (uint64_t)data[35] | ((uint64_t)data[36] <<  8) |
                    ((uint64_t)data[37] << 16) | ((uint64_t)data[38] << 24) |
                    ((uint64_t)data[39] << 32) | ((uint64_t)data[40] << 40) |
                    ((uint64_t)data[41] << 48) | ((uint64_t)data[42] << 56);
                res->valid  = 1;
                out->count++;
            }
        }
        // else: skip unsupported large item types (0x02 GenReg, 0x04 VendorLarge,
        //        0x08 WordAddressSpace, 0x0B ExtendedAddressSpace)

        p += 3 + len;
    }
}

void evaluate_acpi_crs(const char *device_path, acpi_resource_list_t *out) {
    memset(out, 0, sizeof(acpi_resource_list_t));

    if (!device_path) return;

    // Build path to _CRS 
    char crs_path[AML_NAME_MAX];
    fmt_path(crs_path, AML_NAME_MAX, device_path, "._CRS");

    aml_obj_t *crs_obj = ns_exact(crs_path);
    if (!crs_obj) return;

    if (crs_obj->type == AML_BUFFER) {
        // Name(_CRS, Buffer{...}) — parse the raw buffer data directly 
        crs_parse_raw_aml(crs_obj->buffer.data, crs_obj->buffer.len, out);
        return;
    }

    if (crs_obj->type == AML_INT) {
        return;
    }

    if (crs_obj->type == AML_METHOD) {
        // Execute _CRS method 
        uint64_t irq;
        spin_lock_irqsave(&acpi_lock, &irq);
        aml_ctx_t ctx = {0};
        strncpy(ctx.scope, crs_obj->path, AML_NAME_MAX - 1);
        char *dot = strrchr(ctx.scope, '.');
        if (dot) *dot = 0; else { ctx.scope[0] = '\\'; ctx.scope[1] = 0; }
        aml_val_t result = exec_body(crs_obj->method.body, crs_obj->method.blen, &ctx);
        spin_unlock_irqrestore(&acpi_lock, irq);
        (void)result;
        uint8_t *p = crs_obj->method.body;
        uint8_t *end = crs_obj->method.body + crs_obj->method.blen;
        while (p < end - 3) {
            // Look for Buffer opcode (0x11) 
            if (*p == 0x11) {
                p++; // skip opcode 
                // skip size operand 
                uint8_t sz_op = *p;
                if (sz_op == 0x0A) { p++; if (p < end) p++; } // OneByteData 
                else if (sz_op == 0x0B) { p++; if (p+1 < end) p += 2; } // TwoByteData 
                else if (sz_op == 0x0C) { p++; if (p+3 < end) p += 4; } // DWordData 
                else if (sz_op == 0x0E) { p++; if (p+7 < end) p += 8; }
                else if ((sz_op & 0xC0) == 0x40) { p += 1; } // BytePrefix inline length 
                // Now at byte data 
                if (p < end) {
                    uint32_t buf_len = 0;
                    if (*(p-1) == 0x40 || (*(p-1) & 0xC0) == 0x40) {
                        buf_len = (*(p-1)) & 0x3F;
                    }
                    if (buf_len > 0 && p + buf_len <= end) {
                        // This buffer contains the ResourceTemplate 
                        crs_parse_raw_aml(p, buf_len, out);
                        if (out->count > 0) return;
                        p += buf_len;
                    }
                }
                continue;
            }
            p++;
        }
    }
}

// Public: enumerate all ACPI devices from DSDT + SSDTs 
void enumerate_acpi_devices(void) {
    if (!fadt || !acpi_root) return;
    acpi_dev_count = 0;
    memset(&acpi_devices, 0, sizeof(acpi_devices));

    uint64_t da = (fadt->header.revision >= 2 && fadt->x_dsdt) ? fadt->x_dsdt : fadt->dsdt;
    struct acpi_header *dsdt = (struct acpi_header*)(da + hhdm_offset);
    aml_scan_devices((uint8_t*)dsdt + sizeof(struct acpi_header),
                     (uint8_t*)dsdt + dsdt->length, "\\");

    int xsdt = !memcmp(acpi_root->signature, "XSDT", 4);
    size_t entsz = xsdt ? 8 : 4;
    int n = (acpi_root->length - sizeof(struct acpi_header)) / entsz;
    uint8_t *p = (uint8_t*)acpi_root + sizeof(struct acpi_header);
    for (int i = 0; i < n; i++) {
        uint64_t phys = xsdt ? ((uint64_t*)p)[i] : ((uint32_t*)p)[i];
        struct acpi_header *h = (struct acpi_header*)(phys + hhdm_offset);
        if (memcmp(h->signature, "SSDT", 4) == 0) {
            aml_scan_devices((uint8_t*)h + sizeof(struct acpi_header),
                             (uint8_t*)h + h->length, "\\");
        }
    }

    printf("ACPI: Enumerated %d devices.\n", acpi_dev_count);
}
 
const acpi_device_t* find_acpi_pci_device(uint8_t bus, uint8_t dev, uint8_t func) {
    // Try standard encoding: _ADR = (dev << 16) | func 
    uint64_t adr_std = ((uint64_t)dev << 16) | func;
    // Try extended: _ADR = (bus << 24) | (dev << 16) | func 
    uint64_t adr_ext = ((uint64_t)bus << 24) | ((uint64_t)dev << 16) | func;

    for (int i = 0; i < acpi_dev_count; i++) {
        acpi_device_t *d = &acpi_devices.devices[i];
        if (!d->has_adr) continue;
        if (d->adr == adr_std || d->adr == adr_ext) return d;
    }
    return NULL;
}

int get_acpi_device_count(void) {
    return acpi_dev_count;
}

const acpi_device_registry_t* get_acpi_devices(void) {
    return &acpi_devices;
}

int get_acpi_usb_resources(uint8_t bus, uint8_t dev, uint8_t func,
                           acpi_resource_list_t *out) {
    memset(out, 0, sizeof(acpi_resource_list_t));

    const acpi_device_t *acpi_dev = find_acpi_pci_device(bus, dev, func);
    if (!acpi_dev) {
        // Try matching by common USB controller HID patterns in path 
        for (int i = 0; i < acpi_dev_count; i++) {
            acpi_device_t *d = &acpi_devices.devices[i];
            if (!d->has_hid) continue;
            // Match common USB controller HIDs 
            if (strncmp(d->hid_str, "PNP0A03", AML_NAME_MAX) == 0 ||  // PCI host bridge 
                strncmp(d->hid_str, "PNP0D10", AML_NAME_MAX) == 0 ||  // USB xHCI 
                strncmp(d->hid_str, "PNP0D11", AML_NAME_MAX) == 0) {   // USB EHCI 
                // Check if path contains common USB controller name segments 
                if (strstr(d->path, "XHC") || strstr(d->path, "EHC") ||
                    strstr(d->path, "UHC") || strstr(d->path, "OHC") ||
                    strstr(d->path, "USB")) {
                    acpi_dev = d;
                    break;
                }
            }
            // Also match by path convention even without HID 
            if (strstr(d->path, "XHC") || strstr(d->path, "EHC") ||
                strstr(d->path, "UHC") || strstr(d->path, "OHC")) {
                if (strstr(d->path, "PCI0") || strstr(d->path, "PCI1") ||
                    strstr(d->path, "SB__") || strstr(d->path, "_SB_")) {
                    acpi_dev = d;
                    break;
                }
            }
        }
        if (!acpi_dev) return 0;
    }

    if (!acpi_dev->has_crs) return 0;

    evaluate_acpi_crs(acpi_dev->path, out);
    return (out->count > 0) ? 1 : 0;
}

static void init_aml(void) {
    ns_n=0;
    if (!fadt || !acpi_root) return;

    // 1. Scan DSDT
    uint64_t da=(fadt->header.revision>=2&&fadt->x_dsdt)?fadt->x_dsdt:fadt->dsdt;
    struct acpi_header *dsdt=(struct acpi_header*)(da+hhdm_offset);
    aml_scan((uint8_t*)dsdt+sizeof(struct acpi_header), (uint8_t*)dsdt+dsdt->length, "\\");

    // 2. Scan all SSDTs
    int xsdt = !memcmp(acpi_root->signature, "XSDT", 4);
    size_t entsz = xsdt ? 8 : 4;
    int n = (acpi_root->length - sizeof(struct acpi_header)) / entsz;
    uint8_t* p = (uint8_t*)acpi_root + sizeof(struct acpi_header);
    for (int i = 0; i < n; i++) {
        uint64_t phys = xsdt ? ((uint64_t*)p)[i] : ((uint32_t*)p)[i];
        struct acpi_header* h = (struct acpi_header*)(phys + hhdm_offset);
        if (!memcmp(h->signature, "SSDT", 4)) {
            aml_scan((uint8_t*)h+sizeof(struct acpi_header), (uint8_t*)h+h->length, "\\");
        }
    }
}

// Helper: call a named 1-arg method.
static void call_method1(const char *name, const char *scope, uint64_t arg) {
    aml_obj_t *m = ns_find(scope, name);
    if (!m || m->type != AML_METHOD) return;
    aml_ctx_t ctx = {0};
    ctx.args[0] = arg;
    strncpy(ctx.scope, m->path, AML_NAME_MAX-1);
    char *dot = strrchr(ctx.scope, '.');
    if (dot) *dot = 0; else { ctx.scope[0]='\\'; ctx.scope[1]=0; }
    exec_body(m->method.body, m->method.blen, &ctx);
}

static int aml_exec_pts(uint8_t slp_state) {
    uint64_t irq;
    spin_lock_irqsave(&acpi_lock, &irq);

    // Step 1: PS1S=1, PS1E=1 via SPTS or raw IO from SCIO
    aml_obj_t *spts = ns_find("\\", "SPTS");
    if (spts && spts->type == AML_METHOD) {
        call_method1("SPTS", "\\", slp_state);
    } else {
        aml_obj_t *scio = ns_find("\\", "SCIO");
        if (scio && scio->type == AML_INT) {
            uint16_t b = (uint16_t)scio->ival;
            outb(b + 1, inb(b + 1) | 0x80);
            outb(b + 5, inb(b + 5) | 0x80);
        }
    }

    // Step 2: Write SLPN directly via field I/O
    aml_obj_t *slpn = ns_find("\\", "SLPN");
    if (slpn && slpn->type == AML_FIELD) fld_write(slpn, slp_state);

    // Step 3: Write SLPT directly.
    // ECMS is ByteAcc - read RAMB as 4 separate inb() calls, inl() returns garbage.
    aml_obj_t *ramb_fld = ns_find("\\", "RAMB");
    aml_obj_t *slpt_fld = ns_find("\\", "SLPT");
    uint32_t ramb_val = 0;
    if (ramb_fld && ramb_fld->type == AML_FIELD) {
        aml_obj_t *ecms = ns_exact(ramb_fld->field.rgn);
        if (ecms && ecms->type == AML_REGION && ecms->region.space == 1) {
            uint32_t rp = (uint32_t)(ecms->region.base + ramb_fld->field.bit_off / 8);
            ramb_val = (uint32_t)inb((uint16_t)(rp+0))
                     | ((uint32_t)inb((uint16_t)(rp+1)) << 8)
                     | ((uint32_t)inb((uint16_t)(rp+2)) << 16)
                     | ((uint32_t)inb((uint16_t)(rp+3)) << 24);
        }
    }
    if (ramb_val && ramb_val != 0xFFFFFFFFu && slpt_fld && slpt_fld->type == AML_FIELD) {
        uint32_t slpt_off = slpt_fld->field.bit_off / 8;
        volatile uint8_t *sp = (volatile uint8_t*)((uintptr_t)ramb_val + hhdm_offset + slpt_off);
        *sp = slp_state;
    }

    // DEBUG: print readbacks then halt BEFORE SMI fires so LCD stays on.
    // Remove halt() once SLPN=5, SLPT=5, RAMB looks like a valid address.
    {
        uint8_t slpn_rb = slpn ? (uint8_t)fld_read(slpn) : 0xFF;
        uint8_t slpt_rb = 0xFF;
        if (ramb_val && ramb_val != 0xFFFFFFFFu && slpt_fld && slpt_fld->type == AML_FIELD)
            slpt_rb = *(volatile uint8_t*)((uintptr_t)ramb_val + hhdm_offset + slpt_fld->field.bit_off/8);
        aml_obj_t *scio2 = ns_find("\\", "SCIO");
    }

    // Step 4: OEMS fires ISMI(0x9D) - SMI reads SLPN/SLPT to prepare chipset
    aml_obj_t *oems = ns_find("\\", "OEMS");
    if (!oems || oems->type != AML_METHOD) { spin_unlock_irqrestore(&acpi_lock, irq); return 0; }
    call_method1("OEMS", "\\", slp_state);
    spin_unlock_irqrestore(&acpi_lock, irq);
    return 1;
}

void* find_acpi_table(const char* sig) {
    if (!acpi_root) return NULL;
    int xsdt = !memcmp(acpi_root->signature, "XSDT", 4);
    size_t entsz = xsdt ? 8 : 4;
    int n = (acpi_root->length - sizeof(struct acpi_header)) / entsz;
    uint8_t* p = (uint8_t*)acpi_root + sizeof(struct acpi_header);
    for (int i = 0; i < n; i++) {
        uint64_t phys = xsdt ? ((uint64_t*)p)[i] : ((uint32_t*)p)[i];
        struct acpi_header* h = (struct acpi_header*)(phys + hhdm_offset);
        if (!memcmp(h->signature, sig, 4)) return h;
    }
    return NULL;
}

static uint32_t aml_read_int_s5(uint8_t** p) {
    uint8_t op = *(*p)++;
    switch (op) {
        case 0x00: return 0;
        case 0x01: return 1;
        case 0xFF: return 0xFFFFFFFF;
        case 0x0A: return *(*p)++;
        case 0x0B: { uint16_t r = *(uint16_t*)*p; *p += 2; return r; }
        case 0x0C: { uint32_t r = *(uint32_t*)*p; *p += 4; return r; }
        default:   return 0;
    }
}

static uint32_t aml_pkg_len_s5(uint8_t** p) {
    uint8_t lead = *(*p)++;
    uint32_t len = lead & 0x3F;
    uint32_t follow = lead >> 6;
    for (uint32_t i = 0; i < follow; i++) len |= (*(*p)++) << (4 + i * 8);
    return len;
}

static void scan_table_for_s5(struct acpi_header* h) {
    if (slp_typa != 0xFFFF) return;
    uint8_t* aml = (uint8_t*)h + sizeof(struct acpi_header);
    uint32_t len = h->length - sizeof(struct acpi_header);
    if (len < 8) return;
    for (uint32_t i = 0; i < len - 7; i++) {
        uint8_t* s5_ptr = NULL;
        if (aml[i] == 0x08 && memcmp(&aml[i+1], "_S5_", 4) == 0)
            s5_ptr = &aml[i+5];
        else if (aml[i] == 0x08 && aml[i+1] == 0x5C && memcmp(&aml[i+2], "_S5_", 4) == 0)
            s5_ptr = &aml[i+6];
        if (s5_ptr && *s5_ptr == 0x12) {
            s5_ptr++;
            aml_pkg_len_s5(&s5_ptr);
            uint8_t count = *s5_ptr++;
            slp_typa = aml_read_int_s5(&s5_ptr) & 0x7;
            slp_typb = (count > 1) ? (aml_read_int_s5(&s5_ptr) & 0x7) : slp_typa;
            return;
        }
    }
}

static void parse_s5(void) {
    if (!acpi_root) return;
    // 1. Scan DSDT
    uint64_t addr = (fadt->header.revision >= 2 && fadt->x_dsdt) ? fadt->x_dsdt : fadt->dsdt;
    scan_table_for_s5((struct acpi_header*)(addr + hhdm_offset));
    if (slp_typa != 0xFFFF) return;

    // 2. Scan all SSDTs
    int xsdt = !memcmp(acpi_root->signature, "XSDT", 4);
    size_t entsz = xsdt ? 8 : 4;
    int n = (acpi_root->length - sizeof(struct acpi_header)) / entsz;
    uint8_t* p = (uint8_t*)acpi_root + sizeof(struct acpi_header);
    for (int i = 0; i < n; i++) {
        uint64_t phys = xsdt ? ((uint64_t*)p)[i] : ((uint32_t*)p)[i];
        struct acpi_header* h = (struct acpi_header*)(phys + hhdm_offset);
        if (!memcmp(h->signature, "SSDT", 4)) {
            scan_table_for_s5(h);
            if (slp_typa != 0xFFFF) return;
        }
    }
}

void init_acpi(void) {
    if (!rsdp_req.response || !rsdp_req.response->address) return;
    struct rsdp_descriptor* rsdp = (struct rsdp_descriptor*)rsdp_req.response->address;
    struct acpi_header* root = (rsdp->revision >= 2 && rsdp->xsdt_address)
        ? (struct acpi_header*)(rsdp->xsdt_address + hhdm_offset)
        : (struct acpi_header*)((uint64_t)rsdp->rsdt_address + hhdm_offset);
    acpi_root = root;
    fadt = (struct fadt_descriptor*)find_acpi_table("FACP");

    if (!fadt) return;
    if (fadt->smi_cmd && fadt->acpi_enable) {
        outb(fadt->smi_cmd, fadt->acpi_enable);
        struct acpi_gas pm1a = {
            .address_space_id   = 1,
            .register_bit_width = 16,
            .address            = fadt->pm1a_cnt_blk
        };
        for (int i = 0; i < 300; i++) {
            if (read_acpi(&pm1a) & SCI_EN) break;
            for (volatile int d = 0; d < 10000; d++) io_wait(); // Use IO wait because HPET isn't available right now.
        }
    }
    parse_s5();
    init_aml();
    enumerate_acpi_devices();
    printf("ACPI: Initialized ACPI.\n");
}

void poweroff(void) {
    if (!fadt) halt();
    cli();

    uint8_t sleep_val = (uint8_t)slp_typa; 
    if (sleep_val == 0xFF) sleep_val = 7;
    uint8_t sleep_val_b = (slp_typb == 0 || slp_typb == 0xFFFF) ? sleep_val : (uint8_t)slp_typb;

    struct acpi_gas pm1a = { .address_space_id = 1, .register_bit_width = 16, .address = fadt->pm1a_cnt_blk };
    struct acpi_gas pm1b = { .address_space_id = 1, .register_bit_width = 16, .address = fadt->pm1b_cnt_blk };
    if (fadt->header.revision >= 2 && fadt->x_pm1a_cnt_blk.address) pm1a = fadt->x_pm1a_cnt_blk;
    if (fadt->header.revision >= 2 && fadt->x_pm1b_cnt_blk.address) pm1b = fadt->x_pm1b_cnt_blk;

    // 1. Call \_PTS(5) — ACPI spec requires this before entering S5
    aml_obj_t *pts = ns_find("\\", "_PTS");
    if (pts && pts->type == AML_METHOD) {
        call_method1("_PTS", "\\", 5);
    }

    // 2. Platform-specific prep (Apple/NVIDIA SPTS/SLPN/SLPT/OEMS)
    if (ns_find("\\", "SPTS") || ns_find("\\", "SLPN") || ns_find("\\", "RAMB")) {
        aml_exec_pts(sleep_val); 
    }

    // 3. Clear wake status
    if (fadt->pm1a_evt_blk) outw(fadt->pm1a_evt_blk, 0xFFFF);
    if (fadt->pm1b_evt_blk) outw(fadt->pm1b_evt_blk, 0xFFFF);

    // 4. Read-modify-write PM1_CNT: preserve all bits except SLP_TYP and SLP_EN
    uint16_t cur_a = (uint16_t)read_acpi(&pm1a);
    uint16_t val_a = (cur_a & ~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN))
                   | ((uint16_t)sleep_val << 10) | PM1_CNT_SLP_EN;

    if (pm1b.address) {
        uint16_t cur_b = (uint16_t)read_acpi(&pm1b);
        uint16_t val_b = (cur_b & ~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN))
                       | ((uint16_t)sleep_val_b << 10) | PM1_CNT_SLP_EN;
        write_acpi(&pm1b, val_b);
    }
    write_acpi(&pm1a, val_a);

    // Retry loop in case the first write doesn't take
    while(1) {
        write_acpi(&pm1a, val_a);
    }
}

void reboot(void) {
    if (fadt && fadt->header.revision >= 2 && fadt->reset_reg.address) {
        write_acpi(&fadt->reset_reg, fadt->reset_value);
        for (volatile int d = 0; d < 100000; d++) asm volatile("nop");
    }
    while (inb(0x64) & 0x02);
    outb(0x64, 0xFE);
    halt();
}