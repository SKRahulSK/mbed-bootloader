/*
 * PackageLicenseDeclared: Apache-2.0
 * Copyright (c) 2017 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _UNALIGNED_BLOCK_DEVICE_WRAPPER_H
#define _UNALIGNED_BLOCK_DEVICE_WRAPPER_H

/**
 * This is a class that wraps around a block device and allows for unaligned
 * operations on the underlying BD. This is useful because the packets are not
 * written in aligned mode, and this way we have a central place where the
 * block alignment happens.
 *
 * Note that this class initializes a buffer that is one page size long, and
 * that access to this buffer is not thread safe.
 */

#include "mbed.h"
#include "BlockDevice.h"

#if !defined(FRAG_BLOCK_DEVICE_DEBUG)
#define frag_debug(...) do {} while(0)
#else
#define frag_debug(...) debug(__VA_ARGS__)
#endif

enum frag_bd_error {
    BD_ERROR_NO_MEMORY          = -4002,
    BD_ERROR_NOT_INITIALIZED    = -4003,
};

class UnalignedBlockDevice {
public:

    /**
     * Wrap a block device for unaligned operations, note that you still need to initialize this class (by calling 'init')
     *
     * @param bd A block device (can be uninitialized)
     */
    UnalignedBlockDevice(BlockDevice *bd)
        : _block_device(bd), _page_size(0), _total_size(0), _page_buffer(NULL), _last_page(0xffffffff)
    {

    }

    ~UnalignedBlockDevice() {
        if (_page_buffer) free(_page_buffer);
    }

    /**
     * Initialize the block device and the wrapper, this will allocate one page of memory
     */
    int init() {
        if (_page_buffer) free(_page_buffer);

        int init_ret = _block_device->init();
        if (init_ret != 0) {
            return init_ret;
        }

        _page_size = _block_device->get_read_size();
        _total_size = _block_device->size();

        void *buffer = calloc((size_t)_page_size, 1);
        _page_buffer = static_cast<uint8_t*>(buffer);
        if (!_page_buffer) {
            return BD_ERROR_NO_MEMORY;
        }

        return BD_ERROR_OK;
    }

    int program(const void *a_buffer, bd_addr_t addr, bd_size_t size) {
        if (!_page_buffer) return BD_ERROR_NOT_INITIALIZED;

        // Q: a 'global' _page_buffer makes this code not thread-safe...
        // is this a problem? don't really wanna malloc/free in every call

        uint8_t *buffer = (uint8_t*)a_buffer;

        frag_debug("[FBDW] write addr=%lu size=%d\n", addr, size);

        // find the page
        size_t bytes_left = size;
        while (bytes_left > 0) {
            uint32_t page = addr / _page_size; // this gets auto-rounded
            uint32_t offset = addr % _page_size; // offset from the start of the _page_buffer
            uint32_t length = _page_size - offset; // number of bytes to write in this _page_buffer
            if (length > bytes_left) length = bytes_left; // don't overflow

            frag_debug("[FBDW] writing to page=%lu, offset=%lu, length=%lu\n", page, offset, length);

            int r;

            // retrieve the page first, as we don't want to overwrite the full page
            if (_last_page != page) {
                r = _block_device->read(_page_buffer, page * _page_size, _page_size);
                if (r != 0) return r;
            }

            // frag_debug("[FBDW] _page_buffer of page %d is:\n", page);
            // for (size_t ix = 0; ix < _page_size; ix++) {
                // frag_debug("%02x ", _page_buffer[ix]);
            // }
            // frag_debug("\n");

            // now memcpy to the _page_buffer
            memcpy(_page_buffer + offset, buffer, length);

            // frag_debug("_page_buffer after memcpy is:\n", page);
            // for (size_t ix = 0; ix < _page_size; ix++) {
                // frag_debug("%02x ", _page_buffer[ix]);
            // }
            // frag_debug("\n");

            // and write back
            r = _block_device->program(_page_buffer, page * _page_size, _page_size);
            if (r != 0) return r;

            // change the page
            bytes_left -= length;
            addr += length;
            buffer += length;

            _last_page = page;
        }

        return BD_ERROR_OK;
    }

    int read(void *a_buffer, bd_addr_t addr, bd_size_t size) {
        if (!_page_buffer) return BD_ERROR_NOT_INITIALIZED;

        frag_debug("[FBDW] read addr=%lu size=%d\n", addr, size);

        uint8_t *buffer = (uint8_t*)a_buffer;

        size_t bytes_left = size;
        while (bytes_left > 0) {
            uint32_t page = addr / _page_size; // this gets auto-rounded
            uint32_t offset = addr % _page_size; // offset from the start of the _page_buffer
            uint32_t length = _page_size - offset; // number of bytes to read in this _page_buffer
            if (length > bytes_left) length = bytes_left; // don't overflow

            frag_debug("[FBDW] Reading from page=%lu, offset=%lu, length=%lu\n", page, offset, length);

            if (_last_page != page) {
                int r = _block_device->read(_page_buffer, page * _page_size, _page_size);
                if (r != 0) return r;
            }

            // copy into the provided buffer
            memcpy(buffer, _page_buffer + offset, length);

            // change the page
            bytes_left -= length;
            addr += length;
            buffer += length;

            _last_page = page;
        }

        return BD_ERROR_OK;
    }

    int erase(bd_addr_t addr, bd_size_t size) {
        if (!_page_buffer) return BD_ERROR_NOT_INITIALIZED;

        frag_debug("[FBDW] erase addr=%lu size=%d\n", addr, size);

        uint32_t start_page = addr / _page_size; // this gets auto-rounded
        uint32_t end_page = (addr + size) / _page_size;

        memset(_page_buffer, 0xff, _page_size);

        for (size_t ix = start_page; ix <= end_page; ix++) {
            int r = _block_device->program(_page_buffer, ix * _page_size, _page_size);
            if (r != 0) return r;
        }

        if (_last_page >= start_page && _last_page <= end_page) {
            _last_page = 0xffffffff;
        }

        return BD_ERROR_OK;
    }

private:
    BlockDevice*    _block_device;
    bd_size_t       _page_size;
    bd_size_t       _total_size;
    uint8_t*        _page_buffer;
    uint32_t        _last_page;
};

#endif // _UNALIGNED_BLOCK_DEVICE_WRAPPER_H
