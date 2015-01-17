#pragma once

#include <edupfm.h>
#include <eduutil.h>

#include <cstring>
#include <iostream>
#include <map>

namespace edu {
    namespace mem {
        using namespace std;

        enum MemorySpace {
            MemorySpace_Host = 0, MemorySpace_Device = 1, MemorySpace_Unknown = 2, __MemorySpace_N = 3
        };
        ostream &operator<<(ostream &out, MemorySpace space) {
            switch(space) {
            case MemorySpace_Host: return out << "Host";
            case MemorySpace_Device: return out << "Device";
            case MemorySpace_Unknown: return out << "Unknown";
            default: return out << "INVALID";
            }
        }

        MemorySpace curr_space = MemorySpace_Host;

        struct AddressRange {
            size_t start;
            size_t end; //exclusive

            AddressRange(const void *ptr, size_t len) {
                start = size_t(ptr);
                end = start + len;
            }

            friend bool operator<(const AddressRange &a, const AddressRange &b) {
                return a.end <= b.start;
            }
        };

        struct Buffer {
            void *addr;
            size_t len;
            MemorySpace space;

            static Buffer alloc(MemorySpace space, unsigned len) {
                Buffer buf;
                buf.addr = pfm::alloc(len);
                edu_errif(buf.addr == nullptr);
                buf.len = len;
                buf.space = space;
                return buf;
            }

            static Buffer get_universe() {
                return {nullptr, ~size_t(0), MemorySpace_Unknown};
            }

            static Buffer get_uninitialized() {
                return {nullptr, 0, MemorySpace_Unknown};
            }

            void dealloc() {
                edu_errif(!pfm::dealloc(addr, len));
            }

            void activate() {
                edu_errif(!pfm::set_mem_access(addr, len, pfm::MemAccess_ReadWrite));
            }

            void deactivate() {
                edu_errif(!pfm::set_mem_access(addr, len, pfm::MemAccess_None));
            }

            bool is_valid(const void *addr, unsigned len) {
                return (addr >= this->addr)
                    && ((const char*)addr + len) <= ((const char*)this->addr + this->len);
            }

            bool is_valid_offset(void *ptr, signed offset) {
                return ((char*)ptr + offset >= (char*)addr)
                    && ((char*)ptr + offset < (char*)addr + len);
            }
        };

        typedef map<AddressRange, Buffer> BufferMap;
        BufferMap spaces[__MemorySpace_N];

        void warn_new() {
            edu_warn("Unknown memory location, did you use new or shared_ptr/unique_ptr? Please use malloc() or cudaMallocHost() so that errors can be more easily detected by cuda-edu.");
        }

        bool find_buf(const void *addr, Buffer *buf, BufferMap **bufmap = nullptr) {
#define __tryget(space) {                                               \
                auto it = spaces[space].find({addr,1});                 \
                if(it != spaces[space].end()) {                         \
                    if(bufmap) *bufmap = &spaces[space];                \
                    *buf = it->second;                                  \
                    return true;                                        \
                }                                                       \
            }

            __tryget(MemorySpace_Host);
            __tryget(MemorySpace_Device);
#undef __tryget
            return false;
        }

        void activate_space(MemorySpace space) {
            BufferMap &bufs = spaces[space];
            for(auto &kv: bufs) {
                Buffer &buf = kv.second;
                buf.activate();
            }
        }

        void deactivate_space(MemorySpace space) {
            BufferMap &bufs = spaces[space];
            for(auto &kv: bufs) {
                Buffer &buf = kv.second;
                buf.deactivate();
            }
        }

        void set_space(MemorySpace space) {
            activate_space(space);
            deactivate_space(MemorySpace((space + 1) % __MemorySpace_N));
            curr_space = space;
        }

        void *alloc(MemorySpace space, unsigned len) {
            Buffer buf = Buffer::alloc(space, len);
            if(space == curr_space) {
                buf.activate();
            } else {
                buf.deactivate();
            }
            spaces[space][{buf.addr,buf.len}] = buf;
            return buf.addr;
        }

        void dealloc(MemorySpace space, void *addr) {
            Buffer buf;
            BufferMap *bufmap;
            if(!find_buf(addr, &buf, &bufmap)) {
                edu_err("Invalid buffer.");
            }

            if(space != buf.space) {
                edu_err("Requested to free memory in " << space << ", but provided address in " << buf.space);
            }

            bufmap->erase({buf.addr, buf.len});
            buf.dealloc();
        }

        void copy(MemorySpace dst_space, void *dst,
                  MemorySpace src_space, const void *src,
                  unsigned len) {

            Buffer dst_buf;
            Buffer src_buf;

#define __acquire(BUF, SPACE, PTR, DIR) {                               \
                if(!find_buf(PTR, &BUF)) {                              \
                    if(SPACE == MemorySpace_Host) {                     \
                        warn_new();                                     \
                        BUF.addr = nullptr;                             \
                    } else {                                            \
                        edu_err("Invalid Device buffer specified.");    \
                    }                                                   \
                } else {                                                \
                    if(BUF.space != SPACE) {                            \
                        edu_err("Attempting to copy " << DIR << " " << SPACE \
                                << " but provided address in " << BUF.space); \
                    }                                                   \
                    if(BUF.space != curr_space) {                       \
                        BUF.activate();                                 \
                    }                                                   \
                    if(!BUF.is_valid(PTR, len)) {                       \
                        edu_err("Invalid '" << DIR << "' address or bounds."); \
                    }                                                   \
                }                                                       \
            }

#define __release(BUF) {                        \
                if(BUF.addr && (BUF.space != curr_space)) { \
                    BUF.deactivate();           \
                }                               \
            }

            __acquire(dst_buf, dst_space, dst, "to");
            __acquire(src_buf, src_space, src, "from");

            memcpy(dst, src, len);

            __release(dst_buf);
            __release(src_buf);

#undef __acquire
#undef __release
        }
    }
}
