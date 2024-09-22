// Copyright (c) 2014-2024, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

// LOG: Add default constructor and switch case for nettypes

#include "checkpoints.h"
#include "common/dns_utils.h"
#include "string_tools.h"
#include "storages/portable_storage_template_helper.h"
#include "serialization/keyvalue_serialization.h"
#include <boost/system/error_code.hpp>
#include <boost/filesystem.hpp>
#include <functional>
#include <vector>

using namespace epee;

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "checkpoints"

namespace cryptonote {

// Struct for loading a checkpoint from JSON
struct t_hashline {
    uint64_t height;   // Checkpoint height
    std::string hash;  // Checkpoint hash

    BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(height)
        KV_SERIALIZE(hash)
    END_KV_SERIALIZE_MAP()
};

// Struct for loading multiple checkpoints from JSON
struct t_hash_json {
    std::vector<t_hashline> hashlines; // List of checkpoints

    BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(hashlines)
    END_KV_SERIALIZE_MAP()
};

// Constructor
checkpoints::checkpoints() = default;

bool checkpoints::add_checkpoint(uint64_t height, const std::string& hash_str, const std::string& difficulty_str) {
    crypto::hash h = crypto::null_hash;
    
    if (!epee::string_tools::hex_to_pod(hash_str, h)) {
        return false; // Failed to parse hash string
    }

    if (m_points.count(height) && m_points[height] != h) {
        return false; // Checkpoint exists with a different hash
    }
    
    m_points[height] = h;

    if (!difficulty_str.empty()) {
        try {
            difficulty_type difficulty(difficulty_str);
            
            if (m_difficulty_points.count(height) && m_difficulty_points[height] != difficulty) {
                return false; // Difficulty checkpoint exists with a different value
            }
            
            m_difficulty_points[height] = difficulty;
        } catch (...) {
            LOG_ERROR("Failed to parse difficulty checkpoint: " << difficulty_str);
            return false;
        }
    }
    
    return true;
}

bool checkpoints::is_in_checkpoint_zone(uint64_t height) const {
    return !m_points.empty() && height <= m_points.rbegin()->first;
}

bool checkpoints::check_block(uint64_t height, const crypto::hash& h, bool& is_a_checkpoint) const {
    auto it = m_points.find(height);
    is_a_checkpoint = it != m_points.end();
    
    if (!is_a_checkpoint) {
        return true;
    }

    if (it->second == h) {
        MINFO("CHECKPOINT PASSED FOR HEIGHT " << height << " " << h);
        return true;
    } else {
        MWARNING("CHECKPOINT FAILED FOR HEIGHT " << height << ". EXPECTED HASH: " << it->second << ", FETCHED HASH: " << h);
        return false;
    }
}

bool checkpoints::check_block(uint64_t height, const crypto::hash& h) const {
    bool ignored = false;
    return check_block(height, h, ignored);
}

// Determines if an alternative block is allowed
bool checkpoints::is_alternative_block_allowed(uint64_t blockchain_height, uint64_t block_height) const {
    if (block_height == 0) {
        return false;
    }

    auto it = m_points.upper_bound(blockchain_height);
    
    if (it == m_points.begin()) {
        return true; // Blockchain height is before the first checkpoint
    }

    --it;
    return it->first < block_height;
}

uint64_t checkpoints::get_max_height() const {
    return m_points.empty() ? 0 : m_points.rbegin()->first;
}

const std::map<uint64_t, crypto::hash>& checkpoints::get_points() const {
    return m_points;
}

const std::map<uint64_t, difficulty_type>& checkpoints::get_difficulty_points() const {
    return m_difficulty_points;
}

bool checkpoints::check_for_conflicts(const checkpoints& other) const {
    for (const auto& pt : other.get_points()) {
        if (m_points.count(pt.first) && m_points.at(pt.first) != pt.second) {
            return false; // Conflict in checkpoint hash
        }
    }
    return true;
}

// Initialize default checkpoints based on network type
bool checkpoints::init_default_checkpoints(network_type nettype) {
    switch (nettype) {
        case TESTNET:
            ADD_CHECKPOINT2(0, "48ca7cd3c8de5b6a4d53d2861fbdaedca141553559f9be9520068053cda8430b", "0x1");
            ADD_CHECKPOINT2(1000000, "46b690b710a07ea051bc4a6b6842ac37be691089c0f7758cfeec4d5fc0b4a258", "0x7aaad7153");
            ADD_CHECKPOINT2(1058600, "12904f6b4d9e60fd875674e07147d2c83d6716253f046af7b894c3e81da7e1bd", "0x971efd119");
            ADD_CHECKPOINT2(1450000, "87562ca6786f41556b8d5b48067303a57dc5ca77155b35199aedaeca1550f5a0", "0xa639e2930e");
            return true;
        case STAGENET:
            ADD_CHECKPOINT2(0, "76ee3cc98646292206cd3e86f74d88b4dcc1d937088645e9b0cbca84b7ce74eb", "0x1");
            ADD_CHECKPOINT2(10000, "1f8b0ce313f8b9ba9a46108bfd285c45ad7c2176871fd41c3a690d4830ce2fd5", "0x1d73ba");
            ADD_CHECKPOINT2(550000, "409f68cddd8e74b37469b41c1e61250d81c5776b42264f416d5d27c4626383ed", "0x5f3d4d03e");
            return true;
        default:
            ADD_CHECKPOINT2(1, "771fbcd656ec1464d3a02ead5e18644030007a0fc664c0a964d30922821a8148", "0x2");
            ADD_CHECKPOINT2(10, "c0e3b387e47042f72d8ccdca88071ff96bff1ac7cde09ae113dbb7ad3fe92381", "0x2a974");
            ADD_CHECKPOINT2(100, "ac3e11ca545e57c49fca2b4e8c48c03c23be047c43e471e1394528b1f9f80b2d", "0x35d14b");
            ADD_CHECKPOINT2(1000, "5acfc45acffd2b2e7345caf42fa02308c5793f15ec33946e969e829f40b03876", "0x36a0373");
            ADD_CHECKPOINT2(10000, "c758b7c81f928be3295d45e230646de8b852ec96a821eac3fea4daf3fcac0ca2", "0x60a91390");
            ADD_CHECKPOINT2(22231, "7cb10e29d67e1c069e6e11b17d30b809724255fee2f6868dc14cfc6ed44dfb25", "0x1e288793d");
            ADD_CHECKPOINT2(29556, "53c484a8ed91e4da621bb2fa88106dbde426fe90d7ef07b9c1e5127fb6f3a7f6", "0x71f64cce8");
            return true;
    }
}

bool checkpoints::load_checkpoints_from_json(const std::string &json_hashfile_fullpath) {
    boost::system::error_code errcode;
    if (!boost::filesystem::exists(json_hashfile_fullpath, errcode)) {
        LOG_PRINT_L1("Blockchain checkpoints file not found");
        return true;
    }

    LOG_PRINT_L1("Adding checkpoints from blockchain hashfile");

    uint64_t prev_max_height = get_max_height();
    LOG_PRINT_L1("Hard-coded max checkpoint height is " << prev_max_height);
    return true;
}

