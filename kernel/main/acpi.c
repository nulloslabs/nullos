#include <stddef.h>
#include <main/limine_req.h>
#include <io/io.h>
#include <main/string.h>
#include <main/acpi.h>
#include <main/madt.h>
#include <io/hpet.h>
#include <main/halt.h>
#include <main/spinlocks.h>
#include <io/terminal.h>
#include <mm/mm.h>

/*
  I do not know what I did here.
  I refused to port uACPI.
  Welp, was it worth it? One word: Yes.
 */

struct fadt_descriptor* fadt = NULL;
struct acpi_header* acpi_root = NULL;

static aml_obj_t ns[AML_NS_MAX];
static int ns_n = 0;

static acpi_device_registry_t acpi_devices = {0};
static int acpi_dev_count = 0;

spinlock_t acpi_lock = SPINLOCK_INIT;

uint32_t read_acpi(struct acpi_gas *gas) {
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

void write_acpi(struct acpi_gas *gas, uint32_t val) {
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

static void nameseg(uint8_t **p, char out[5]) { memcpy(out, *p, 4); out[4] = 0; *p += 4; }

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

aml_obj_t *ns_exact(const char *path) {
    for (int i=0; i<ns_n; i++)
        if (strcmp(ns[i].path, path)==0) return &ns[i];
    return NULL;
}

aml_obj_t *ns_find(const char *scope, const char *relname) {
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

uint64_t fld_read(aml_obj_t *fld) {
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

void fld_write(aml_obj_t *fld, uint64_t val) {
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
    if (is_int_op(op)) return VALUE(parse_int(pp));

    // Locals / Args
    if (op>=0x60&&op<=0x67) { (*pp)++; return VALUE(ctx->locals[op-0x60]); }
    if (op>=0x68&&op<=0x6E) { (*pp)++; return VALUE(ctx->args[op-0x68]); }

    // NoOp
    if (op==0xA3) { (*pp)++; return VOID; }

    // ReturnOp
    if (op==0xA4) { (*pp)++; aml_val_t v=eval(pp,end,ctx); return RET(v.v); }

    // StoreOp: Store(src, dst)
    if (op==0x70) {
        (*pp)++;
        aml_val_t src=eval(pp,end,ctx);
        store_to(pp,end,ctx,src.v);
        return VALUE(src.v);
    }

    // CopyObjectOp: same semantics as Store for our purposes
    if (op==0x9D) {
        (*pp)++;
        aml_val_t src=eval(pp,end,ctx);
        store_to(pp,end,ctx,src.v);
        return VALUE(src.v);
    }

    // IfOp
    if (op==0xA0) {
        (*pp)++;
        uint8_t *ps=*pp; uint32_t len=pkglen(pp); uint8_t *pe=ps+len;
        if (pe>end) pe=end;
        aml_val_t cond=eval(pp,pe,ctx);
        aml_val_t result=VOID;
        if (cond.v) {
            while (*pp<pe) { result=eval(pp,pe,ctx); if(result.t==2){*pp=pe;break;} }
        }
        *pp=pe;
        // Check for trailing ElseOp
        if (*pp<end && **pp==0xA1) {
            (*pp)++;
            uint8_t *es=*pp; uint32_t el=pkglen(pp); uint8_t *ee=es+el;
            if (ee>end) ee=end;
            if (!cond.v) {
                while (*pp<ee) { result=eval(pp,ee,ctx); if(result.t==2){*pp=ee;break;} }
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
        return VALUE(res);
    }

    // SubtractOp
    if (op==0x74) {
        (*pp)++;
        aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx);
        uint64_t res=a.v-b.v; store_to(pp,end,ctx,res); return VALUE(res);
    }

    // AndOp
    if (op==0x7B) {
        (*pp)++;
        aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx);
        uint64_t res=a.v&b.v; store_to(pp,end,ctx,res); return VALUE(res);
    }

    // OrOp
    if (op==0x7D) {
        (*pp)++;
        aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx);
        uint64_t res=a.v|b.v; store_to(pp,end,ctx,res); return VALUE(res);
    }

    // LNotOp: returns 0xFFFFFFFF if operand is 0, else 0
    if (op==0x92) { (*pp)++; aml_val_t v=eval(pp,end,ctx); return VALUE(v.v?0:0xFFFFFFFFu); }

    // LAndOp, LOrOp
    if (op==0x90) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return VALUE((a.v&&b.v)?0xFFFFFFFFu:0); }
    if (op==0x91) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return VALUE((a.v||b.v)?0xFFFFFFFFu:0); }

    // Comparison ops
    if (op==0x93) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return VALUE(a.v==b.v?0xFFFFFFFFu:0); }
    if (op==0x94) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return VALUE(a.v>b.v?0xFFFFFFFFu:0); }
    if (op==0x95) { (*pp)++; aml_val_t a=eval(pp,end,ctx); aml_val_t b=eval(pp,end,ctx); return VALUE(a.v<b.v?0xFFFFFFFFu:0); }

    // NotifyOp: 2 args, no result - skip (not needed for shutdown)
    if (op==0x86) { (*pp)++; eval(pp,end,ctx); eval(pp,end,ctx); return VOID; }

    // SizeOfOp
    if (op==0x87) { (*pp)++; eval(pp,end,ctx); return VALUE(0); }

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
        if (ext==0x23)            { (*pp)+=2; return VALUE(0); }            // AcquireOp
        if (ext==0x27)            { eval(pp,end,ctx); return VOID; }    // ReleaseOp
        if (ext==0x12)            { eval(pp,end,ctx); return VOID; }    // CondRefOf
        return VOID;
    }

    // Name reference or method call
    if (is_name_start(op)) {
        char name[AML_NAME_MAX]; parse_path(pp, name);
        aml_obj_t *obj=ns_find(ctx->scope, name);
        if (obj) {
            if (obj->type==AML_INT)    return VALUE(obj->ival);
            if (obj->type==AML_FIELD)  return VALUE(fld_read(obj));
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
                aml_val_t r=exec_body(obj->method.body, obj->method.blen, &ch);
                return VALUE(r.v);
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
        if (last.t==2) return last;
    }
    return last;
}

// ============================================================================
// ACPI Device Enumeration
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

                // This is a Device() object — scan its children for _HID, _ADR
                aml_scan_devices(ps, pe, fn);

                // Evaluate _HID, _ADR, _UID for this device 
                dev_eval_hid(fn);
                dev_eval_adr(fn);
                dev_eval_uid(fn);

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
void call_method1(const char *name, const char *scope, uint64_t arg) {
    aml_obj_t *m = ns_find(scope, name);
    if (!m || m->type != AML_METHOD) return;
    aml_ctx_t ctx = {0};
    ctx.args[0] = arg;
    strncpy(ctx.scope, m->path, AML_NAME_MAX-1);
    char *dot = strrchr(ctx.scope, '.');
    if (dot) *dot = 0; else { ctx.scope[0]='\\'; ctx.scope[1]=0; }
    exec_body(m->method.body, m->method.blen, &ctx);
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
    init_aml();
    enumerate_acpi_devices();
    printf("acpi: initialized acpi\n");
}
