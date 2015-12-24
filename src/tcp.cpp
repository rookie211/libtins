/*
 * Copyright (c) 2014, Matias Fontanini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following disclaimer
 *   in the documentation and/or other materials provided with the
 *   distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <cstring>
#include <algorithm>
#include <cassert>
#include "tcp.h"
#include "ip.h"
#include "ipv6.h"
#include "constants.h"
#include "rawpdu.h"
#include "utils.h"
#include "exceptions.h"
#include "internals.h"
#include "memory_helpers.h"

using std::find_if;
using Tins::Memory::InputMemoryStream;

namespace Tins {

const uint16_t TCP::DEFAULT_WINDOW = 32678;

TCP::TCP(uint16_t dport, uint16_t sport) 
: _tcp(), _options_size(0), _total_options_size(0) 
{
    this->dport(dport);
    this->sport(sport);
    data_offset(sizeof(tcphdr) / sizeof(uint32_t));
    window(DEFAULT_WINDOW);
}

TCP::TCP(const uint8_t *buffer, uint32_t total_sz) 
: _options_size(0), _total_options_size(0) 
{
    InputMemoryStream stream(buffer, total_sz);
    stream.read(_tcp);
    // Check that we have at least the amount of bytes we need and not less
    if (TINS_UNLIKELY(data_offset() * sizeof(uint32_t) > total_sz || 
                      data_offset() * sizeof(uint32_t) < sizeof(tcphdr))) {
        throw malformed_packet();
    }
    const uint8_t *header_end = buffer + (data_offset() * sizeof(uint32_t));

    while (stream.pointer() < header_end) {
        const OptionTypes option_type = (OptionTypes)stream.read<uint8_t>();
        if (option_type <= NOP) {
            #if TINS_IS_CXX11
            add_option(option_type, 0);
            #else
            add_option(option(option_type, 0));
            #endif // TINS_IS_CXX11
        }
        else {
            // Extract the length
            uint32_t len = stream.read<uint8_t>();
            const uint8_t *data_start = stream.pointer();

            // We need to subtract the option type and length from the size
            if (TINS_UNLIKELY(len < sizeof(uint8_t) << 1)) {
                throw malformed_packet();
            }
            len -= (sizeof(uint8_t) << 1);
            // Make sure we have enough bytes for the advertised option payload length
            if (TINS_UNLIKELY(data_start + len > header_end)) {
                throw malformed_packet(); 
            }
            // If we're using C++11, use the variadic template overload
            #if TINS_IS_CXX11
            add_option(option_type, data_start, data_start + len);
            #else
            add_option(option(option_type, data_start, data_start + len));
            #endif // TINS_IS_CXX11
            // Skip the option's payload
            stream.skip(len);
        }
    }
    // If we still have any bytes left
    if (stream) {
        inner_pdu(new RawPDU(stream.pointer(), stream.size()));
    }
}

void TCP::dport(uint16_t new_dport) {
    _tcp.dport = Endian::host_to_be(new_dport);
}

void TCP::sport(uint16_t new_sport) {
    _tcp.sport = Endian::host_to_be(new_sport);
}

void TCP::seq(uint32_t new_seq) {
    _tcp.seq = Endian::host_to_be(new_seq);
}

void TCP::ack_seq(uint32_t new_ack_seq) {
    _tcp.ack_seq = Endian::host_to_be(new_ack_seq);
}

void TCP::window(uint16_t new_window) {
    _tcp.window = Endian::host_to_be(new_window);
}

void TCP::checksum(uint16_t new_check) {
    _tcp.check = Endian::host_to_be(new_check);
}

void TCP::urg_ptr(uint16_t new_urg_ptr) {
    _tcp.urg_ptr = Endian::host_to_be(new_urg_ptr);
}

void TCP::data_offset(small_uint<4> new_doff) {
    this->_tcp.doff = new_doff;
}

void TCP::mss(uint16_t value) {
    value = Endian::host_to_be(value);
    add_option(option(MSS, 2, (uint8_t*)&value));
}

uint16_t TCP::mss() const {
    return generic_search<uint16_t>(MSS);
}

void TCP::winscale(uint8_t value) {
    add_option(option(WSCALE, 1, &value));
}

uint8_t TCP::winscale() const {
    return generic_search<uint8_t>(WSCALE);
}

void TCP::sack_permitted() {
    add_option(option(SACK_OK, 0));
}

bool TCP::has_sack_permitted() const {
    return search_option(SACK_OK) != NULL;
}

void TCP::sack(const sack_type &edges) {
    uint32_t *value = 0;
    if(edges.size()) {
        value = new uint32_t[edges.size()];
        uint32_t *ptr = value;
        for(sack_type::const_iterator it = edges.begin(); it != edges.end(); ++it)
            *(ptr++) = Endian::host_to_be(*it);
    }
    add_option(
        option(
            SACK, 
            (uint8_t)(sizeof(uint32_t) * edges.size()), 
            (const uint8_t*)value
        )
    );
    delete[] value;
}

TCP::sack_type TCP::sack() const {
    const option *opt = search_option(SACK);
    if(!opt) 
        throw option_not_found();
    return opt->to<sack_type>();
}

void TCP::timestamp(uint32_t value, uint32_t reply) {
    uint64_t buffer = (uint64_t(value) << 32) | reply;
    buffer = Endian::host_to_be(buffer);
    add_option(option(TSOPT, 8, (uint8_t*)&buffer));
}

std::pair<uint32_t, uint32_t> TCP::timestamp() const {
    const option *opt = search_option(TSOPT);
    if(!opt)
        throw option_not_found();
    return opt->to<std::pair<uint32_t, uint32_t> >();
}

void TCP::altchecksum(AltChecksums value) {
    uint8_t int_value = value;
    add_option(option(ALTCHK, 1, &int_value));
}

TCP::AltChecksums TCP::altchecksum() const {
    return static_cast<AltChecksums>(generic_search<uint8_t>(ALTCHK));
}

small_uint<1> TCP::get_flag(Flags tcp_flag) const {
    switch(tcp_flag) {
        case FIN:
            return _tcp.flags.fin;
            break;
        case SYN:
            return _tcp.flags.syn;
            break;
        case RST:
            return _tcp.flags.rst;
            break;
        case PSH:
            return _tcp.flags.psh;
            break;
        case ACK:
            return _tcp.flags.ack;
            break;
        case URG:
            return _tcp.flags.urg;
            break;
        case ECE:
            return _tcp.flags.ece;
            break;
        case CWR:
            return _tcp.flags.cwr;
            break;
        default:
            return 0;
            break;
    };
}

small_uint<12> TCP::flags() const {
    return (_tcp.res1 << 8) | _tcp.flags_8;
}

void TCP::set_flag(Flags tcp_flag, small_uint<1> value) {
    switch(tcp_flag) {
        case FIN:
            _tcp.flags.fin = value;
            break;
        case SYN:
            _tcp.flags.syn = value;
            break;
        case RST:
            _tcp.flags.rst = value;
            break;
        case PSH:
            _tcp.flags.psh = value;
            break;
        case ACK:
            _tcp.flags.ack = value;
            break;
        case URG:
            _tcp.flags.urg = value;
            break;
        case ECE:
            _tcp.flags.ece = value;
            break;
        case CWR:
            _tcp.flags.cwr = value;
            break;
    };
}

void TCP::flags(small_uint<12> value) {
    _tcp.res1 = (value >> 8) & 0x0f;
    _tcp.flags_8 = value & 0xff;
}

void TCP::add_option(const option &opt) {
    _options.push_back(opt);
    internal_add_option(opt);
}

uint32_t TCP::header_size() const {
    return sizeof(tcphdr) + _total_options_size;
}

void TCP::write_serialization(uint8_t *buffer, uint32_t total_sz, const PDU *parent) {
    assert(total_sz >= header_size());
    uint8_t *tcp_start = buffer;
    checksum(0);
    buffer += sizeof(tcphdr);
    _tcp.doff = (sizeof(tcphdr) + _total_options_size) / sizeof(uint32_t);
    for(options_type::iterator it = _options.begin(); it != _options.end(); ++it)
        buffer = write_option(*it, buffer);

    if(_options_size < _total_options_size) {
        uint16_t padding = _options_size;
        while(padding < _total_options_size) {
            *(buffer++) = 1;
            padding++;
        }
    }

    memcpy(tcp_start, &_tcp, sizeof(tcphdr));

    const Tins::IP *ip_packet = tins_cast<const Tins::IP*>(parent);
    if(ip_packet) {
        uint32_t check = Utils::pseudoheader_checksum(ip_packet->src_addr(),  
                                                         ip_packet->dst_addr(), 
                                                         size(), Constants::IP::PROTO_TCP) +
                            Utils::do_checksum(tcp_start, tcp_start + total_sz);
        while (check >> 16)
            check = (check & 0xffff) + (check >> 16);
        checksum(~check);
        ((tcphdr*)tcp_start)->check = _tcp.check;
    }
    else {
        const Tins::IPv6 *ipv6_packet = tins_cast<const Tins::IPv6*>(parent);
        if(ipv6_packet) {
            uint32_t check = Utils::pseudoheader_checksum(ipv6_packet->src_addr(),  
                                                         ipv6_packet->dst_addr(), 
                                                         size(), Constants::IP::PROTO_TCP) +
                                        Utils::do_checksum(tcp_start, tcp_start + total_sz);
            while (check >> 16)
                check = (check & 0xffff) + (check >> 16);
            checksum(~check);
            ((tcphdr*)tcp_start)->check = _tcp.check;
        }
    }
}

const TCP::option *TCP::search_option(OptionTypes type) const {
    // Search for the iterator. If we found something, return it, otherwise return nullptr.
    options_type::const_iterator iter = search_option_iterator(type);
    return (iter != _options.end()) ? &*iter : 0;
}

TCP::options_type::const_iterator TCP::search_option_iterator(OptionTypes type) const {
    Internals::option_type_equality_comparator<option> comparator(type);
    return find_if(_options.begin(), _options.end(), comparator);
}

TCP::options_type::iterator TCP::search_option_iterator(OptionTypes type) {
    Internals::option_type_equality_comparator<option> comparator(type);
    return find_if(_options.begin(), _options.end(), comparator);
}

/* options */

uint8_t *TCP::write_option(const option &opt, uint8_t *buffer) {
    if(opt.option() == 0 || opt.option() == 1) {
        *buffer = opt.option();
        return buffer + 1;
    }
    else {
        buffer[0] = opt.option();
        buffer[1] = static_cast<uint8_t>(opt.length_field());
        // only add the identifier and size field sizes if the length
        // field hasn't been spoofed.
        if(opt.length_field() == opt.data_size())
            buffer[1] += (sizeof(uint8_t) << 1);
        return std::copy(opt.data_ptr(), opt.data_ptr() + opt.data_size(), buffer + 2);
    }
}

void TCP::update_options_size() {
    uint8_t padding = _options_size & 3;
    _total_options_size = (padding) ? (_options_size - padding + 4) : _options_size;
}

void TCP::internal_add_option(const option &opt) {
    _options_size += sizeof(uint8_t);
    // SACK_OK contains length but not data....
    if(opt.data_size() || opt.option() == SACK_OK) {
        _options_size += sizeof(uint8_t);    
        _options_size += static_cast<uint16_t>(opt.data_size());
    }
    update_options_size();
}

bool TCP::remove_option(OptionTypes type) {
    options_type::iterator iter = search_option_iterator(type);
    if (iter == _options.end()) {
        return false;
    }
    _options_size -= sizeof(uint8_t);
    // SACK_OK contains length but not data....
    if(iter->data_size() || iter->option() == SACK_OK) {
        _options_size -= sizeof(uint8_t);
        _options_size -= static_cast<uint16_t>(iter->data_size());
    }
    _options.erase(iter);
    update_options_size();
    return true;
}

bool TCP::matches_response(const uint8_t *ptr, uint32_t total_sz) const {
    if(total_sz < sizeof(tcphdr))
        return false;
    const tcphdr *tcp_ptr = (const tcphdr*)ptr;
    if(tcp_ptr->sport == _tcp.dport && tcp_ptr->dport == _tcp.sport) {
        uint32_t sz = std::min<uint32_t>(total_sz, tcp_ptr->doff * sizeof(uint32_t));
        return inner_pdu() ? inner_pdu()->matches_response(ptr + sz, total_sz - sz) : true;
    }
    else
        return false;
}

}

